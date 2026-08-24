/**
 * @file audio_sync.c
 * @brief 词条发音音频同步实现 (P0C，2026-08-24)
 *
 * 遍历词池（word_parser），对 cloud_id 非空且 /sdcard/audio/{cloud_id}.mp3
 * 缺失的词串行 GET /api/device/audio/{cloud_id}.mp3：事件回调流式直写
 * SD 临时文件，成功后 rename（防半文件；中止/断电残留 .tmp 不参与
 * 存在性判断，下次重下覆盖）。复用 sync_client 配置源（Base URL/
 * X-Device-Key）与 http/TLS 分流策略（ota_manager ota_fill_cfg 同款）。
 *
 * 失败护栏：404 = 云端尚未合成（TtsJob 次日补齐）非错误；网络级失败
 * 连续 5 次中止本轮（Wi-Fi 掉线不死等 15s×N 超时）。菜单徽标
 * 「缺N/M」读缓存（任务结束时更新），缺文件不阻断学习闭环。
 */
#include "audio_sync.h"
#include "debug_log.h"
#include "gpio_config.h"       /* AUDIO_DIR */
#include "word_parser.h"
#include "sync_client.h"
#include "storage_manager.h"
#include "haptic.h"
#include "wifi_manager.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "AUDIO_SYNC";

#define ASYNC_TASK_STACK     (6 * 1024)
#define ASYNC_TASK_PRIO      (4)     /* 与音频任务同级，低于 btn_scan(5) */
#define ASYNC_HTTP_TIMEOUT_MS 15000
#define ASYNC_MAX_BYTES      (1024 * 1024) /* 单词条 MP3 ≈4KB，>1MB 视为异常响应 */
#define ASYNC_NET_FAIL_MAX   (5)     /* 连续网络级失败熔断阈值 */

static volatile bool s_running = false;
static volatile int  s_done    = 0;  /* 本次已处理缺失词数 */
static volatile int  s_missing = -1; /* 缓存缺失数（-1 未统计） */

/* 单词条云端路径（13+1+36+4+1=55 ≤ path 系列缓冲） */
static void word_path(char *buf, size_t n, const char *cloud_id)
{
    snprintf(buf, n, "%s/%s.mp3", AUDIO_DIR, cloud_id);
}

/* 按 URL 前缀选传输：http: 明文 TCP（本地开发后端），其余 TLS + 证书包
 * （sync_client fill_cfg / ota_manager ota_fill_cfg 同策略） */
static void as_fill_cfg(esp_http_client_config_t *cfg, const char *url,
                        int timeout_ms)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = timeout_ms;
    cfg->keep_alive_enable = true;
    if (strncmp(url, "http:", 5) == 0) {
        cfg->transport_type = HTTP_TRANSPORT_OVER_TCP;
    } else {
        cfg->transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg->crt_bundle_attach = arduino_esp_crt_bundle_attach;
    }
}

/* 流式下载上下文（cfg.user_data 传入事件回调，免全局变量竞态） */
typedef struct {
    FILE  *f;
    size_t bytes;
    bool   bad;    /* 写失败/超大：后续数据不再落盘 */
} dl_ctx_t;

static esp_err_t dl_event(esp_http_client_event_t *evt)
{
    dl_ctx_t *ctx = (dl_ctx_t *)evt->user_data;
    if (ctx && evt->event_id == HTTP_EVENT_ON_DATA && !ctx->bad) {
        if (ctx->bytes + evt->data_len > ASYNC_MAX_BYTES ||
            fwrite(evt->data, 1, evt->data_len, ctx->f) != (size_t)evt->data_len) {
            ctx->bad = true;
            return ESP_OK;
        }
        ctx->bytes += evt->data_len;
    }
    return ESP_OK;
}

/**
 * 下载单词条 MP3 到 SD（临时文件 + rename 防半文件）。
 * @return 0 成功；1 云端 404（未合成，跳过）；-1 一般失败；-2 网络级失败。
 */
static int download_one(const char *cloud_id)
{
    char final_path[80], tmp_path[88], url[192];
    word_path(final_path, sizeof(final_path), cloud_id);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    snprintf(url, sizeof(url), "%s/api/device/audio/%s.mp3",
             sync_get_base_url(), cloud_id);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        LOG_W("fopen %s failed (errno=%d, 无 SD?)", tmp_path, errno);
        return -1;
    }

    dl_ctx_t ctx = { .f = f };
    esp_http_client_config_t cfg;
    as_fill_cfg(&cfg, url, ASYNC_HTTP_TIMEOUT_MS);
    cfg.event_handler = dl_event;
    cfg.user_data = &ctx;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(f);
        remove(tmp_path);
        return -2;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "X-Device-Key", sync_get_device_key());

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    fclose(f);

    if (!ctx.bad && err == ESP_OK && status == 200 && ctx.bytes > 0) {
        if (rename(tmp_path, final_path) == 0) return 0;
        LOG_W("rename %s failed (errno=%d)", final_path, errno);
        remove(tmp_path);
        return -1;
    }
    remove(tmp_path);
    if (err != ESP_OK) return -2;      /* 连接/超时级失败（熔断计数） */
    if (status == 404) return 1;       /* 云端尚未合成，TtsJob 次日补 */
    if (!ctx.bad)
        LOG_W("audio %s status=%d bytes=%u", cloud_id, status, (unsigned)ctx.bytes);
    return -1;
}

static void audio_sync_task(void *arg)
{
    (void)arg;
    /* 目录兜底（EEXIST 无害；无 SD 时后续 fopen 全失败自然收敛为 fail） */
    if (mkdir(AUDIO_DIR, 0775) != 0 && errno != EEXIST)
        LOG_W("mkdir %s errno=%d（无 SD 卡）", AUDIO_DIR, errno);

    int total = word_parser_get_count();
    int cloud = 0, missing = 0, ok = 0, miss404 = 0, fail = 0;
    int net_streak = 0;
    bool aborted = false;

    for (int i = 0; i < total; i++) {
        const WordEntry *w = word_parser_get(i);
        if (!w || !w->cloud_id[0]) continue;
        cloud++;

        char path[80];
        word_path(path, sizeof(path), w->cloud_id);
        if (storage_file_exists(path)) continue;

        missing++;
        s_done = missing;
        if (missing % 10 == 1)
            LOG_I("syncing %d/%d ...", missing, total);

        int rc = download_one(w->cloud_id);
        if (rc == 0 || rc == 1) {
            net_streak = 0;
            if (rc == 0) ok++; else miss404++;
        } else {
            fail++;
            if (rc == -2 && ++net_streak >= ASYNC_NET_FAIL_MAX) {
                LOG_W("network failures %d in a row, abort", net_streak);
                aborted = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));   /* 礼让，长任务不占满核 */
    }

    /* 任务统计更新徽标缓存（404/失败词仍缺失），免再扫一遍 */
    s_missing = missing - ok;
    LOG_I("audio sync done: ok=%d miss404=%d fail=%d aborted=%d "
          "(cloud=%d missing=%d)", ok, miss404, fail, aborted, cloud, missing);
    if (missing > 0) haptic_event(HAPTIC_PASS);   /* 有动作才双短震反馈 */

    s_running = false;
    vTaskDelete(NULL);
}

int audio_sync_cloud_total(void)
{
    int n = 0;
    int total = word_parser_get_count();
    for (int i = 0; i < total; i++) {
        const WordEntry *w = word_parser_get(i);
        if (w && w->cloud_id[0]) n++;
    }
    return n;
}

int audio_sync_refresh_stats(void)
{
    if (mkdir(AUDIO_DIR, 0775) != 0 && errno != EEXIST)
        return -1;    /* /sdcard 不在 VFS = 无卡 */

    int n = 0;
    int total = word_parser_get_count();
    for (int i = 0; i < total; i++) {
        const WordEntry *w = word_parser_get(i);
        if (!w || !w->cloud_id[0]) continue;
        char path[80];
        word_path(path, sizeof(path), w->cloud_id);
        if (!storage_file_exists(path)) n++;
    }
    s_missing = n;
    return n;
}

int  audio_sync_missing_cached(void) { return s_missing; }
bool audio_sync_is_running(void)     { return s_running; }
int  audio_sync_progress(void)       { return s_done; }

int audio_sync_start(void)
{
    if (s_running) return -1;
    if (!wifi_is_connected() || !sync_has_device_key()) return -1;
    if (mkdir(AUDIO_DIR, 0775) != 0 && errno != EEXIST) return -1;  /* 无 SD */

    /* 先置位再建任务：任务可能瞬间跑完（无缺失）先行清位 */
    s_running = true;
    s_done = 0;
    if (xTaskCreate(audio_sync_task, "audio_sync", ASYNC_TASK_STACK,
                    NULL, ASYNC_TASK_PRIO, NULL) != pdPASS) {
        s_running = false;
        LOG_E("xTaskCreate audio_sync failed");
        return -1;
    }
    return 0;
}
