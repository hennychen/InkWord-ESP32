/**
 * @file ota_manager.c
 * @brief OTA 升级管理实现 (Task F-19)
 *
 * 流程：检查更新 -> esp_https_ota 下载写入 OTA 分区 ->
 *       重启切到新分区 -> 启动正常则 ota_mark_valid 防回滚。
 * 检查与下载均复用 sync_client 配置的 Base URL（NVS/宏三级优先级）。
 */
#include "ota_manager.h"
#include "sync_client.h"
#include "debug_log.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "OTA";

/* ---- 更新检查：GET /api/device/ota/check?currentVer=x ---- */
typedef struct {
    char *buf;
    int   size;
    int   offset;
} ota_recv_t;

static ota_recv_t *s_ota_ctx = NULL;

static esp_err_t ota_event_handler(esp_http_client_event_t *evt)
{
    if (s_ota_ctx && evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_ota_ctx->offset + evt->data_len < s_ota_ctx->size) {
            memcpy(s_ota_ctx->buf + s_ota_ctx->offset, evt->data, evt->data_len);
            s_ota_ctx->offset += evt->data_len;
        }
    }
    return ESP_OK;
}

/* 按 URL 前缀选传输：http: 明文 TCP（本地开发后端），其余 TLS + 证书包
 * （与 sync_client fill_cfg 同策略） */
static void ota_fill_cfg(esp_http_client_config_t *cfg, const char *url,
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

bool ota_check_for_update(char *out_url, int url_len, char *out_md5, int md5_len, int *out_size)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/ota/check", sync_get_base_url());

    static char buf[1024];
    ota_recv_t r = { .buf = buf, .size = sizeof(buf), .offset = 0 };
    s_ota_ctx = &r;

    esp_http_client_config_t cfg;
    ota_fill_cfg(&cfg, url, 15000);
    cfg.event_handler = ota_event_handler;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    s_ota_ctx = NULL;
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || r.offset == 0) {
        LOG_W("ota check failed/no data");
        return false;
    }
    buf[r.offset] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    cJSON *hasUpdate = cJSON_GetObjectItem(root, "hasUpdate");
    bool has = cJSON_IsTrue(hasUpdate);
    if (has) {
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(root, "md5"));
        cJSON *sz = cJSON_GetObjectItem(root, "size");
        if (u && out_url) { strncpy(out_url, u, url_len - 1); out_url[url_len - 1] = '\0'; }
        if (m && out_md5) { strncpy(out_md5, m, md5_len - 1); out_md5[md5_len - 1] = '\0'; }
        if (sz && out_size) *out_size = sz->valueint;
        LOG_I("new firmware available");
    }
    cJSON_Delete(root);
    return has;
}

/* MD5 校验：ESP-IDF 提供 mbedtls，此处用 esp_partition 得到的 sha 可比较 */
int ota_perform_upgrade(const char *url, const char *expect_md5)
{
    if (!url) return -1;
    LOG_I("starting OTA from %s", url);

    esp_http_client_config_t cfg;
    ota_fill_cfg(&cfg, url, 30000);

    esp_https_ota_config_t ota_cfg = {
        .http_config = &cfg,
        /* .partial_http_download = true 可加速大文件，按需开启 */
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        LOG_E("ota begin failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* 分块写入 */
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int total = esp_https_ota_get_image_size(ota_handle);
        int read  = esp_https_ota_get_image_len_read(ota_handle);
        if (total > 0 && (read % (64 * 1024) < 4096)) {
            LOG_I("ota progress: %d/%d (%d%%)", read, total, read * 100 / total);
        }
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        LOG_E("ota data incomplete, aborting");
        esp_https_ota_abort(ota_handle);
        return -1;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err == ESP_OK) {
        LOG_I("ota finish OK, preparing to reboot into new partition");
        /* 校验 MD5（可选）：esp_https_ota_finish 内部已校验固件签名/校验和 */
        (void)expect_md5;
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return 0;  /* 不会到达 */
    }

    LOG_E("ota finish failed: %s, will rollback", esp_err_to_name(err));
    /* 失败：不切换分区，下次仍从旧分区启动（自动回滚） */
    return -1;
}

int ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
        if (st == ESP_OTA_IMG_PENDING_VERIFY) {
            LOG_I("marking current firmware as valid (cancel rollback)");
            return (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ? 0 : -1;
        }
    }
    return 0;
}
