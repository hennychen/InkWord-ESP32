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
 * NVS 布局：命名空间 "inkword"，单 blob "lr_state"（LR02 sparse）=
 *   { u32 magic, u16 count, u16 used, lr_sparse_t[used] }，只存非默认
 *   态词（≤300 活跃词 ≈ 6.3KB，配 nvs 24KB 分区；见 LR_SPARSE_MAX）。
 * 保存时机：评分/收藏仅置脏，主循环 maybe_save 静默 5s 后一次性写
 * （按键路径零 NVS 阻塞；掉电窗口 ≤5s 与事件队列策略一致）。
 */
#include "learning_state.h"
#include "srs_engine.h"
#include "debug_log.h"

#include "nvs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"   /* 状态数组 PSRAM（词库扩容 2026-08-20） */
#include "freertos/FreeRTOS.h"  /* portMUX：队列跨 task 访问的临界区 */

#include <string.h>
#include <stdlib.h>

static const char *TAG = "LRN";

/* ---- 运行态 ---- */

typedef struct {
    SrsNode srs;               /* ease==0 表示未学（惰性初始化） */
    uint8_t consecutive_wrong; /* 连错计数，>0 即在错词本 */
    bool    collected;         /* 收藏标志 */
} lr_entry_t;

static lr_entry_t *s_state = NULL;  /* PSRAM（4000 词×~24B；词池扩容前为 DRAM 静态 128 词） */
static int s_count = 0;

/* ---- 脏标记延迟落盘：评分/收藏仅置脏（按键路径零 NVS 写阻塞），
 * 主循环 maybe_save 静默 LR_SAVE_DELAY 秒后一次性写入 ---- */
#define LR_SAVE_DELAY_SEC  (5)
static bool    s_dirty = false;
static int64_t s_dirty_at = 0;    /* 最后一次置脏时刻（连续操作刷新静默计时） */

/* ---- NVS 打包格式（变更需 bump LR_MAGIC 使旧数据整体失效） ----
 * LR02（词库扩容 2026-08-20）：sparse —— 只存非默认态词
 * （ease>0 / 连错>0 / 已收藏），适配 nvs 24KB 分区；
 * LR01 全量格式（4000 词≈76KB blob）物理写不下，升级即作废 */

#define LR_MAGIC   0x4C523032u  /* "LR02" */

typedef struct __attribute__((packed)) {
    float    ease;
    uint16_t rep;
    uint16_t interval;
    int32_t  next_delta;  /* 距保存时刻的剩余到期秒（<0 存 0） */
    uint8_t  level;
    uint8_t  wrong;
    uint8_t  collected;
} lr_packed_t;  /* 19B */

typedef struct __attribute__((packed)) {
    uint16_t    idx;      /* 词库索引（升序写入，读回按此回填） */
    lr_packed_t p;
} lr_sparse_t;  /* 21B */

/* sparse 容量保护：nvs 24KB 分区扣除其他键后 lr_state 可用 ~17KB，
 * 每活跃词占 21B 数据 + NVS entry 开销 ~32B ≈ 53B → 保守上限 300；
 * 超限丢 idx 大的尾部（极小概率：全词库学完才触达）并告警，
 * 根治需扩 nvs 分区（16MB Flash 充裕，待后续分区表修订） */
#define LR_SPARSE_MAX  (300)

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

/* sparse 回填：blob = header(8B) + used × lr_sparse_t，按 idx 升序 */
static void unpack(const uint8_t *buf, uint16_t used)
{
    const lr_sparse_t *sp = (const lr_sparse_t *)(buf + 8);
    int64_t now = now_sec();
    for (int i = 0; i < used; i++) {
        uint16_t idx = sp[i].idx;
        if (idx >= (uint16_t)s_count) continue;   /* 防御：异常 idx 跳过 */
        s_state[idx].srs.ease_factor   = sp[i].p.ease;
        s_state[idx].srs.repetition    = sp[i].p.rep;
        s_state[idx].srs.interval_days = sp[i].p.interval;
        s_state[idx].srs.next_review   = now + sp[i].p.next_delta;
        s_state[idx].srs.srs_level     = sp[i].p.level;
        s_state[idx].consecutive_wrong = sp[i].p.wrong;
        s_state[idx].collected         = sp[i].p.collected != 0;
    }
}

void learning_state_init(int word_count)
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

    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) != ESP_OK) return;

    /* LR02 sparse：header = magic(4) + count(2) + used(2) */
    size_t len = 0;
    if (nvs_get_blob(h, "lr_state", NULL, &len) == ESP_OK &&
        len >= 8 && len <= 8 + (size_t)LR_SPARSE_MAX * sizeof(lr_sparse_t)) {
        uint8_t *buf = malloc(len);
        if (buf) {
            if (nvs_get_blob(h, "lr_state", buf, &len) == ESP_OK) {
                uint32_t magic;
                uint16_t count, used;
                memcpy(&magic, buf, 4);
                memcpy(&count, buf + 4, 2);
                memcpy(&used,  buf + 6, 2);
                if (magic == LR_MAGIC && count == (uint16_t)s_count &&
                    len >= 8 + (size_t)used * sizeof(lr_sparse_t)) {
                    unpack(buf, used);
                    LOG_I("restored learning state: %d words (%d active)",
                          s_count, used);
                } else {
                    /* 词库规模变化/旧格式：保守丢弃旧状态（重新学习） */
                    LOG_W("lr_state stale (count %u != %d or old format), dropped",
                          (unsigned)count, s_count);
                }
            }
            free(buf);
        }
    }
    nvs_close(h);
}

void learning_state_save(void)
{
    if (s_count <= 0 || !s_state) return;

    /* sparse 打包：只收非默认态词（学过/连错>0/已收藏），
     * idx 升序写入；超 LR_SPARSE_MAX 丢尾部并告警（见宏注释） */
    int active = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_state[i].srs.ease_factor > 0.f ||
            s_state[i].consecutive_wrong > 0 || s_state[i].collected)
            active++;
    }
    if (active > LR_SPARSE_MAX) {
        LOG_W("lr sparse overflow: %d active > %d cap, tail dropped",
              active, LR_SPARSE_MAX);
        active = LR_SPARSE_MAX;
    }

    size_t len = 8 + (size_t)active * sizeof(lr_sparse_t);
    uint8_t *buf = malloc(len);   /* ≤ ~6.3KB，DRAM 可靠 */
    if (!buf) {
        LOG_W("lr_state save: OOM (%u bytes)", (unsigned)len);
        return;
    }

    uint32_t magic = LR_MAGIC;
    uint16_t count = (uint16_t)s_count;
    uint16_t used  = (uint16_t)active;
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &count, 2);
    memcpy(buf + 6, &used, 2);

    lr_sparse_t *sp = (lr_sparse_t *)(buf + 8);
    int64_t now = now_sec();
    int w = 0;
    for (int i = 0; i < s_count && w < active; i++) {
        if (!(s_state[i].srs.ease_factor > 0.f ||
              s_state[i].consecutive_wrong > 0 || s_state[i].collected))
            continue;
        int64_t delta = s_state[i].srs.next_review - now;
        sp[w].idx        = (uint16_t)i;
        sp[w].p.ease      = s_state[i].srs.ease_factor;
        sp[w].p.rep       = s_state[i].srs.repetition;
        sp[w].p.interval  = s_state[i].srs.interval_days;
        sp[w].p.next_delta = delta < 0 ? 0 : (int32_t)delta;
        sp[w].p.level     = s_state[i].srs.srs_level;
        sp[w].p.wrong     = s_state[i].consecutive_wrong;
        sp[w].p.collected = s_state[i].collected ? 1 : 0;
        w++;
    }

    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_set_blob(h, "lr_state", buf, len);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err != ESP_OK) LOG_W("lr_state save failed: %d", (int)err);
        nvs_close(h);
    }
    free(buf);
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
    if (e->srs.ease_factor == 0.f) srs_init_node(&e->srs, now_sec());
    srs_calculate_next_review((srs_quality_t)quality, &e->srs, now_sec());

    if (quality < 3) {
        if (e->consecutive_wrong < 255) e->consecutive_wrong++;
    } else {
        e->consecutive_wrong = 0;
    }

    LOG_I("quality %d -> word #%d (wrong=%u interval=%u)",
          quality, word_idx, (unsigned)e->consecutive_wrong,
          (unsigned)e->srs.interval_days);
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
