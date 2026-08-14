/**
 * @file sync_client.c
 * @brief HTTP 同步客户端实现 (Task F-18)，基于 esp_http_client。
 */
#include "sync_client.h"
#include "debug_log.h"
#include "wifi_manager.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "SYNC";

static char s_base_url[128] = "https://api.inkword.example.com";
static char s_device_key[64] = "";

void sync_set_base_url(const char *url)
{
    if (url) {
        strncpy(s_base_url, url, sizeof(s_base_url) - 1);
        s_base_url[sizeof(s_base_url) - 1] = '\0';
    }
}

void sync_set_device_key(const char *key)
{
    if (key) {
        strncpy(s_device_key, key, sizeof(s_device_key) - 1);
        s_device_key[sizeof(s_device_key) - 1] = '\0';
    }
}

/* 通用：给请求附加设备认证头 */
static esp_err_t set_common_headers(esp_http_client_handle_t client)
{
    esp_http_client_set_header(client, "X-Device-Key", s_device_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    return ESP_OK;
}

/* 拉取词库：GET /api/device/sync/words?version=N&count=... */
typedef struct {
    char  *buf;
    int    buf_size;
    int    offset;
} recv_ctx_t;

static recv_ctx_t *s_pull_ctx = NULL;

static esp_err_t pull_event_handler(esp_http_client_event_t *evt)
{
    if (s_pull_ctx && evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_pull_ctx->offset + evt->data_len < s_pull_ctx->buf_size) {
            memcpy(s_pull_ctx->buf + s_pull_ctx->offset, evt->data, evt->data_len);
            s_pull_ctx->offset += evt->data_len;
        }
    }
    return ESP_OK;
}

int sync_pull_words(int local_version, char *out_buf, int buf_size)
{
    if (!out_buf || buf_size <= 0) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/sync/words?version=%d",
             s_base_url, local_version);

    recv_ctx_t ctx = { .buf = out_buf, .buf_size = buf_size, .offset = 0 };
    s_pull_ctx = &ctx;

    esp_http_client_config_t cfg = {
        .url = url,
        .cert_pem = NULL,
        .crt_bundle_attach = arduino_esp_crt_bundle_attach,
        .event_handler = pull_event_handler,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    set_common_headers(client);

    esp_err_t err = esp_http_client_perform(client);
    s_pull_ctx = NULL;
    int status = esp_http_client_get_status_code(client);
    int ret = -1;

    if (err == ESP_OK && status == 200) {
        out_buf[ctx.offset] = '\0';
        /* 从响应头 X-Word-Version 取新版本号 */
        char newver[16] = {0};
        if (esp_http_client_get_header(client, "X-Word-Version", (char **)&newver)) {
            ret = atoi(newver);
        }
        if (ret < 0) ret = ctx.offset > 0 ? local_version + 1 : local_version;
        LOG_I("pulled %d bytes, new version=%d", ctx.offset, ret);
    } else {
        LOG_E("pull words failed: err=%s status=%d", esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);
    return ret;
}

/* 回传进度：POST /api/device/sync/progress */
int sync_push_progress(const ProgressItem *items, int count)
{
    if (!items || count <= 0) return -1;

    /* 组装 JSON 数组 */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "wordId", items[i].word_id);
        cJSON_AddNumberToObject(o, "quality", items[i].quality);
        cJSON_AddNumberToObject(o, "timestamp", (double)items[i].timestamp);
        cJSON_AddItemToArray(arr, o);
    }
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!body) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/sync/progress", s_base_url);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = arduino_esp_crt_bundle_attach,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    set_common_headers(client);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err == ESP_OK && status == 200) {
        LOG_I("pushed %d progress records", count);
        return 0;
    }
    LOG_E("push progress failed: %s status=%d", esp_err_to_name(err), status);
    return -1;
}

/* 心跳：POST /api/device/heartbeat */
int sync_heartbeat(int battery, const char *fw_ver)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "battery", battery);
    cJSON_AddStringToObject(o, "version", fw_ver ? fw_ver : "0.0.0");
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/heartbeat", s_base_url);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = arduino_esp_crt_bundle_attach,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    set_common_headers(client);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    return (err == ESP_OK && status == 200) ? 0 : -1;
}
