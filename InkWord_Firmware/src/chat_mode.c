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
 */
#include "chat_mode.h"
#include "debug_log.h"
#include "gpio_config.h"       /* AUDIO_DIR */
#include "audio_player.h"
#include "mic_recorder.h"
#include "sync_client.h"       /* base_url / device_key 配置源 */
#include "haptic.h"
#include "wifi_manager.h"

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

#define CHAT_TASK_STACK     (6 * 1024)
#define CHAT_TASK_PRIO      (4)     /* pron/audio_sync 同级，低于 btn_scan(5) */
#define CHAT_MAX_MS         (10000) /* 对话档录音上限（P1 跟读 3s） */
#define CHAT_UPLOAD_TIMEOUT_MS (45000) /* 含后端 ASR+LLM+TTS 全链路 */
#define CHAT_DL_TIMEOUT_MS  (15000)
#define CHAT_MP3_MAX_BYTES  (512 * 1024) /* 回复 ≤2 句 ≤40 词，超此为异常 */
#define CHAT_PLAY_MAX_MS    (30000) /* 播放等待兜底（打断/超时出循环） */
#define CHAT_TMP_PATH       (AUDIO_DIR "/chat_tmp.mp3")
#define CHAT_RESP_MAX       (1536)  /* reply 260 + audioUrl + 信封余量 */

/* ---- 模块状态（单写者=对话任务，按键上下文仅写触发/取消位） ---- */
static volatile bool s_active    = false; /* 模式在（任务生命周期域） */
static volatile bool s_round_req = false; /* 中键触发新一轮 */
static volatile bool s_cancel    = false; /* 录音取消位（丢弃） */
static volatile bool s_send_now  = false; /* 录音手动断（说完即发，保留） */
static volatile bool s_stop_play = false; /* 播放打断位 */
static volatile chat_state_t s_state = CHAT_STATE_IDLE;
static char s_reply[CHAT_REPLY_MAX];      /* 末句回复（屏显驻留） */

/* main.cpp 导出（C++ → C，ui_render_pron 同款先例） */
extern void ui_render_chat(chat_state_t st, const char *text);

/* ---- 后端响应子集（transcript 设备端不消费，屏显克制） ---- */
typedef struct {
    char reply[CHAT_REPLY_MAX];
    char audio_url[160];       /* "/api/device/audio/chat_{ts}.mp3" */
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

/* ---- 上传 WAV 对话（multipart 三段流式写，mic_recorder_upload 同构；
 *      响应信封 { code, data:{ transcript, reply, engine, audioUrl } }） ---- */
static int chat_upload(const uint8_t *wav, size_t len, chat_resp_t *out)
{
    static const char BND[] = "InkWordChat1886";
    static char resp[CHAT_RESP_MAX];

    char head[192], tail[48], url[192], ctype[64];
    int hl = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"chat.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", BND);
    int fl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", BND);
    snprintf(url, sizeof(url), "%s/api/device/chat", sync_get_base_url());
    snprintf(ctype, sizeof(ctype),
             "multipart/form-data; boundary=%s", BND);

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

    int resp_len = 0;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0)
        resp_len = esp_http_client_read(client, resp, sizeof(resp) - 1);

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || resp_len <= 0) {
        LOG_W("chat upload failed: err=%d status=%d len=%d", err, status, resp_len);
        return -1;
    }
    resp[resp_len] = '\0';

    int rc = -1;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(resp);
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
            rc = 0;
        }
        cJSON_Delete(root);
    }
    if (rc != 0) LOG_W("chat payload invalid");
    return rc;
}

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

static int chat_download(const char *audio_url)
{
    char url[288];
    snprintf(url, sizeof(url), "%s%s", sync_get_base_url(), audio_url);

    FILE *f = fopen(CHAT_TMP_PATH, "wb");
    if (!f) {
        LOG_W("fopen %s failed (errno=%d, 无 SD?)", CHAT_TMP_PATH, errno);
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
    remove(CHAT_TMP_PATH);
    LOG_W("chat audio download failed: err=%d status=%d bytes=%u",
          err, status, (unsigned)ctx.bytes);
    return -1;
}

/* ---- 单轮对话：录音 → 上传 → 下载 → 播放 → 清理 ---- */
static void run_round(void)
{
    if (!wifi_is_connected()) { net_fail(); return; }

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
    chat_resp_t resp;
    rc = chat_upload(wav, wav_len, &resp);
    free(wav);
    if (!s_active) return;
    if (rc != 0) { net_fail(); return; }

    /* 末句回复缓存（屏显驻留）；回复到达两短震 */
    strncpy(s_reply, resp.reply, sizeof(s_reply) - 1);
    s_reply[sizeof(s_reply) - 1] = '\0';

    if (!resp.audio_url[0]) {                /* TTS 降级：文本已到即反馈 */
        haptic_event(HAPTIC_PASS);
        set_state(CHAT_STATE_IDLE, NULL);
        return;
    }

    set_state(CHAT_STATE_THINKING, NULL);
    rc = chat_download(resp.audio_url);
    if (!s_active) { remove(CHAT_TMP_PATH); return; }
    if (rc != 0) { net_fail(); return; }

    set_state(CHAT_STATE_PLAYING, NULL);
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
    LOG_I("chat round done: reply=%.32s", s_reply);

    if (s_active) set_state(CHAT_STATE_IDLE, NULL);
}

static void chat_task(void *arg)
{
    (void)arg;
    while (s_active) {
        if (!s_round_req) {
            vTaskDelay(pdMS_TO_TICKS(20));   /* 轮询触发位（延迟无感） */
            continue;
        }
        s_round_req = false;
        run_round();
    }
    /* 模式退出收口：清临时文件与状态（渲染已由 s_active 拦截） */
    remove(CHAT_TMP_PATH);
    s_state = CHAT_STATE_IDLE;
    LOG_I("chat task exited");
    vTaskDelete(NULL);
}

/* ---- 公共 API ---- */

void chat_mode_enter(void)
{
    if (s_active) return;                    /* 幂等 */
    s_active = true;
    s_state = CHAT_STATE_IDLE;
    s_reply[0] = '\0';
    s_round_req = s_cancel = s_send_now = s_stop_play = false;
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
    case CHAT_STATE_THINKING:
        break;                               /* HTTP 回合不可中断，静默 */
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
