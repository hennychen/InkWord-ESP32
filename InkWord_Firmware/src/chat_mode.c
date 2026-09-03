/**
 * @file chat_mode.c
 * @brief AI 语音对话模式状态机实现 (P2B，2026-08-24)
 *
 * 常驻任务 + 轮询触发位（20ms 拾起延迟无感）：单任务串行整轮
 * 录音→上传→下载→播放，无并发 HTTP/播放竞态；播放打断重说 =
 * 停播 + 置触发位，任务收尾清理后自然进新一轮。
 *
 * HTTP 复用项目惯例：按 URL 前缀 http:/其余 选明文/TLS（sync_client/
 * ota_manager/audio_sync/mic_recorder 同策略）、multipart 三段流式写
 * （PSRAM 峰值纪律）、X-Device-Key 头。上传超时 45s——后端单端点
 * 串 ASR+LLM+TTS 全链路（局域网实测 3-6s，云路径余量）。
 *
 * 回复 MP3 落 /sdcard/audio/chat_tmp.mp3 走 audio_play_file()（零新
 * 播放路径），播完即删；audioUrl 为空（后端 TTS 失败降级）仅屏显
 * 文本，语音链路降级不熔断。
 *
 * A3 生词命中：响应 wordHits[{text,cloudId}] ≤5 条缓存（idle 态计数
 * 行屏显），SET 短按置收藏触发位——逐条推 /sync/collect 在任务上下
 * 文串行执行（按键回调零阻塞，同轮次触发位范式）；三色屏无局刷时
 * 仅震动反馈。
 */
#include "chat_mode.h"
#include "debug_log.h"
#include "gpio_config.h"       /* AUDIO_DIR */
#include "audio_player.h"
#include "mic_recorder.h"
#include "sync_client.h"       /* base_url / device_key 配置源 */
#include "haptic.h"
#include "wifi_manager.h"
#include "chat_ui.h"           /* T1.3：屏显回调（ui_render_chat/anim_tick，原 main.cpp extern） */
#include "epd_driver.h"        /* T2.2 栈化：chat_page_enter 首帧 flush */
#include "study_mode_machine.h" /* T2.2 栈化：study_mode_exit_chat/MODE_CHAT */
#include "page_router.h"       /* T2.2 栈化：g_chat_page/pop_if/render_top */

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "CHAT";

/* main.cpp 导出的状态栏绘制（ui_render_word 同款 extern 先例；
 * chat_page_enter 首帧取用） */
extern void ui_draw_status(study_mode_t mode);

#define CHAT_TASK_STACK     (6 * 1024)
#define CHAT_TASK_PRIO      (4)     /* pron/audio_sync 同级，低于 btn_scan(5) */
#define CHAT_MAX_MS         (10000) /* 对话档录音上限（P1 跟读 3s） */
#define CHAT_UPLOAD_TIMEOUT_MS (45000) /* 含后端 ASR+LLM+TTS 全链路 */
#define CHAT_DL_TIMEOUT_MS  (15000)
#define CHAT_MP3_MAX_BYTES  (512 * 1024) /* 回复 ≤2 句 ≤40 词，超此为异常 */
#define CHAT_PLAY_MAX_MS    (30000) /* 播放等待兑底（打断/超时出循环） */
#define CHAT_TMP_PATH       (AUDIO_DIR "/chat_tmp.mp3")
#define CHAT_RESP_MAX       (2304)  /* reply 256 + warmup 192 + audioUrl + wordHits 5×(text+Guid) + 信封余量 */
#define CHAT_URL_MAX        (192)   /* base_url + 路径 + mode/scenario query */
/* ---- P0-1 流式（2026-08-30）：NDJSON 增量读 + 句级流水线 ---- */
#define CHAT_STREAM_READ_MS   (500)  /* 读流短超时：回环查打断位（barge-in 0.5s 拾起）
                                          * + 驱动 THINKING 涟漪帧（ui_chat_anim_tick） */
#define CHAT_STREAM_IDLE_MAX  (90)    /* 连续超时上限（×500ms=45s，对齐老上传超时兑底） */
#define CHAT_STREAM_PLAY_MAX_MS (120000) /* 续播等待兑底（translate ≤6 句，打断/超时出循环） */
#define CHAT_ABORT_TIMEOUT_MS (3000)  /* abort 上报封顶（fire-and-forget，失败忽略） */
#define CHAT_SENT_MAX       (8)      /* 落地句文件表（后端护栏 ≤6 句，余量） */
#define CHAT_SENT_NAME_MAX  (40)     /* chat_{13位ts}_s{0-7}.mp3 */
#define CHAT_ABORT_URL_MAX  (256)    /* base_url + /chat/abort?roundId={36} */

/* ---- 模块状态（单写者=对话任务，按键上下文仅写触发/取消位） ---- */
static volatile bool s_active    = false; /* 模式在（任务生命周期域） */
static volatile bool s_round_req = false; /* 中键触发新一轮 */
static volatile bool s_cancel    = false; /* 录音取消位（丢弃） */
static volatile bool s_send_now  = false; /* 录音手动断（说完即发，保留） */
static volatile bool s_stop_play = false; /* 轮次中止位（P0-1 起：读流/播放期通用打断，新轮开头作废） */
static volatile chat_state_t s_state = CHAT_STATE_IDLE;
static char s_reply[CHAT_REPLY_MAX];      /* 末句回复（屏显驻留） */
static char s_warmup[CHAT_WARMUP_MAX];    /* 场景首轮预热（speaking 态屏显） */
static chat_request_t s_req;              /* 模式请求（enter 拷入，任务读） */
static int  s_hits_n = 0;                 /* A3 生词命中数（任务写，渲染读） */
static char s_hits_text[CHAT_HIT_MAX][CHAT_HIT_TEXT_MAX];
static char s_hits_cloud[CHAT_HIT_MAX][CHAT_HIT_CLOUD_MAX];
static volatile bool s_collect_req = false; /* SET 收藏触发位（按键置位） */

/* P0-1 流式行缓冲（静态：任务串行独占，栈节流；兼容老后端单行 JSON
 * 整包 2304B——首行无 t 字段即回退老路径）；残留长度跨 read 保有 */
static char s_line[CHAT_RESP_MAX];
static int  s_line_len;

/* P1-1 replay（end 行 commands 携带 replay 且本轮已播完 → 重播已落地
 * 句文件）：任务上下文自置自清（单任务串行，无需 volatile）；句文件
 * 删除推迟到重播/打断/新轮/退出后统一执行（replay_purge 四处收口） */
static char s_replay_files[CHAT_SENT_MAX][CHAT_SENT_NAME_MAX];
static int  s_replay_total;
static bool s_replay_req;

/* 全句拼接（IDLE 回看：墨水屏静态驻留红利，2026-09-01 最优方案）
 * + 当前播放句号（PLAYING 进度指示） */
static char s_full_reply[CHAT_RESP_MAX];
static int  s_sent_no;

/* 本轮录音反馈（2026-09-01 真机实测：录音期禁刷屏，说话中页面无任何
 * 动静，用户无从得知 mic 是否正常——反馈全部落在录音结束后）：
 *   s_rec_ms 录音时长（UPLOADING 态「已录 X.X 秒」屏显，零网络依赖）；
 *   s_heard  后端 ASR 识别文本（meta.transcript，THINKING 态「你说：…」
 *           回显——mic 正常与否的最强证据）；s_meta_got 区分
 *           「meta 未到」(NULL) 与「到而空串」(未听到内容) */
static char s_heard[128];
static bool s_meta_got;
static int  s_rec_ms;

const char *chat_mode_full_reply(void) { return s_full_reply; }
int chat_mode_sentence_no(void) { return s_sent_no; }
/* meta 未到返回 NULL；到则返回串（可能为空串=后端判无话音） */
const char *chat_mode_heard(void) { return s_meta_got ? s_heard : NULL; }
int chat_mode_rec_ms(void) { return s_rec_ms; }

/* ---- 后端响应子集（transcript 设备端不消费，屏显克制；A1 增 warmup，
 *      A3 增 wordHits） ---- */
typedef struct {
    char reply[CHAT_REPLY_MAX];
    char audio_url[160];       /* "/api/device/audio/chat_{ts}.mp3" */
    char warmup[CHAT_WARMUP_MAX]; /* 场景首轮中文预热（其余轮空串） */
    int  hits_n;               /* 生词命中数（wordHits 实收，≤CHAT_HIT_MAX） */
    char hits_text[CHAT_HIT_MAX][CHAT_HIT_TEXT_MAX];
    char hits_cloud[CHAT_HIT_MAX][CHAT_HIT_CLOUD_MAX];
} chat_resp_t;

/* 按 URL 前缀选传输：http: 明文 TCP，其余 TLS + 证书包（项目第 5 处同款） */
static void chat_fill_cfg(esp_http_client_config_t *cfg, const char *url,
                          int timeout_ms)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = timeout_ms;
    if (strncmp(url, "http:", 5) == 0) {
        cfg->transport_type = HTTP_TRANSPORT_OVER_TCP;
    } else {
        cfg->transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg->crt_bundle_attach = arduino_esp_crt_bundle_attach;
    }
}

/* 状态切换 + 屏显（模式已退则跳过渲染；text 空 = 驻留末句回复） */
static void set_state(chat_state_t st, const char *text)
{
    s_state = st;
    if (!s_active) return;
    ui_render_chat(st, (text && text[0]) ? text : s_reply);
}

/* 网络失败：一长震 + 状态区提示，短暂驻留（可被新一轮提前打断）后
 * 回 idle 不退模式（计划 P2B §5） */
static void net_fail(void)
{
    haptic_event(HAPTIC_ERROR);
    set_state(CHAT_STATE_NETFAIL, "网络不可用");
    int waited = 0;
    while (waited < 2500 && !s_round_req && s_active) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (s_active) set_state(CHAT_STATE_IDLE, NULL);
}

/* 老后端 JSON 信封解析（P0-1 提取：chat_upload 与流式回退共用单一
 * 来源，协议冻结 §2）：{ code:0, data:{ reply, audioUrl?, warmup?,
 * wordHits? } }；audioUrl/warmup/wordHits 均可选降级不熔断 */
static void chat_parse_hits(cJSON *jhits, chat_resp_t *out)
{
    if (!cJSON_IsArray(jhits)) return;
    cJSON *jh;
    cJSON_ArrayForEach(jh, jhits) {
        if (out->hits_n >= CHAT_HIT_MAX) break;
        cJSON *jt = cJSON_GetObjectItem(jh, "text");
        cJSON *jc = cJSON_GetObjectItem(jh, "cloudId");
        if (cJSON_IsString(jt) && jt->valuestring &&
            jt->valuestring[0] &&
            cJSON_IsString(jc) && jc->valuestring &&
            jc->valuestring[0]) {
            strncpy(out->hits_text[out->hits_n],
                    jt->valuestring, CHAT_HIT_TEXT_MAX - 1);
            strncpy(out->hits_cloud[out->hits_n],
                    jc->valuestring, CHAT_HIT_CLOUD_MAX - 1);
            out->hits_n++;
        }
    }
}

static int chat_parse_legacy(const char *json, chat_resp_t *out)
{
    int rc = -1;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *jreply = data ? cJSON_GetObjectItem(data, "reply") : NULL;
        if (cJSON_IsNumber(code) && code->valueint == 0 &&
            cJSON_IsString(jreply) && jreply->valuestring &&
            jreply->valuestring[0]) {
            strncpy(out->reply, jreply->valuestring, sizeof(out->reply) - 1);
            cJSON *jurl = cJSON_GetObjectItem(data, "audioUrl");
            /* audioUrl 可空（后端 TTS 失败降级：仅文本回复） */
            if (cJSON_IsString(jurl) && jurl->valuestring)
                strncpy(out->audio_url, jurl->valuestring,
                        sizeof(out->audio_url) - 1);
            /* warmup 可选（A1 场景首轮中文预热，其余轮缺省/空） */
            cJSON *jwarm = data ? cJSON_GetObjectItem(data, "warmup") : NULL;
            if (cJSON_IsString(jwarm) && jwarm->valuestring)
                strncpy(out->warmup, jwarm->valuestring,
                        sizeof(out->warmup) - 1);
            /* wordHits 可选（A3 生词命中 [{text,cloudId}]，超上限截断） */
            chat_parse_hits(data ? cJSON_GetObjectItem(data, "wordHits") : NULL, out);
            rc = 0;
        }
        cJSON_Delete(root);
    }
    return rc;
}

/* P0-1 注：原 chat_upload 整段函数（multipart 写 + 一次性 read + 信封
 * 解析）已由 run_round_stream 取代：multipart 写入其内（追加 stream=1
 * 协商位，老后端忽略未知 query 行为不变）、信封解析提取为
 * chat_parse_legacy（流式回退直接解析已到响应，不重传不浪费）。 */

/* ---- 回复 MP3 下载（audio_sync download_one 同构；固定名直写，用完
 *      即删由调用方收口，失败 remove 半文件，无需 tmp+rename） ---- */
typedef struct {
    FILE  *f;
    size_t bytes;
    bool   bad;
} chat_dl_ctx_t;

static esp_err_t chat_dl_event(esp_http_client_event_t *evt)
{
    chat_dl_ctx_t *ctx = (chat_dl_ctx_t *)evt->user_data;
    if (ctx && evt->event_id == HTTP_EVENT_ON_DATA && !ctx->bad) {
        if (ctx->bytes + evt->data_len > CHAT_MP3_MAX_BYTES ||
            fwrite(evt->data, 1, evt->data_len, ctx->f) != (size_t)evt->data_len) {
            ctx->bad = true;
            return ESP_OK;
        }
        ctx->bytes += evt->data_len;
    }
    return ESP_OK;
}

static int chat_download_to(const char *audio_url, const char *path)
{
    char url[288];
    snprintf(url, sizeof(url), "%s%s", sync_get_base_url(), audio_url);

    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG_W("fopen %s failed (errno=%d, 无 SD?)", path, errno);
        return -1;
    }

    chat_dl_ctx_t ctx = { .f = f };
    esp_http_client_config_t cfg;
    chat_fill_cfg(&cfg, url, CHAT_DL_TIMEOUT_MS);
    cfg.event_handler = chat_dl_event;
    cfg.user_data = &ctx;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(f);
        remove(CHAT_TMP_PATH);
        return -1;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "X-Device-Key", sync_get_device_key());

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    fclose(f);

    if (!ctx.bad && err == ESP_OK && status == 200 && ctx.bytes > 0)
        return 0;
    remove(path);
    LOG_W("chat audio download failed: err=%d status=%d bytes=%u",
          err, status, (unsigned)ctx.bytes);
    return -1;
}

/* 老整段路径固定名下载（P0-1 前唯一入口，现仅回退分支使用） */
static int chat_download(const char *audio_url)
{
    return chat_download_to(audio_url, CHAT_TMP_PATH);
}

/* ---- P0-1 流式轮次（2026-08-30）：NDJSON 增量读 + 句级流水线播放
 *      + barge-in（协议见 AI_CHAT_MODE.md §2b） ---- */

typedef struct {
    char round_id[CHAT_SENT_NAME_MAX];        /* meta.roundId（abort 用；空=未收到不发） */
    char files[CHAT_SENT_MAX][CHAT_SENT_NAME_MAX]; /* 已落地句文件名 */
    int  total;                               /* 已落地句数 */
    int  played;                              /* 已投播句数（下一句下标） */
    bool started;                             /* 首句已起播（haptic/屏显只做一次） */
} chat_stream_t;

/* 停播并等音频任务退出播放循环（fd 打开时 remove 未定义，老路径
 * 同款 ≤2s 兑底） */
static void stream_stop_wait(void)
{
    audio_stop();
    int waited = 0;
    while (audio_is_playing() && waited < 2000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
}

/* 轮次收尾：停播等退出后删净句文件（残网兑底：后端 CleanupChatClips
 * 小时级回收） */
static void stream_cleanup(chat_stream_t *st)
{
    stream_stop_wait();
    for (int i = 0; i < st->total; i++) {
        char path[CHAT_SENT_NAME_MAX + 16];
        snprintf(path, sizeof(path), AUDIO_DIR "/%s", st->files[i]);
        remove(path);
    }
}

/* P1-1 replay 句文件删净（replay 播完 / 新轮作废 / 模式退出收口共用）*/
static void replay_purge(void)
{
    for (int i = 0; i < s_replay_total; i++) {
        char path[CHAT_SENT_NAME_MAX + 16];
        snprintf(path, sizeof(path), AUDIO_DIR "/%s", s_replay_files[i]);
        remove(path);
    }
    s_replay_total = 0;
    s_replay_req = false;
}

/* end 行 commands.replay 触发（chat_task 串行拾起；新轮触发位优先，
 * run_round 开头 purge 作废）。播完/打断/文件丢失删净收尾，纪律同
 * 流式轮次（audio_play_file 异步入队：等 !playing 再投下一句） */
static void replay_round(void)
{
    s_replay_req = false;                    /* 先清防异常路径重入 */
    if (s_replay_total <= 0) return;

    set_state(CHAT_STATE_PLAYING, s_reply);  /* 末句回复驻留屏显 */
    int played = 0, waited = 0;
    while (played < s_replay_total && s_active && !s_stop_play &&
           waited < CHAT_STREAM_PLAY_MAX_MS) {
        if (!audio_is_playing()) {
            char path[CHAT_SENT_NAME_MAX + 16];
            snprintf(path, sizeof(path), AUDIO_DIR "/%s",
                     s_replay_files[played]);
            if (audio_play_file(path) == 0) {
                played++;
            } else {
                LOG_W("replay play s%d failed", played);
                break;                       /* 文件丢失（兑底清理早到）不重试 */
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
    }
    stream_stop_wait();                      /* 停播等音频任务退出（fd 打开时 remove 未定义） */
    LOG_I("chat replay done: %d/%d sentences", played, s_replay_total);
    replay_purge();
    if (s_active && !s_stop_play) set_state(CHAT_STATE_IDLE, NULL);
}

/* abort 上报：fire-and-forget（3s 封顶失败忽略）。未收 meta 不发
 * （后端至多损失 ASR+首句算力；关流本身亦触发 RequestAborted 截断，
 * 本调用是反代不传递断连场景的显式保底）；关流后、新录音前发出，
 * 耗时已被超时封顶，仍单任务串行 */
static void chat_abort_post(const char *round_id)
{
    if (!round_id[0]) return;
    char url[CHAT_ABORT_URL_MAX];
    snprintf(url, sizeof(url), "%s/api/device/chat/abort?roundId=%s",
             sync_get_base_url(), round_id);

    esp_http_client_config_t cfg;
    chat_fill_cfg(&cfg, url, CHAT_ABORT_TIMEOUT_MS);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "X-Device-Key", sync_get_device_key());
    esp_err_t err = esp_http_client_perform(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) LOG_W("chat abort post failed: err=%d", err);
}

/* 投播下一句（audio_play_file 异步入队：正在播时投播会打断当前曲，
 * 调用方须保证 !audio_is_playing()） */
static int stream_play_next(chat_stream_t *st)
{
    char path[CHAT_SENT_NAME_MAX + 16];
    snprintf(path, sizeof(path), AUDIO_DIR "/%s", st->files[st->played]);
    if (audio_play_file(path) == 0) {
        st->played++;
        s_sent_no = st->played;             /* PLAYING 进度：已播句号（1 基） */
        return 0;
    }
    LOG_W("audio_play_file s%d failed", st->played);
    return -1;
}

/* 单行分发：0 继续 / 1 end / -1 err 行或致命 / 3 老后端回退（legacy
 * 已填）。首行无 t 字段 = 老后端整包 JSON（无 \n，EOF 收尾触发）；
 * 非首行损坏行跳过（EOF 兑底，不熔断）；u 空/TTS 失败句文本照发 */
static int stream_line_dispatch(char *line, bool first, chat_stream_t *st,
                                chat_resp_t *legacy)
{
    cJSON *root = cJSON_Parse(line);
    cJSON *jt = root ? cJSON_GetObjectItem(root, "t") : NULL;
    int verdict = 0;
    if (!cJSON_IsString(jt) || !jt->valuestring) {
        /* 无 t/解析失败：首行 = 老后端整包 JSON 信封（单行无 \n，首行
         * 或 EOF 收尾触发）→ 解析成功回退老路径；非首行损坏行跳过
         * （EOF 兑底不熔断） */
        cJSON_Delete(root);
        if (first)
            return chat_parse_legacy(line, legacy) == 0 ? 3 : -1;
        if (!root) LOG_W("chat stream line parse failed: %.32s", line);
        return 0;
    }
    if (strcmp(jt->valuestring, "meta") == 0) {
        cJSON *jr = cJSON_GetObjectItem(root, "roundId");
        if (cJSON_IsString(jr) && jr->valuestring)
            strncpy(st->round_id, jr->valuestring, sizeof(st->round_id) - 1);
        /* 识别文本回显（meta 携带，ASR 后即发）：THINKING 态重刷
         * 「你说：…」——录音期页面静默的补偿反馈（mic 闭环证据） */
        cJSON *jtr = cJSON_GetObjectItem(root, "transcript");
        if (cJSON_IsString(jtr) && jtr->valuestring) {
            strncpy(s_heard, jtr->valuestring, sizeof(s_heard) - 1);
            s_heard[sizeof(s_heard) - 1] = '\0';
            s_meta_got = true;
            if (s_active && s_state == CHAT_STATE_THINKING)
                set_state(CHAT_STATE_THINKING, NULL);   /* 同态重入=重刷 */
        }
        /* warmup 可选（scenario 首轮中文预热，首句起播屏显用后即清） */
        cJSON *jw = cJSON_GetObjectItem(root, "warmup");
        if (cJSON_IsString(jw) && jw->valuestring) {
            strncpy(s_warmup, jw->valuestring, sizeof(s_warmup) - 1);
            s_warmup[sizeof(s_warmup) - 1] = '\0';
        }
    } else if (strcmp(jt->valuestring, "s") == 0) {
        cJSON *jx = cJSON_GetObjectItem(root, "x");
        cJSON *ju = cJSON_GetObjectItem(root, "u");
        const char *x = cJSON_IsString(jx) && jx->valuestring ? jx->valuestring : "";
        const char *u = cJSON_IsString(ju) && ju->valuestring[0] ? ju->valuestring : NULL;
        bool have_audio = false;
        if (u && st->total < CHAT_SENT_MAX) {
            const char *base = strrchr(u, '/');
            base = base ? base + 1 : u;
            char path[CHAT_SENT_NAME_MAX + 16];
            snprintf(path, sizeof(path), AUDIO_DIR "/%s", base);
            if (chat_download_to(u, path) == 0) {
                strncpy(st->files[st->total], base, CHAT_SENT_NAME_MAX - 1);
                have_audio = true;
                st->total++;
            } else {
                LOG_W("句 s%d 下载失败，降级跳过音频", st->total);
            }
        }
        /* 末句驻留逐句更新（屏显克制：句句覆盖，end 后即末句） */
        strncpy(s_reply, x, sizeof(s_reply) - 1);
        s_reply[sizeof(s_reply) - 1] = '\0';
        /* 全句拼接（IDLE 回看）：空格连接，缓冲顶则截断（护栏 ≤6 句） */
        if (s_full_reply[0])
            strlcat(s_full_reply, " ", sizeof(s_full_reply));
        strlcat(s_full_reply, x, sizeof(s_full_reply));
        if (!st->started) {
            if (have_audio) {
                st->started = true;
                set_state(CHAT_STATE_PLAYING,
                          s_warmup[0] ? s_warmup : x);
                haptic_event(HAPTIC_PASS);   /* 回复到达（老路径同时点） */
                stream_play_next(st);         /* 首句下载完即起播（首响红利） */
            } else {
                set_state(CHAT_STATE_PLAYING, x);  /* TTS 降级句：文本先行 */
            }
        } else {
            set_state(CHAT_STATE_PLAYING, x);  /* 逐句局刷（墨水屏节奏红利） */
            /* 播放快于到达兑底：读流中前句播完且已落地下一句则续播 */
            if (have_audio && !audio_is_playing() && st->played < st->total)
                stream_play_next(st);
        }
    } else if (strcmp(jt->valuestring, "end") == 0) {
        chat_parse_hits(cJSON_GetObjectItem(root, "wordHits"), legacy);
        /* P1-1 commands：replay 触发位（本轮播完后重播，句文件保留）；
         * emotion 一期存不消费（协议位预留，屏显克制红线） */
        cJSON *jcmds = cJSON_GetObjectItem(root, "commands");
        if (cJSON_IsArray(jcmds)) {
            cJSON *jcmd;
            cJSON_ArrayForEach(jcmd, jcmds) {
                cJSON *ja = cJSON_GetObjectItem(jcmd, "a");
                if (cJSON_IsString(ja) && ja->valuestring &&
                    strcmp(ja->valuestring, "replay") == 0) {
                    s_replay_req = true;
                    break;
                }
            }
        }
        verdict = 1;
    } else if (strcmp(jt->valuestring, "err") == 0) {
        LOG_W("chat stream err line: %.64s", line);
        verdict = -1;
    }
    cJSON_Delete(root);
    return verdict;
}

/* 流式整轮：multipart 写（chat_upload 同构）→ NDJSON 增量读（行缓冲
 * 静态，read 3s 超时回环查打断位）→ 句级流水线（下载即播/续播）→
 * end 后播完收尾。TCP 反压天然限流：下载句文件时不 read，后端写流
 * 阻塞节流（句序安全）。返回 0 正常完成 / -1 失败（net_fail 语义）/
 * 2 用户打断（新轮触发位已置，静默）/ 3 老后端回退（legacy 已填，
 * 走保留的旧整段路径后半段） */
static int run_round_stream(const uint8_t *wav, size_t len, chat_resp_t *legacy)
{
    static const char BND[] = "InkWordChat1886";
    char head[192], tail[48], url[CHAT_URL_MAX], ctype[64];
    int hl = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"chat.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", BND);
    int fl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", BND);
    snprintf(url, sizeof(url), "%s/api/device/chat", sync_get_base_url());
    if (s_req.mode[0]) {                     /* 模式 query 同老路径 + 流式协商位 */
        strlcat(url, "?mode=", sizeof(url));
        strlcat(url, s_req.mode, sizeof(url));
        if (s_req.scenario[0]) {
            strlcat(url, "&scenarioId=", sizeof(url));
            strlcat(url, s_req.scenario, sizeof(url));
        }
        strlcat(url, "&stream=1", sizeof(url));
    } else {
        strlcat(url, "?stream=1", sizeof(url));
    }
    snprintf(ctype, sizeof(ctype),
             "multipart/form-data; boundary=%s", BND);

    chat_stream_t st;                        /* 栈 ~370B（句名表），任务串行独占 */
    memset(&st, 0, sizeof(st));
    memset(legacy, 0, sizeof(*legacy));

    esp_http_client_config_t cfg;
    chat_fill_cfg(&cfg, url, CHAT_UPLOAD_TIMEOUT_MS);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", ctype);
    esp_http_client_set_header(client, "X-Device-Key", sync_get_device_key());

    esp_err_t err = esp_http_client_open(client, hl + (int)len + fl);
    if (err == ESP_OK) {
        int w = esp_http_client_write(client, head, hl);
        if (w == hl) w = esp_http_client_write(client, (const char *)wav, (int)len);
        if (w == (int)len) w = esp_http_client_write(client, tail, fl);
        if (w != fl) err = ESP_FAIL;
    }
    int status = 0;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0)
        status = esp_http_client_get_status_code(client);
    if (err != ESP_OK || status != 200) {
        LOG_W("chat stream open failed: err=%d status=%d", err, status);
        esp_http_client_cleanup(client);
        return -1;
    }

    set_state(CHAT_STATE_THINKING, NULL);    /* 请求已发在读流 */
    /* 读流短超时：周期回环查打断位（barge-in 3s 内拾起）；写阶段已
    完成，原上传 45s 长超时不再适用 */
    esp_http_client_set_timeout_ms(client, CHAT_STREAM_READ_MS);

    int verdict = 0, dead = 0, waited = 0;
    bool first = true;
    char rbuf[256];
    for (;;) {
        if (s_stop_play || !s_active) {       /* barge-in：读流期/模式退出 */
            esp_http_client_cleanup(client);  /* 关流（后端 RequestAborted 截断） */
            s_replay_req = false;             /* end 已到再打断：replay 作废 */
            stream_cleanup(&st);
            if (s_active) chat_abort_post(st.round_id);  /* 仅用户打断发 */
            return 2;
        }
        int n = esp_http_client_read(client, rbuf, sizeof(rbuf));
        ui_chat_anim_tick();                  /* 超时回环点驱动涟漪（内部节拍防抖） */
        if (n > 0) {
            dead = 0;
            for (int i = 0; i < n && verdict == 0; i++) {
                if (rbuf[i] == '\n') {
                    s_line[s_line_len] = '\0';
                    verdict = stream_line_dispatch(s_line, first, &st, legacy);
                    first = false;
                    s_line_len = 0;
                } else if (s_line_len < (int)sizeof(s_line) - 1) {
                    s_line[s_line_len++] = rbuf[i];
                } else {
                    LOG_W("chat stream line overflow");
                    verdict = -1;
                }
            }
        } else if (n == 0) {                  /* 连接关闭：尾行收尾 */
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                verdict = stream_line_dispatch(s_line, first, &st, legacy);
                s_line_len = 0;
            }
            break;                            /* 无 end 的 EOF = 异常截断 → 失败 */
        } else {                              /* 超时/瞬时错误：限次重试 */
            if (++dead >= CHAT_STREAM_IDLE_MAX) {
                LOG_W("chat stream idle timeout");
                verdict = -1;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    esp_http_client_cleanup(client);
    s_line_len = 0;

    if (verdict == 3) return 3;               /* 老后端：首行即整包，无句文件 */

    if (verdict == 1) {                       /* end：续播至完 + 命中缓存 */
        for (int i = 0; i < legacy->hits_n; i++) {   /* 命中缓存（IDLE 计数行） */
            strncpy(s_hits_text[i], legacy->hits_text[i], CHAT_HIT_TEXT_MAX - 1);
            s_hits_text[i][CHAT_HIT_TEXT_MAX - 1] = '\0';
            strncpy(s_hits_cloud[i], legacy->hits_cloud[i], CHAT_HIT_CLOUD_MAX - 1);
            s_hits_cloud[i][CHAT_HIT_CLOUD_MAX - 1] = '\0';
        }
        s_hits_n = legacy->hits_n;
        while (st.played < st.total && s_active && !s_stop_play &&
               waited < CHAT_STREAM_PLAY_MAX_MS) {
            if (!audio_is_playing()) {
                if (stream_play_next(&st) != 0) break;   /* 投播失败不重试 */
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
                waited += 100;
            }
        }
        /* P1-1 replay：commands.replay 且句文件齐备未打断 → 文件表移交
         * 重播（删除推迟）；否则常规删净 */
        if (s_replay_req && st.total > 0 && !s_stop_play && s_active) {
            for (int i = 0; i < st.total; i++)
                memcpy(s_replay_files[i], st.files[i], CHAT_SENT_NAME_MAX);
            s_replay_total = st.total;
            stream_stop_wait();               /* 只停播，句文件保留 */
        } else {
            s_replay_req = false;
            stream_cleanup(&st);
        }
        s_warmup[0] = '\0';                   /* 预热仅首轮 speaking 态可见 */
        LOG_I("chat stream round done: mode=%s reply=%.32s",
              s_req.mode[0] ? s_req.mode : "free", s_reply);
        return 0;
    }

    /* err 行 / 超时 / EOF 无 end：已播句停播删文件后 net_fail 语义 */
    if (st.total > 0) stream_cleanup(&st);
    s_replay_req = false;                     /* P1-1：异常路径 replay 作废 */
    return -1;
}

/* ---- 单轮对话：录音 → 上传 → 下载 → 播放 → 清理 ---- */
static void run_round(void)
{
    if (!wifi_is_connected()) { net_fail(); return; }

    s_hits_n = 0;                           /* 新一轮：旧命中作废 */
    s_stop_play = false;                    /* 新一轮：旧打断位作废（兼修老路径
                                               漏清：打断重说后新回复被立即静音杀） */
    s_full_reply[0] = '\0';                 /* 新一轮：全句拼接/句号归零 */
    s_sent_no = 0;
    s_heard[0] = '\0';                      /* 新一轮：识别回显/时长归零 */
    s_meta_got = false;
    s_rec_ms = 0;
    replay_purge();                         /* 新一轮：遗留 replay 句文件作废 */
    set_state(CHAT_STATE_RECORDING, NULL);
    haptic_event(HAPTIC_KEYPRESS);          /* 录音起一短震 */

    uint8_t *wav = NULL;
    size_t wav_len = 0;
    int rc = mic_recorder_record(&wav, &wav_len, &s_cancel, &s_send_now,
                                 CHAT_MAX_MS);
    if (!s_active) { free(wav); return; }   /* 模式已退：静默收尾 */
    if (rc == -2) {                          /* 用户取消：回 idle */
        set_state(CHAT_STATE_IDLE, NULL);
        return;
    }
    if (rc != 0) {                           /* -3 无话音 / -1 录音失败 */
        haptic_event(rc == -3 ? HAPTIC_KEYPRESS : HAPTIC_ERROR);
        set_state(CHAT_STATE_IDLE,
                  rc == -3 ? "未听到 · 按中键重试" : "录音失败 · 按中键重试");
        return;
    }

    set_state(CHAT_STATE_UPLOADING, NULL);
    s_rec_ms = (int)((wav_len - 44) * 1000 / (16000 * 2));  /* 16bit/mono */
    if (!s_send_now)
        haptic_event(HAPTIC_KEYPRESS);      /* VAD 自动断句：触觉确认（录音期
                                               禁刷屏，屏幕不能给反馈） */
    static chat_resp_t resp;                /* 栈节流（+wordHits ~330B），任务串行独占 */
    /* P0-1 流式优先：NDJSON 增量读 + 句级流水线；首行无 t 字段 =
     * 老后端整包 JSON 信封，直接解析已到响应走老后半段（零重传） */
    int verdict = run_round_stream(wav, wav_len, &resp);
    free(wav);
    if (!s_active) return;
    if (verdict == 0) {                     /* 流式完成：句文件已删净、收尾已做 */
        if (s_active) set_state(CHAT_STATE_IDLE, NULL);
        return;
    }
    if (verdict == 2) return;               /* 用户打断：新轮触发位已置，静默 */
    if (verdict != 3) { net_fail(); return; }   /* err 行/超时/HTTP 失败 */

    /* ---- 老后端回退（P0-1）：保留的旧整段路径后半段（响应已解析） ---- */

    /* 末句回复缓存（屏显驻留）；场景首轮预热单独缓存（speaking 态
     * 屏显，用后即清——仅首轮可见，后续轮驻留 reply） */
    strncpy(s_reply, resp.reply, sizeof(s_reply) - 1);
    s_reply[sizeof(s_reply) - 1] = '\0';
    strlcpy(s_full_reply, resp.reply, sizeof(s_full_reply)); /* 回看同步 */
    strncpy(s_warmup, resp.warmup, sizeof(s_warmup) - 1);
    s_warmup[sizeof(s_warmup) - 1] = '\0';
    for (int i = 0; i < resp.hits_n; i++) { /* A3 生词命中缓存（IDLE 计数行） */
        strncpy(s_hits_text[i], resp.hits_text[i], CHAT_HIT_TEXT_MAX - 1);
        s_hits_text[i][CHAT_HIT_TEXT_MAX - 1] = '\0';
        strncpy(s_hits_cloud[i], resp.hits_cloud[i], CHAT_HIT_CLOUD_MAX - 1);
        s_hits_cloud[i][CHAT_HIT_CLOUD_MAX - 1] = '\0';
    }
    s_hits_n = resp.hits_n;

    if (!resp.audio_url[0]) {                /* TTS 降级：文本已到即反馈 */
        haptic_event(HAPTIC_PASS);
        set_state(CHAT_STATE_IDLE, NULL);
        return;
    }

    set_state(CHAT_STATE_THINKING, NULL);
    rc = chat_download(resp.audio_url);
    if (!s_active) { remove(CHAT_TMP_PATH); return; }
    if (rc != 0) { net_fail(); return; }

    set_state(CHAT_STATE_PLAYING, s_warmup[0] ? s_warmup : NULL);
    haptic_event(HAPTIC_PASS);
    if (audio_play_file(CHAT_TMP_PATH) == 0) {
        int waited = 0;
        while (audio_is_playing() && waited < CHAT_PLAY_MAX_MS &&
               !s_stop_play && s_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        audio_stop();                        /* 打断/超时兜底（幂等） */
        /* 等音频任务退出播放循环再删临时文件（fd 打开时 remove 未定义） */
        waited = 0;
        while (audio_is_playing() && waited < 2000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited += 50;
        }
    }
    remove(CHAT_TMP_PATH);                   /* 播完即删（零残留） */
    s_warmup[0] = '\0';                      /* 预热仅首轮 speaking 态可见 */
    LOG_I("chat round done: mode=%s reply=%.32s",
          s_req.mode[0] ? s_req.mode : "free", s_reply);

    if (s_active) set_state(CHAT_STATE_IDLE, NULL);
}

/* ---- A3 生词收藏：SET 触发位拾起，逐条推 /sync/collect（云端收藏，
 *      App/下次同步可见；命中词不一定在当前词书，本地收藏列表一期
 *      不建 cloudId→index 映射）；成功清零防重复，失败一长震不重试
 *      （下次对话再收） ---- */
static void collect_hits(void)
{
    char msg[48];
    int n = s_hits_n, ok = 0;
    for (int i = 0; i < n && s_active; i++)
        if (sync_push_collect(s_hits_cloud[i], true) == 0) ok++;
    if (!s_active) return;
    if (ok == n && n > 0) {
        haptic_event(HAPTIC_PASS);
        s_hits_n = 0;                        /* 防重复收藏 */
        snprintf(msg, sizeof(msg), "已收藏 %d 词", n);
    } else {
        haptic_event(HAPTIC_ERROR);
        snprintf(msg, sizeof(msg), "收藏失败 %d/%d · 稍后再试", ok, n);
    }
    set_state(CHAT_STATE_IDLE, msg);
}

static void chat_task(void *arg)
{
    (void)arg;
    while (s_active) {
        if (s_collect_req) {                 /* A3 SET 收藏（任务串行推） */
            s_collect_req = false;
            collect_hits();
            continue;
        }
        if (s_replay_req && !s_round_req) {  /* P1-1 replay（新轮优先作废） */
            replay_round();
            continue;
        }
        if (!s_round_req) {
            vTaskDelay(pdMS_TO_TICKS(20));   /* 轮询触发位（延迟无感） */
            continue;
        }
        s_round_req = false;
        run_round();
    }
    /* 模式退出收口：清临时文件与状态（渲染已由 s_active 拦截） */
    remove(CHAT_TMP_PATH);
    replay_purge();                          /* P1-1 replay 遗留句文件 */
    s_state = CHAT_STATE_IDLE;
    LOG_I("chat task exited");
    vTaskDelete(NULL);
}

/* ---- 公共 API ---- */

void chat_mode_enter(const chat_request_t *req)
{
    if (s_active) return;                    /* 幂等 */
    s_active = true;
    s_state = CHAT_STATE_IDLE;
    s_reply[0] = '\0';
    s_warmup[0] = '\0';
    s_hits_n = 0;
    memset(&s_req, 0, sizeof(s_req));
    if (req) {
        strlcpy(s_req.mode, req->mode, sizeof(s_req.mode));
        strlcpy(s_req.scenario, req->scenario, sizeof(s_req.scenario));
        strlcpy(s_req.title, req->title, sizeof(s_req.title));
    }
    s_round_req = s_cancel = s_send_now = s_stop_play = false;
    s_collect_req = false;
    if (xTaskCreate(chat_task, "chat", CHAT_TASK_STACK, NULL,
                    CHAT_TASK_PRIO, NULL) != pdPASS) {
        s_active = false;
        LOG_E("xTaskCreate chat failed");
    }
}

void chat_mode_request_exit(void)
{
    s_cancel = true;                         /* 录音循环逐块检查 */
    s_stop_play = true;                      /* 播放等待循环出界 */
    s_round_req = false;
    s_active = false;                        /* 任务循环边界静默收尾 */
    audio_stop();
}

bool chat_mode_on_button(nav_key_t id, button_event_t event)
{
    if (!s_active) return true;              /* 未激活吞键（不应发生） */

    /* 退出：长按中 / RST（模式内 RST 无回首语义，任意按退出） */
    if ((id == NAV_CENTER && event == BUTTON_EVENT_LONG_PRESS) ||
        id == NAV_RST)
        return false;                        /* 交编排层执行退出 */

    if (event != BUTTON_EVENT_SHORT_PRESS) return true;  /* 其余长按忽略 */
    /* A3 SET 收藏：idle/netfail 态且有命中时置位（任务串行推，按键零阻塞） */
    if (id == NAV_SET) {
        if ((s_state == CHAT_STATE_IDLE || s_state == CHAT_STATE_NETFAIL) &&
            s_hits_n > 0 && !s_collect_req) {
            s_collect_req = true;
            haptic_event(HAPTIC_KEYPRESS);
        }
        return true;
    }
    if (id != NAV_CENTER) return true;                   /* 其余短按忽略 */

    switch (s_state) {
    case CHAT_STATE_IDLE:
    case CHAT_STATE_NETFAIL:
        s_cancel = false;
        s_send_now = false;
        s_round_req = true;                  /* 任务轮询拾起新一轮 */
        break;
    case CHAT_STATE_RECORDING:
        s_send_now = true;                   /* 说完立即发（保留已录） */
        break;
    case CHAT_STATE_UPLOADING:
        break;                               /* multipart 写流不可中断（秒级），静默 */
    case CHAT_STATE_THINKING:
        /* P0-1 读流期可打断：读循环 3s 内拾起（abort 截后端生成），
         * 打断重说与 PLAYING 期同款 */
        s_stop_play = true;
        s_cancel = false;
        s_send_now = false;
        s_round_req = true;
        break;
    case CHAT_STATE_PLAYING:
        s_stop_play = true;                  /* 打断重说（计划验收项） */
        audio_stop();
        s_cancel = false;
        s_send_now = false;
        s_round_req = true;
        break;
    }
    return true;
}

bool chat_mode_is_active(void)          { return s_active; }
chat_state_t chat_mode_state(void)      { return s_state; }
const char *chat_mode_reply(void)       { return s_reply; }
const char *chat_mode_warmup(void)      { return s_warmup; }
const char *chat_mode_title(void)      { return s_req.title[0] ? s_req.title : "AI Chat"; }
int chat_mode_wordhit_count(void)      { return s_hits_n; }

/* ---- T2.2 页面路由接入：render=NULL 自管局刷先例 ---- */
/* 首帧（push 的 enter 回调）：状态栏+内容区整屏全刷一次；环路内仅
 * 内容区局刷（ui_render_chat 由 set_state 回调驱动），进/出各一次
 * 全刷红线（原 main.cpp base_render 的 MODE_CHAT 分支迁此） */
static void chat_page_enter(void)
{
    ui_draw_status(MODE_CHAT);
    ui_render_chat(chat_mode_state(), chat_mode_reply());
    epd_gfx_flush();
}

/* 栈顶按键：全转发 chat_mode_on_button；false=请求退出，退出编排
 * 内聚于此（原 main.cpp on_button 的 MODE_CHAT 分支迁此） */
static bool chat_page_on_button(nav_key_t id, button_event_t event)
{
    if (chat_mode_on_button(id, event))
        return true;
    haptic_event(HAPTIC_MODE);       /* 退出模式 50ms（进/出同档） */
    study_mode_exit_chat();          /* 内部 request_exit：任务静默收尾 */
    page_router_exit(&g_chat_page);  /* P2：pop+render_top 两连收敛；
                                       * 模式变化自然全刷回闪卡 */
    return true;
}

const page_t g_chat_page = { "chat", NULL, chat_page_on_button,
                             chat_page_enter, NULL, false };
