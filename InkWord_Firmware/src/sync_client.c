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

static char s_base_url[128] = "https://api.einkword.com";
static char s_device_key[64] = "";

void sync_set_base_url(const char *url)
{
    if (url) {
        strncpy(s_base_url, url, sizeof(s_base_url) - 1);
        s_base_url[sizeof(s_base_url) - 1] = '\0';
    }
}

const char *sync_get_base_url(void)
{
    return s_base_url;
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

/* 通用响应体收集器：供 pull/register 等需要读 body 的请求复用 */
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

bool sync_has_device_key(void)
{
    return s_device_key[0] != '\0';
}

const char *sync_get_device_key(void)
{
    return s_device_key;
}

/* 按 URL 前缀选传输：http: 明文 TCP（本地开发后端），其余 TLS + 证书包 */
static void fill_cfg(esp_http_client_config_t *cfg, const char *url, int timeout_ms)
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

/* 首次注册：POST /api/device/register，后端按 MAC 幂等。
 * 响应信封 { code, message, data:{ deviceId, apiKey } }，仅取 apiKey */
int sync_register(const char *mac, const char *name,
                  char *out_api_key, size_t key_len)
{
    if (!mac || !out_api_key || key_len <= 0) return -1;
    out_api_key[0] = '\0';

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mac", mac);
    cJSON_AddStringToObject(o, "name", name ? name : "");
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/register", s_base_url);

    /* 响应体经 event_handler 收集（perform 后直接 read 不可靠） */
    char resp[512] = { 0 };
    recv_ctx_t rctx = { .buf = resp, .buf_size = sizeof(resp) - 1, .offset = 0 };
    s_pull_ctx = &rctx;

    esp_http_client_config_t cfg;
    fill_cfg(&cfg, url, 15000);
    cfg.event_handler = pull_event_handler;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    set_common_headers(client);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    s_pull_ctx = NULL;
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status != 200 || rctx.offset <= 0) {
        LOG_E("register failed: %s status=%d", esp_err_to_name(err), status);
        return -1;
    }

    int ret = -1;
    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *key = data ? cJSON_GetObjectItem(data, "apiKey") : NULL;
        if (cJSON_IsNumber(code) && code->valueint == 0 &&
            cJSON_IsString(key) && key->valuestring && key->valuestring[0]) {
            strncpy(out_api_key, key->valuestring, key_len - 1);
            out_api_key[key_len - 1] = '\0';
            ret = 0;
        }
        cJSON_Delete(root);
    }
    if (ret != 0) LOG_E("register payload invalid");
    return ret;
}

int sync_pull_words(int local_version, char *out_buf, int buf_size)
{
    if (!out_buf || buf_size <= 0) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/sync/words?version=%d",
             s_base_url, local_version);

    recv_ctx_t ctx = { .buf = out_buf, .buf_size = buf_size, .offset = 0 };
    s_pull_ctx = &ctx;

    esp_http_client_config_t cfg;
    fill_cfg(&cfg, url, 15000);
    cfg.event_handler = pull_event_handler;

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
    } else if (err == ESP_OK && status == 401) {
        ret = SYNC_ERR_AUTH;   /* 钥被换发：上层清钥重注册 */
        LOG_W("pull words: key rejected (401), re-register pending");
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

    /* 组装 JSON 数组：wordId 为云端 Guid 字符串（后端 ProgressItem.WordId）；
     * timestamp=0 由服务器落地时间代替（设备自治钟无绝对时间域） */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "wordId", items[i].word_id);
        cJSON_AddNumberToObject(o, "quality", items[i].quality);
        cJSON_AddNumberToObject(o, "timestamp", (double)items[i].timestamp);
        cJSON_AddItemToArray(arr, o);
    }
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!body) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/sync/progress", s_base_url);

    esp_http_client_config_t cfg;
    fill_cfg(&cfg, url, 15000);
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
    if (err == ESP_OK && status == 401) {
        LOG_W("push progress: key rejected (401)");
        return SYNC_ERR_AUTH;
    }
    LOG_E("push progress failed: %s status=%d", esp_err_to_name(err), status);
    return -1;
}

/* 收藏上报：POST /api/device/sync/collect { wordId, collected } */
int sync_push_collect(const char *word_id, bool collected)
{
    if (!word_id || !word_id[0]) return -1;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wordId", word_id);
    cJSON_AddBoolToObject(o, "collected", collected);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/sync/collect", s_base_url);

    esp_http_client_config_t cfg;
    fill_cfg(&cfg, url, 15000);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    set_common_headers(client);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err == ESP_OK && status == 200) {
        LOG_I("collect '%s' -> %d", word_id, collected);
        return 0;
    }
    if (err == ESP_OK && status == 401) {
        LOG_W("push collect: key rejected (401)");
        return SYNC_ERR_AUTH;
    }
    LOG_E("push collect failed: %s status=%d", esp_err_to_name(err), status);
    return -1;
}

/* 墨封上报：POST /api/device/sync/master { wordId, mastered }
 * （sync_push_collect 同构镜像，2026-09-04 墨封功能） */
int sync_push_master(const char *word_id, bool mastered)
{
    if (!word_id || !word_id[0]) return -1;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wordId", word_id);
    cJSON_AddBoolToObject(o, "mastered", mastered);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/sync/master", s_base_url);

    esp_http_client_config_t cfg;
    fill_cfg(&cfg, url, 15000);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    set_common_headers(client);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err == ESP_OK && status == 200) {
        LOG_I("master '%s' -> %d", word_id, mastered);
        return 0;
    }
    if (err == ESP_OK && status == 401) {
        LOG_W("push master: key rejected (401)");
        return SYNC_ERR_AUTH;
    }
    if (err == ESP_OK && status == 404) {
        /* 后端未部署本端点（固件先行窗口期）：永久失败，丢弃事件防
         * flush 队头阻塞（NVS 已存终态，部署后由后续幂等 set 对账） */
        LOG_W("push master: endpoint missing (404), drop");
        return SYNC_ERR_DROP;
    }
    LOG_E("push master failed: %s status=%d", esp_err_to_name(err), status);
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

    esp_http_client_config_t cfg;
    fill_cfg(&cfg, url, 10000);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    set_common_headers(client);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err == ESP_OK && status == 401) return SYNC_ERR_AUTH;
    return (err == ESP_OK && status == 200) ? 0 : -1;
}

/* 天气：GET /api/device/weather（后端聚合上游并缓存，附带服务器时间） */
static char s_wx_buf[1024];

int sync_fetch_weather(weather_info_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/weather", s_base_url);

    recv_ctx_t ctx = { .buf = s_wx_buf, .buf_size = sizeof(s_wx_buf), .offset = 0 };
    s_pull_ctx = &ctx;

    esp_http_client_config_t cfg;
    fill_cfg(&cfg, url, 10000);
    cfg.event_handler = pull_event_handler;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    set_common_headers(client);

    esp_err_t err = esp_http_client_perform(client);
    s_pull_ctx = NULL;
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx.offset <= 0) {
        LOG_E("fetch weather failed: err=%s status=%d", esp_err_to_name(err), status);
        return -1;
    }
    s_wx_buf[ctx.offset] = '\0';

    /* 解析信封 { code, message, data:{ icon, tempC, desc, serverTime, tzOffsetMin } } */
    int ret = -1;
    cJSON *root = cJSON_Parse(s_wx_buf);
    if (!root) {
        LOG_E("weather json parse failed");
        return -1;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsNumber(code) && code->valueint == 0 && data) {
        cJSON *jicon = cJSON_GetObjectItem(data, "icon");
        cJSON *jtemp = cJSON_GetObjectItem(data, "tempC");
        cJSON *jdesc = cJSON_GetObjectItem(data, "desc");
        cJSON *jtime = cJSON_GetObjectItem(data, "serverTime");
        cJSON *jtz   = cJSON_GetObjectItem(data, "tzOffsetMin");

        if (cJSON_IsNumber(jicon) && cJSON_IsNumber(jtemp)) {
            out->icon   = (uint8_t)jicon->valueint;
            out->temp_c = (int8_t)jtemp->valueint;
            if (cJSON_IsString(jdesc) && jdesc->valuestring)
                strncpy(out->desc, jdesc->valuestring, sizeof(out->desc) - 1);
            if (cJSON_IsNumber(jtime)) out->server_time = (int64_t)jtime->valuedouble;
            if (cJSON_IsNumber(jtz))   out->tz_offset_min = (int16_t)jtz->valueint;
            ret = 0;
            LOG_I("weather: %dC icon=%u '%s'", out->temp_c, out->icon, out->desc);
        }
    }

    cJSON_Delete(root);
    if (ret != 0) LOG_E("weather payload invalid");
    return ret;
}

/* A3 对话周报：GET /api/device/chat-review（weather 同构；404 单列=
 * 尚无周报，屏显文案与网络失败区分） */
static char s_review_buf[1280];

int sync_fetch_chat_review(chat_review_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    char url[256];
    snprintf(url, sizeof(url), "%s/api/device/chat-review", s_base_url);

    recv_ctx_t ctx = { .buf = s_review_buf, .buf_size = sizeof(s_review_buf),
                       .offset = 0 };
    s_pull_ctx = &ctx;

    esp_http_client_config_t cfg;
    fill_cfg(&cfg, url, 10000);
    cfg.event_handler = pull_event_handler;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    set_common_headers(client);

    esp_err_t err = esp_http_client_perform(client);
    s_pull_ctx = NULL;
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx.offset <= 0) {
        if (status == 404) return 1;   /* 周报未生成（Job 未跑/无对话） */
        LOG_E("fetch chat-review failed: err=%s status=%d",
              esp_err_to_name(err), status);
        return -1;
    }
    s_review_buf[ctx.offset] = '\0';

    /* 信封 { code, data:{ weekStart, turnCount, review:{ summary,
     * topics[], highlights[], suggestion, reviewWords[] } } }——设备端
     * 只取三段（topics/highlights 屏显克制不消费） */
    int ret = -1;
    cJSON *root = cJSON_Parse(s_review_buf);
    if (!root) {
        LOG_E("chat-review json parse failed");
        return -1;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsNumber(code) && code->valueint == 0 && data) {
        cJSON *jws = cJSON_GetObjectItem(data, "weekStart");
        cJSON *jtc = cJSON_GetObjectItem(data, "turnCount");
        cJSON *jrv = cJSON_GetObjectItem(data, "review");
        if (cJSON_IsString(jws) && jws->valuestring &&
            cJSON_IsNumber(jtc)) {
            if (strlen(jws->valuestring) >= 10)
                strncpy(out->week_start, jws->valuestring, 10); /* ISO 截日期 */
            else
                memcpy(out->week_start, "----00-00", 10); /* 异常占位（menu 端 +5 免越界） */
            out->turn_count = jtc->valueint;
            if (cJSON_IsObject(jrv)) {
                cJSON *jsum = cJSON_GetObjectItem(jrv, "summary");
                cJSON *jsug = cJSON_GetObjectItem(jrv, "suggestion");
                if (cJSON_IsString(jsum) && jsum->valuestring)
                    strncpy(out->summary, jsum->valuestring,
                            sizeof(out->summary) - 1);
                if (cJSON_IsString(jsug) && jsug->valuestring)
                    strncpy(out->suggestion, jsug->valuestring,
                            sizeof(out->suggestion) - 1);
                cJSON *jwords = cJSON_GetObjectItem(jrv, "reviewWords");
                if (cJSON_IsArray(jwords)) {
                    size_t off = 0;
                    cJSON *jw;
                    cJSON_ArrayForEach(jw, jwords) {
                        if (!cJSON_IsString(jw) || !jw->valuestring ||
                            !jw->valuestring[0])
                            continue;
                        int n = snprintf(out->words + off,
                                         sizeof(out->words) - off,
                                         off ? " · %s" : "%s",
                                         jw->valuestring);
                        if (n < 0 || (size_t)n >= sizeof(out->words) - off)
                            break;   /* 截断即止（≤5 词屏显余量） */
                        off += (size_t)n;
                    }
                }
            }
            ret = 0;
        }
    }

    cJSON_Delete(root);
    if (ret != 0) LOG_E("chat-review payload invalid");
    return ret;
}

/* ============================================================
 * HTTP Date 头校时（设备主时间源，替代被运营商 UDP 123 劫持废掉的 SNTP）
 * ============================================================ */

#define SYNC_HTTP_TIME_URL    "http://connect.rom.miui.com/generate_204"
#define SYNC_HTTP_TIME_TIMEOUT_MS 5000
#define SYNC_TIME_MIN_EPOCH   1735689600LL /* 2025-01-01，早于此视为异常 */
#define SYNC_TIME_MAX_EPOCH   4102444800LL /* 2100-01-01，晚于此视为异常 */

/* 公历转自 1970-01-01 的天数（Howard Hinnant 算法，免 timegm 依赖） */
static long http_time_days_from_civil(long y, long m, long d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;                                   /* [0,399] */
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* 解析 RFC1123 Date 头："Mon, 17 Aug 2026 09:05:03 GMT" → Unix 秒 */
static int64_t http_time_parse_date(const char *v)
{
    static const char *k_months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    const char *p = v ? strchr(v, ',') : NULL;
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[8] = { 0 };

    if (!p) return 0;
    if (sscanf(p + 1, " %d %3s %d %d:%d:%d",
               &day, mon, &year, &hh, &mm, &ss) != 6) return 0;

    int mi = 0;
    for (int i = 0; i < 12; i++)
        if (strcmp(mon, k_months[i]) == 0) { mi = i + 1; break; }
    if (mi < 1 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return 0;

    long days = http_time_days_from_civil(year, mi, day);
    int64_t t = (int64_t)days * 86400 + hh * 3600 + mm * 60 + ss;  /* GMT */
    return (t >= SYNC_TIME_MIN_EPOCH && t <= SYNC_TIME_MAX_EPOCH) ? t : 0;
}

static int64_t s_http_time;  /* 事件回调收集的 Date 头时间 */

static esp_err_t http_time_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
        evt->header_value && strcasecmp(evt->header_key, "Date") == 0) {
        s_http_time = http_time_parse_date(evt->header_value);
    }
    return ESP_OK;
}

int64_t sync_fetch_http_time(void)
{
    s_http_time = 0;
    esp_http_client_config_t cfg = {
        .url = SYNC_HTTP_TIME_URL,
        .timeout_ms = SYNC_HTTP_TIME_TIMEOUT_MS,
        .event_handler = http_time_evt,
        .method = HTTP_METHOD_HEAD,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 0;

    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK && s_http_time > 0 && code >= 200 && code < 400) {
        LOG_I("http time ok: %lld", (long long)s_http_time);
        return s_http_time;
    }
    LOG_W("http time failed (err=%d code=%d)", err, code);
    return 0;
}
