/**
 * @file learning_state.c
 * @brief 本地学习状态实现 (P1 错词本 + 收藏)
 *
 * 时间域：esp_timer 单调相对秒（自治钟策略对齐 standby_page：系统
 * settimeofday 路径在本机损坏，弃用）。持久化时 next_review 转存
 * "剩余到期秒"，加载时以当前相对秒重建 —— 跨重启语义保持，代价是
 * 关机期间流逝的时间被冻结（SRS 天级间隔下可忽略）；绝对时钟统一
 * 接入后在此单点切换。
 *
 * NVS 布局：命名空间 "inkword"，blob 键按卡组分派（LR04）：默认卡组
 *   "lr_state"，其余 "lr_st_<id>"（id ≤7，NVS 键名 15 上限内），
 *   每组独立进度互不覆盖；lr_stats 保持全局（跨卡组累计口径）。
 * 保存时机：评分/收藏仅置脏，主循环 maybe_save 静默 5s 后一次性写
 * （按键路径零 NVS 阻塞；掉电窗口 ≤5s 与事件队列策略一致）。
 */
#include "learning_state.h"
#include "srs_engine.h"
#include "debug_log.h"
#include "schedule.h"      /* v1.6：课程表自动激活（评分路径跨日触发） */

#include "nvs.h"
#include "settings_keys.h"   /* P2b：NVS 键权威表 */
#include "esp_timer.h"
#include "esp_heap_caps.h"   /* 状态数组 PSRAM（词库扩容 2026-08-20） */
#include "freertos/FreeRTOS.h"  /* portMUX：队列跨 task 访问的临界区 */

#include <string.h>
#include <stdlib.h>
#include <time.h>            /* 今日统计日历换算（gmtime_r，UTC 域） */

/* 自治钟公共导出（standby_page.c；今日统计日结基准。native-test 不链
 * 此符号——learning_state 无 native 测试，与现状一致） */
extern int64_t standby_time_now(void);

static const char *TAG = "LRN";

/* ---- 运行态 ---- */

typedef struct {
    SrsNode srs;               /* stability==0 表示未学（惰性初始化） */
    uint8_t consecutive_wrong; /* 连错计数，>0 即在错词本 */
    bool    collected;         /* 收藏标志 */
    bool    mastered;          /* 墨封标志（2026-09-04 Anki suspend 哲学：
                               * 调度层过滤，FSRS/收藏不动；置位方向同步
                               * 清连错——声明式通过，后端 SyncMaster
                               * 双端镜像） */
} lr_entry_t;

static lr_entry_t *s_state = NULL;  /* PSRAM（4000 词×~24B；词池扩容前为 DRAM 静态 128 词） */
static int s_count = 0;

/* 本会话已评分 bitmap（复习到期视图排除位）：apply_quality 置位，
 * 重启清零——到期词当天已评不再推，重启后重新列出（新会话重新提醒）。
 * 分配失败时置 NULL，到期视图安全退化为空（复习页显「无到期词」） */
static uint8_t *s_done = NULL;   /* s_count bits，PSRAM calloc */

static bool lr_done(int idx)
{
    return !s_done || (s_done[idx >> 3] & (uint8_t)(1u << (idx & 7)));
}

static void lr_mark_done(int idx)
{
    if (s_done) s_done[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

/* ---- 今日学习统计（2026-08-24，百词斩「今日进度」借鉴）：
 * 日期域 = 自治钟 epoch（未同步挂起日结，首个有效评分补结）；
 * 口径：新学 = 评分时 stability==0（首评）；复习 = 每次评分；
 * streak = 每日 ≥1 评分的连续天数（跨日首个评分结算昨日）。
 * NVS 独立 key "lr_stats"（随 lr_state 同窗口落盘，跨词库变化保留） ---- */
#define LR_STATS_MAGIC  0x53543031u  /* "ST01" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    int32_t  ymd;        /* 结算基准日（yyyymmdd，UTC+8）；0 = 尚无有效日期 */
    uint16_t today_new;
    uint16_t today_reviews;
    uint16_t streak_days;
} lr_stats_t;

static lr_stats_t s_stats = { LR_STATS_MAGIC, 0, 0, 0, 0 };
static bool s_stats_dirty = false;

/* 卡组短串宽度 8 = DECK_ID_MAX(7) + NUL（SD01 按组表与 LR04 头共用；
 * 定义前置供 dstats 区引用，对齐 deck_manager 短 id 约定） */
#define LR_DECK_W    (8)

/* 前向声明：epoch_ymd/civil_days 定义在本区之后（日期数学组） */
static int32_t epoch_ymd(int64_t epoch);

/* ---- 按卡组今日计数（v1.5 T5.5 科目级配额）：独立键 lr_stdeck
 * （SD01；lr_stats ST01 全局跨组口径不动——T4.2 决策保留），小表
 * 条目 {deck[8], new, rev} ×6（LRU：满 6 覆盖今日总量最小条目——
 * 最久未学组优先让位）；跨日清零与 stats_roll 同日同步 ---- */
#define LR_DSTATS_MAGIC  0x53443031u  /* "SD01" */
#define LR_DSTATS_MAX    6

typedef struct __attribute__((packed)) {
    char     deck[LR_DECK_W];   /* 与 LR04 头同宽 8B（含 NUL） */
    uint16_t new_n;
    uint16_t rev;
} lr_dentry_t;                 /* 12B */

typedef struct __attribute__((packed)) {
    uint32_t    magic;
    int32_t     ymd;             /* 结算基准日（与 lr_stats 同源 epoch） */
    uint8_t     count;           /* 有效条目数 ≤ LR_DSTATS_MAX */
    uint8_t     _pad[3];         /* 对齐一致性（重建即全量覆写） */
    lr_dentry_t e[LR_DSTATS_MAX];
} lr_dstats_t;                  /* 8 + 72 = 80B */

static lr_dstats_t s_dstats = { LR_DSTATS_MAGIC, 0, 0, {0, 0, 0},
                               {{{0}, 0, 0}} }; /* e[0] 全字段显式，余条目零填
                                                      *（数组部分初始化不触发
                                                      * -Wmissing-field-initializers）*/
static bool s_dstats_dirty = false;

/* 天数序号（Howard Hinnant days_from_civil；跨日差=1 判连续，
 * 免受月末/闰年 ymd 数值进位断链） */
static int64_t civil_days(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                  /* [0,399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0,365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0,146096] */
    return era * 146097 + doe - 719468;
}

/* epoch -> UTC+8 日历 yyyymmdd（区间外/未同步返回 0） */
static int32_t epoch_ymd(int64_t epoch)
{
    time_t t = (time_t)(epoch + 8 * 3600);
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return 0;
    return (int32_t)((tmv.tm_year + 1900) * 10000 +
                     (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
}

static void ymd_split(int32_t ymd, int *y, int *m, int *d)
{
    *y = ymd / 10000; *m = ymd % 10000 / 100; *d = ymd % 100;
}

/* 跨日结算：今日首个评分时调用——昨日有学习且连续则 streak+1，
 * 否则从今日置 1；计数清零（首日同步前学习的计数从同步起算） */
static void stats_roll(int64_t epoch)
{
    int32_t today = epoch_ymd(epoch);
    if (today == 0 || today == s_stats.ymd) return;

    if (s_stats.ymd > 0 && s_stats.today_reviews > 0) {
        int y1, m1, d1, y2, m2, d2;
        ymd_split(s_stats.ymd, &y1, &m1, &d1);
        ymd_split(today, &y2, &m2, &d2);
        if (civil_days(y2, m2, d2) - civil_days(y1, m1, d1) == 1) {
            if (s_stats.streak_days < 65535) s_stats.streak_days++;
        } else {
            s_stats.streak_days = 1;   /* 中断空档：从今日重新起算 */
        }
    } else {
        s_stats.streak_days = 1;       /* 首日 / 昨日空档 */
    }
    s_stats.ymd = today;
    s_stats.today_new = s_stats.today_reviews = 0;
    s_stats_dirty = true;
}

static void stats_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    lr_stats_t t;
    size_t len = sizeof(t);
    if (nvs_get_blob(h, NVS_KEY_LR_STATS, &t, &len) == ESP_OK &&
        len == sizeof(t) && t.magic == LR_STATS_MAGIC)
        s_stats = t;
    nvs_close(h);
}

/* 按组小表加载（坏 magic/长度不齐保守重建空表——丢当日分组数可接受，
 * 全局 lr_stats 独立键不受幸） */
static void dstats_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    lr_dstats_t t;
    size_t len = sizeof(t);
    if (nvs_get_blob(h, NVS_KEY_LR_STDECK, &t, &len) == ESP_OK &&
        len == sizeof(t) && t.magic == LR_DSTATS_MAGIC)
        s_dstats = t;
    else
        memset(&s_dstats, 0, sizeof(s_dstats));
    s_dstats.magic = LR_DSTATS_MAGIC;
    nvs_close(h);
}

/* 跨日清零（与 stats_roll 同日同步；旧条目直接丢弃——新的一天各组从 0） */
static void dstats_roll(int32_t today)
{
    if (today == 0 || today == s_dstats.ymd) return;
    s_dstats.ymd  = today;
    s_dstats.count = 0;
    memset(s_dstats.e, 0, sizeof(s_dstats.e));
    s_dstats_dirty = true;
}

/* 当前组条目下标（-1 未命中）；deck 串 8B 含 NUL 语义与 LR04 头一致 */
static int dstats_find(const char *deck_id)
{
    const char *id = deck_id ? deck_id : "";
    for (int i = 0; i < s_dstats.count; i++)
        if (strncmp(s_dstats.e[i].deck, id, LR_DECK_W) == 0) return i;
    return -1;
}

/* 取可写条目：未满 append 新条；否则覆盖今日总量（new+rev）最小的
 * 条目（最久未学组让位，与 LRU 意图等价的廉价近似） */
static int dstats_alloc(const char *deck_id)
{
    if (s_dstats.count < LR_DSTATS_MAX) {
        int i = s_dstats.count++;
        memset(&s_dstats.e[i], 0, sizeof(lr_dentry_t));
        snprintf(s_dstats.e[i].deck, LR_DECK_W, "%s", deck_id ? deck_id : "");
        return i;
    }
    int min_i = 0;
    for (int i = 1; i < LR_DSTATS_MAX; i++) {
        unsigned a = (unsigned)s_dstats.e[i].new_n + s_dstats.e[i].rev;
        unsigned b = (unsigned)s_dstats.e[min_i].new_n + s_dstats.e[min_i].rev;
        if (a < b) min_i = i;
    }
    memset(&s_dstats.e[min_i], 0, sizeof(lr_dentry_t));
    snprintf(s_dstats.e[min_i].deck, LR_DECK_W, "%s", deck_id ? deck_id : "");
    return min_i;
}

int learning_state_deck_today_new(const char *deck_id)
{
    /* 低频显示路径：现取日期（而非依赖 roll 已触发——午夜后未评分时
     * 表内还是昨日数据，返回 0 才是「今日还没学」的正确语义） */
    int64_t ep = standby_time_now();
    if (ep <= 0) return 0;
    int32_t today = epoch_ymd(ep);
    if (today == 0 || today != s_dstats.ymd) return 0;

    int i = dstats_find(deck_id);
    return i < 0 ? 0 : s_dstats.e[i].new_n;
}

/* ---- 脏标记延迟落盘：评分/收藏仅置脏（按键路径零 NVS 写阻塞），
 * 主循环 maybe_save 静默 LR_SAVE_DELAY 秒后一次性写入 ---- */
#define LR_SAVE_DELAY_SEC  (5)
static bool    s_dirty = false;
static int64_t s_dirty_at = 0;    /* 最后一次置脏时刻（连续操作刷新静默计时） */

/* ---- NVS 打包格式（变更需 bump LR_MAGIC 使旧数据整体失效）----
 * LR05（墨封 2026-09-04）：lr_packed 尾部追加 mastered（19→20B，
 *   sparse 21→22B）。唯一例外不作废旧数据：restore 兼容读 LR04
 *   （magic/长度双判别，mastered 补 false）——存量用户 2407 词进度
 *   不清零是本功能上线前提；写入一律 LR05。
 * LR04（v1.4 T4.2 卡组隔离 2026-08-24）：头加 deck 短串（8B 含 NUL），
 *   blob 键按卡组分派（默认组沿用 "lr_state"，其余 "lr_st_<id>"），
 *   切卡组=旧组保存+新组恢复，各组进度互不丢；头 deck 串与键双保险
 *   （manifest 改 id / 键错位时恢复校验兜底作废）。旧 LR03 状态
 *   保守作废重建（magic 不匹配）。
 * LR03（M4 FSRS 切换 2026-08-22）：sparse 载荷换 FSRS 字段
 *   （stability/difficulty + next/last 双 delta），SM-2 的
 *   ease/rep/interval 废弃；旧 LR02 状态保守作废重建（magic 不匹配）。
 * LR02（词库扩容 2026-08-20）：sparse —— 只存非默认态词
 *   （学过/连错>0/已收藏），适配 nvs 24KB 分区；
 * LR01 全量格式（4000 词≈76KB blob）物理写不下，升级即作废 */

#define LR_MAGIC     0x4C523035u  /* "LR05" */
#define LR_MAGIC_OLD 0x4C523034u  /* "LR04"（兼容读，见版本注释块） */

/* LR04 头：magic(4) + deck[8] + count(2) + used(2) = 16B */
#define LR_HDR       (16)
static char s_deck[LR_DECK_W] = "";   /* 当前锁定卡组（""=默认） */

/* 卡组锁定（清零后写短串，防上次长 id 残留；超长截断保 NUL） */
static void lr_set_deck(const char *deck_id)
{
    memset(s_deck, 0, sizeof(s_deck));
    snprintf(s_deck, sizeof(s_deck), "%s", deck_id ? deck_id : "");
}

/* NVS 键按卡组分派：默认组沿用 "lr_state"（兼容既有键位），其余
 * "lr_st_<id>"（6+7=13 ≤ 键名 15 上限；与 rd_* 进度键后缀同先例） */
static void lr_key(char *out, size_t cap)
{
    if (!s_deck[0]) snprintf(out, cap, NVS_KEY_LR_STATE);
    else            snprintf(out, cap, NVS_KEY_LR_ST_PFX "%s", s_deck);
}

typedef struct __attribute__((packed)) {
    float    stability;   /* FSRS 记忆稳定性 S（天）；>0 = 已学 */
    float    difficulty;  /* FSRS 难度 D ∈ [1,10] */
    int32_t  next_delta;  /* 距保存时刻的剩余到期秒（<0 存 0） */
    int32_t  last_delta;  /* 保存时刻距上次复习的秒数（elapsed 重建） */
    uint8_t  level;
    uint8_t  wrong;
    uint8_t  collected;
    uint8_t  mastered;    /* LR05 新增（尾部追加保兼容读布局） */
} lr_packed_t;  /* 20B */

typedef struct __attribute__((packed)) {
    uint16_t    idx;      /* 词库索引（升序写入，读回按此回填） */
    lr_packed_t p;
} lr_sparse_t;  /* 22B */

/* LR04 旧载荷（19B/21B）：仅 restore 兼容读解释用（见版本注释块），
 * 写入路径不再引用 */
typedef struct __attribute__((packed)) {
    float    stability;
    float    difficulty;
    int32_t  next_delta;
    int32_t  last_delta;
    uint8_t  level;
    uint8_t  wrong;
    uint8_t  collected;
} lr_packed_v4_t;  /* 19B */

typedef struct __attribute__((packed)) {
    uint16_t      idx;
    lr_packed_v4_t p;
} lr_sparse_v4_t;  /* 21B */

/* sparse 容量：nvs 已扩容 192KB（T1.5 分区表 0x6000→0x30000），
 * lr_state blob 最坏 8B 头 + 22B×4000 = 88,016B，双写余量充足；
 * 上限对齐 LEARNING_STATE_MAX（4000）= 全词库可持久化，不再丢尾。
 * 注：分区表变更仅限 USB 烧录，不可经 OTA 下发（见 csv 注释） */
#define LR_SPARSE_MAX  (4000)

static int64_t now_sec(void)
{
    return esp_timer_get_time() / 1000000LL;
}

/* ---- 上报事件环形队列（P2）：评分/收藏动作入队，
 * background_task 联网时逐条 flush（见头文件注释）；
 * 按键回调（Arduino loop task）入队 / 后台任务出队，
 * portMUX 临界区保护（操作均为常数短临界区） ---- */

#define LR_EVENT_MAX 24
static lr_event_t s_events[LR_EVENT_MAX];
static int s_ev_head = 0, s_ev_count = 0;
static portMUX_TYPE s_ev_mux = portMUX_INITIALIZER_UNLOCKED;

static void event_push(int word_idx, int quality, int collected)
{
    portENTER_CRITICAL(&s_ev_mux);
    if (s_ev_count == LR_EVENT_MAX) {           /* 满：覆盖最旧 */
        s_ev_head = (s_ev_head + 1) % LR_EVENT_MAX;
        s_ev_count--;
    }
    lr_event_t *e = &s_events[(s_ev_head + s_ev_count) % LR_EVENT_MAX];
    e->word_idx  = word_idx;
    e->quality   = quality;
    e->collected = collected != 0;
    s_ev_count++;
    portEXIT_CRITICAL(&s_ev_mux);
}

int learning_state_event_count(void)
{
    portENTER_CRITICAL(&s_ev_mux);
    int n = s_ev_count;
    portEXIT_CRITICAL(&s_ev_mux);
    return n;
}

bool learning_state_event_peek(int i, lr_event_t *out)
{
    if (!out || i < 0) return false;
    portENTER_CRITICAL(&s_ev_mux);
    bool ok = i < s_ev_count;
    if (ok) *out = s_events[(s_ev_head + i) % LR_EVENT_MAX];
    portEXIT_CRITICAL(&s_ev_mux);
    return ok;
}

void learning_state_event_drop(int n)
{
    if (n <= 0) return;
    portENTER_CRITICAL(&s_ev_mux);
    if (n > s_ev_count) n = s_ev_count;
    s_ev_head = (s_ev_head + n) % LR_EVENT_MAX;
    s_ev_count -= n;
    portEXIT_CRITICAL(&s_ev_mux);
}

/* sparse 回填：blob = header(LR_HDR) + used × lr_sparse_t，按 idx 升序 */
static void unpack(const uint8_t *buf, uint16_t used)
{
    const lr_sparse_t *sp = (const lr_sparse_t *)(buf + LR_HDR);
    int64_t now = now_sec();
    for (int i = 0; i < used; i++) {
        uint16_t idx = sp[i].idx;
        if (idx >= (uint16_t)s_count) continue;   /* 防御：异常 idx 跳过 */
        s_state[idx].srs.stability   = sp[i].p.stability;
        s_state[idx].srs.difficulty  = sp[i].p.difficulty;
        s_state[idx].srs.next_review = now + sp[i].p.next_delta;
        s_state[idx].srs.last_review = now - sp[i].p.last_delta;
        s_state[idx].srs.srs_level   = sp[i].p.level;
        s_state[idx].consecutive_wrong = sp[i].p.wrong;
        s_state[idx].collected         = sp[i].p.collected != 0;
        s_state[idx].mastered          = sp[i].p.mastered != 0;
    }
}

/* LR04 回填（兼容读，2026-09-04）：旧 21B 布局解释，mastered 保持
 * 零初始化 false；其余字段语义与新版逐位一致 */
static void unpack_v4(const uint8_t *buf, uint16_t used)
{
    const lr_sparse_v4_t *sp = (const lr_sparse_v4_t *)(buf + LR_HDR);
    int64_t now = now_sec();
    for (int i = 0; i < used; i++) {
        uint16_t idx = sp[i].idx;
        if (idx >= (uint16_t)s_count) continue;
        s_state[idx].srs.stability   = sp[i].p.stability;
        s_state[idx].srs.difficulty  = sp[i].p.difficulty;
        s_state[idx].srs.next_review = now + sp[i].p.next_delta;
        s_state[idx].srs.last_review = now - sp[i].p.last_delta;
        s_state[idx].srs.srs_level   = sp[i].p.level;
        s_state[idx].consecutive_wrong = sp[i].p.wrong;
        s_state[idx].collected         = sp[i].p.collected != 0;
    }
}

/* LR04 sparse 恢复：按 s_deck 分派键读 blob，头三重校验
 * （magic / deck 串 / 词数）+ 长度边界；不符保守丢弃（重新学习） */
static void restore_from_nvs(void)
{
    char key[16];
    lr_key(key, sizeof(key));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    /* LR05/LR04 双格式判别：header = magic(4) + deck[8] + count(2) +
     * used(2)；旧格式按 21B 条目精确验长后走 unpack_v4（兼容读，
     * mastered 补 false），见版本注释块 */
    size_t len = 0;
    if (nvs_get_blob(h, key, NULL, &len) == ESP_OK &&
        len >= LR_HDR &&
        len <= LR_HDR + (size_t)LR_SPARSE_MAX * sizeof(lr_sparse_t)) {
        uint8_t *buf = malloc(len);
        if (buf) {
            if (nvs_get_blob(h, key, buf, &len) == ESP_OK) {
                uint32_t magic;
                uint16_t count, used;
                char deck[LR_DECK_W];
                memcpy(&magic, buf, 4);
                memcpy(deck,  buf + 4, LR_DECK_W);
                memcpy(&count, buf + 4 + LR_DECK_W, 2);
                memcpy(&used,  buf + 6 + LR_DECK_W, 2);
                size_t need5 = LR_HDR + (size_t)used * sizeof(lr_sparse_t);
                size_t need4 = LR_HDR + (size_t)used * sizeof(lr_sparse_v4_t);
                if ((magic == LR_MAGIC || magic == LR_MAGIC_OLD) &&
                    strncmp(deck, s_deck, LR_DECK_W) == 0 &&
                    count == (uint16_t)s_count &&
                    ((magic == LR_MAGIC && len >= need5) ||
                     (magic == LR_MAGIC_OLD && len >= need4))) {
                    if (magic == LR_MAGIC)
                        unpack(buf, used);
                    else
                        unpack_v4(buf, used);
                    LOG_I("restored learning state: %d words (%d active, deck=%s, %s)",
                          s_count, used, s_deck[0] ? s_deck : "default",
                          magic == LR_MAGIC ? "LR05" : "LR04->compat");
                } else {
                    /* 键错位（manifest 改 id）/词库规模变化/旧格式：
                     * 保守丢弃旧状态（重新学习） */
                    LOG_W("lr_state stale (count %u != %d, deck mismatch or old format), dropped",
                          (unsigned)count, s_count);
                }
            }
            free(buf);
        }
    }
    nvs_close(h);
}

void learning_state_init(int word_count, const char *deck_id)
{
    if (word_count > LEARNING_STATE_MAX) word_count = LEARNING_STATE_MAX;
    if (word_count < 0) word_count = 0;
    s_count = word_count;

    /* 状态数组 PSRAM（词池同策略；失败退化为不跟踪，评分/收藏静默失效） */
    s_state = (lr_entry_t *)heap_caps_malloc(
        (size_t)LEARNING_STATE_MAX * sizeof(lr_entry_t), MALLOC_CAP_SPIRAM);
    if (!s_state) {
        LOG_E("lr state PSRAM alloc failed (%uB), tracking disabled",
              (unsigned)(LEARNING_STATE_MAX * sizeof(lr_entry_t)));
        s_count = 0;
        return;
    }
    memset(s_state, 0, (size_t)LEARNING_STATE_MAX * sizeof(lr_entry_t));

    /* 会话已评分 bitmap（500B PSRAM；失败到期视图退化空，见 lr_done） */
    s_done = (uint8_t *)heap_caps_calloc(LEARNING_STATE_MAX / 8 + 1, 1,
                                         MALLOC_CAP_SPIRAM);
    if (!s_done)
        LOG_W("lr done-bitmap alloc failed, due view degraded");

    /* LR04 卡组锁定：deck_manager 活跃 id（""=默认） */
    lr_set_deck(deck_id);

    stats_load();
    dstats_load();
    restore_from_nvs();
}

void learning_state_reload(int word_count, const char *deck_id)
{
    if (word_count > LEARNING_STATE_MAX) word_count = LEARNING_STATE_MAX;
    if (word_count < 0) word_count = 0;

    /* 复用 init 已分配的数组（重复 init 会重复 malloc 泄漏 PSRAM）；
     * 未初始化过（切书先于 init，理论不可达）退化静默不跟踪 */
    if (!s_state) {
        LOG_E("reload before init, tracking disabled");
        s_count = 0;
        return;
    }

    /* LR04 语义（v1.4 T4.2，替代 v1.3「切书作废」先例）：旧卡组进度
     * 先落盘（键按旧 s_deck 分派，兜底 5s 静默窗口内未写脏数据）
     * ——切书不丢学习进度（与 rd_* 阅读进度同验收口径）；lr_stats
     * 全局保留（跨卡组今日/连续累计）。save 后才重锁新词数 */
    learning_state_save();
    s_count = word_count;

    memset(s_state, 0, (size_t)LEARNING_STATE_MAX * sizeof(lr_entry_t));
    if (s_done)
        memset(s_done, 0, LEARNING_STATE_MAX / 8 + 1);

    /* 旧卡组未上报事件丢弃（word_idx 对新卡组无意义）；脏标记撤销 */
    portENTER_CRITICAL(&s_ev_mux);
    s_ev_head = 0;
    s_ev_count = 0;
    portEXIT_CRITICAL(&s_ev_mux);
    s_dirty = false;
    s_dirty_at = 0;

    /* 换组后从新键恢复该卡组历史状态（无记录=从零学） */
    lr_set_deck(deck_id);
    restore_from_nvs();
    LOG_I("learning state reloaded for deck switch: %d words (deck=%s)",
          s_count, s_deck[0] ? s_deck : "default");
}

void learning_state_save(void)
{
    if (s_count <= 0 || !s_state) return;

    /* sparse 打包：只收非默认态词（stability>0 / 连错>0 / 已收藏 /
     * 已墨封），idx 升序写入；超 LR_SPARSE_MAX 丢尾部并告警（见宏注释） */
    int active = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_state[i].srs.stability > 0.f ||
            s_state[i].consecutive_wrong > 0 || s_state[i].collected ||
            s_state[i].mastered)
            active++;
    }
    if (active > LR_SPARSE_MAX) {
        LOG_W("lr sparse overflow: %d active > %d cap, tail dropped",
              active, LR_SPARSE_MAX);
        active = LR_SPARSE_MAX;
    }

    size_t len = LR_HDR + (size_t)active * sizeof(lr_sparse_t);
    uint8_t *buf = malloc(len);   /* 最坏 ~84KB（4000 词满库），DRAM 可靠 */
    if (!buf) {
        LOG_W("lr_state save: OOM (%u bytes)", (unsigned)len);
        return;
    }

    uint32_t magic = LR_MAGIC;
    uint16_t count = (uint16_t)s_count;
    uint16_t used  = (uint16_t)active;
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, s_deck, LR_DECK_W);          /* NUL 补齐短串 */
    memcpy(buf + 4 + LR_DECK_W, &count, 2);
    memcpy(buf + 6 + LR_DECK_W, &used, 2);

    lr_sparse_t *sp = (lr_sparse_t *)(buf + LR_HDR);
    int64_t now = now_sec();
    int w = 0;
    for (int i = 0; i < s_count && w < active; i++) {
        if (!(s_state[i].srs.stability > 0.f ||
              s_state[i].consecutive_wrong > 0 || s_state[i].collected ||
              s_state[i].mastered))
            continue;
        int64_t delta = s_state[i].srs.next_review - now;
        int64_t last  = now - s_state[i].srs.last_review;   /* ≥0（last ≤ now） */
        sp[w].idx         = (uint16_t)i;
        sp[w].p.stability = s_state[i].srs.stability;
        sp[w].p.difficulty = s_state[i].srs.difficulty;
        sp[w].p.next_delta = delta < 0 ? 0 : (int32_t)delta;
        sp[w].p.last_delta = last < 0 ? 0 : (int32_t)last;
        sp[w].p.level     = s_state[i].srs.srs_level;
        sp[w].p.wrong     = s_state[i].consecutive_wrong;
        sp[w].p.collected = s_state[i].collected ? 1 : 0;
        sp[w].p.mastered  = s_state[i].mastered ? 1 : 0;
        w++;
    }

    char key[16];
    lr_key(key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_set_blob(h, key, buf, len);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err != ESP_OK) LOG_W("lr_state save failed: %d", (int)err);
        nvs_close(h);
    }
    free(buf);

    /* 今日统计随同窗口落盘（独立 key，跨词库变化保留；SD01 按组
     * 表同窗口——评分路径两表同脏，写失败独立告警互不阻断） */
    if (s_stats_dirty) {
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t err = nvs_set_blob(h, NVS_KEY_LR_STATS, &s_stats, sizeof(s_stats));
            if (err == ESP_OK) err = nvs_commit(h);
            if (err != ESP_OK) LOG_W("lr_stats save failed: %d", (int)err);
            nvs_close(h);
        }
        s_stats_dirty = false;
    }
    if (s_dstats_dirty) {
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t err = nvs_set_blob(h, NVS_KEY_LR_STDECK, &s_dstats, sizeof(s_dstats));
            if (err == ESP_OK) err = nvs_commit(h);
            if (err != ESP_OK) LOG_W("lr_stdeck save failed: %d", (int)err);
            nvs_close(h);
        }
        s_dstats_dirty = false;
    }
}

void learning_state_maybe_save(void)
{
    if (!s_dirty || s_count <= 0 || !s_state) return;
    if (now_sec() - s_dirty_at < LR_SAVE_DELAY_SEC) return;  /* 静默窗口内 */
    s_dirty = false;
    learning_state_save();
}

void learning_state_apply_quality(int word_idx, int quality)
{
    if (word_idx < 0 || word_idx >= s_count) return;

    lr_entry_t *e = &s_state[word_idx];
    bool was_new = !(e->srs.stability > 0.f);   /* 首评=新学（今日统计口径） */
    if (was_new) srs_init_node(&e->srs, now_sec());
    uint16_t interval = srs_calculate_next_review((srs_quality_t)quality, &e->srs, now_sec());

    if (quality < 3) {
        if (e->consecutive_wrong < 255) e->consecutive_wrong++;
    } else {
        e->consecutive_wrong = 0;
    }

    /* 本会话已处理位（复习到期视图不再推） */
    lr_mark_done(word_idx);

    /* 今日统计：时钟同步后计数；未同步挂起（首个有效评分补结）。
     * 全局 lr_stats 与按组 SD01 同日同源（T5.5 科目级配额分子） */
    int64_t ep = standby_time_now();
    if (ep > 0) {
        int32_t today = epoch_ymd(ep);
        stats_roll(ep);
        if (s_stats.today_reviews < 65535) s_stats.today_reviews++;
        if (was_new && s_stats.today_new < 65535) s_stats.today_new++;
        s_stats_dirty = true;

        dstats_roll(today);
        int di = dstats_find(s_deck);
        if (di < 0) di = dstats_alloc(s_deck);
        if (s_dstats.e[di].rev < 65535) s_dstats.e[di].rev++;
        if (was_new && s_dstats.e[di].new_n < 65535) s_dstats.e[di].new_n++;
        s_dstats_dirty = true;

        /* v1.6 课程表：跨日首次评分时尝试自动激活（同日重复调用
         * 返回 1 无动作；自动激活路径调 deck_flow_switch 会走
         * schedule_on_manual_switch，但此时 last_slot=-1 不会误标） */
        schedule_try_activate();
    }

    LOG_I("quality %d -> word #%d (wrong=%u interval=%u)",
          quality, word_idx, (unsigned)e->consecutive_wrong,
          (unsigned)interval);
    event_push(word_idx, quality, -1);  /* 云端上报队列（无 cloudId 词 flush 时丢弃） */
    s_dirty = true; s_dirty_at = now_sec();  /* 延迟落盘（maybe_save） */
}

bool learning_state_toggle_collect(int word_idx)
{
    if (word_idx < 0 || word_idx >= s_count) return false;
    s_state[word_idx].collected = !s_state[word_idx].collected;
    LOG_I("collect word #%d -> %s", word_idx,
          s_state[word_idx].collected ? "on" : "off");
    event_push(word_idx, -1, s_state[word_idx].collected ? 1 : 0);
    s_dirty = true; s_dirty_at = now_sec();      /* 延迟落盘（maybe_save） */
    return s_state[word_idx].collected;
}

bool learning_state_is_collected(int word_idx)
{
    return (word_idx >= 0 && word_idx < s_count) && s_state[word_idx].collected;
}

/* ---- 墨封（2026-09-04，收藏同构；置位方向清连错=声明式通过，
 * 与后端 SyncMaster 双端镜像） ---- */
bool learning_state_toggle_master(int word_idx)
{
    if (word_idx < 0 || word_idx >= s_count) return false;
    lr_entry_t *e = &s_state[word_idx];
    e->mastered = !e->mastered;
    if (e->mastered) e->consecutive_wrong = 0;   /* 移出错词本 */
    LOG_I("master word #%d -> %s (wrong=%u)", word_idx,
          e->mastered ? "on" : "off", (unsigned)e->consecutive_wrong);
    event_push(word_idx, -2, e->mastered ? 1 : 0);   /* 云端墨封事件 */
    s_dirty = true; s_dirty_at = now_sec();      /* 延迟落盘（maybe_save） */
    return e->mastered;
}

bool learning_state_is_mastered(int word_idx)
{
    return (word_idx >= 0 && word_idx < s_count) && s_state[word_idx].mastered;
}

int learning_state_mastered_count(void)
{
    if (!s_state) return 0;
    int n = 0;
    for (int i = 0; i < s_count; i++)
        if (s_state[i].mastered) n++;
    return n;
}

int learning_state_mastered_at(int pos)
{
    if (!s_state || pos < 0) return -1;
    for (int i = 0; i < s_count; i++) {
        if (s_state[i].mastered && pos-- == 0) return i;
    }
    return -1;
}

int learning_state_active_count(void)
{
    if (!s_state) return 0;
    int n = 0;
    for (int i = 0; i < s_count; i++)
        if (!s_state[i].mastered) n++;
    return n;
}

int learning_state_active_at(int pos)
{
    if (!s_state || pos < 0) return -1;
    for (int i = 0; i < s_count; i++) {
        if (!s_state[i].mastered && pos-- == 0) return i;
    }
    return -1;
}

int learning_state_active_new_count(void)
{
    if (!s_state) return 0;
    int n = 0;
    for (int i = 0; i < s_count; i++)
        if (!s_state[i].mastered &&
            !(s_state[i].srs.stability > 0.f)) n++;
    return n;
}

int learning_state_wrong_count(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++)
        if (s_state[i].consecutive_wrong > 0) n++;
    return n;
}

int learning_state_wrong_at(int pos)
{
    if (pos < 0) return -1;
    for (int i = 0; i < s_count; i++) {
        if (s_state[i].consecutive_wrong > 0 && pos-- == 0) return i;
    }
    return -1;
}

int learning_state_collected_count(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++)
        if (s_state[i].collected) n++;
    return n;
}

int learning_state_collected_at(int pos)
{
    if (pos < 0) return -1;
    for (int i = 0; i < s_count; i++) {
        if (s_state[i].collected && pos-- == 0) return i;
    }
    return -1;
}

/* 到期判定：已学 + FSRS due + 本会话未评（O(N) 虚游走，与
 * wrong/collected 同构；N≤4000 按键频次下可接受）。考试冲刺
 * horizon（T5.5）：>0 时判定时刻前推 horizon 天——考试前将到期的
 * 词提前入队清账（日期反推优先推到期卡） */
static int s_due_horizon = 0;   /* 天；0=标准 due；外部注入，切组保持 */

void learning_state_set_due_horizon(int days)
{
    s_due_horizon = (days > 0 && days <= 99) ? days : 0;
}

static bool due_now(int i, int64_t now)
{
    int64_t eff = s_due_horizon > 0
                      ? now + (int64_t)s_due_horizon * 86400 : now;
    /* 已墨封词到期不推（调度层过滤；启封后按原到期自然回队） */
    return s_state[i].srs.stability > 0.f && !lr_done(i) &&
           !s_state[i].mastered && srs_is_due(&s_state[i].srs, eff);
}

int learning_state_due_count(void)
{
    if (!s_state || s_count <= 0) return 0;
    int64_t now = now_sec();
    int n = 0;
    for (int i = 0; i < s_count; i++)
        if (due_now(i, now)) n++;
    return n;
}

int learning_state_due_at(int pos)
{
    if (!s_state || pos < 0) return -1;
    int64_t now = now_sec();
    for (int i = 0; i < s_count; i++) {
        if (due_now(i, now) && pos-- == 0) return i;
    }
    return -1;
}

bool learning_state_is_new(int word_idx)
{
    return (word_idx >= 0 && word_idx < s_count) &&
           !(s_state[word_idx].srs.stability > 0.f);
}

int learning_state_today_new(void)      { return s_stats.today_new; }
int learning_state_today_reviews(void)  { return s_stats.today_reviews; }
int learning_state_streak_days(void)    { return s_stats.streak_days; }
