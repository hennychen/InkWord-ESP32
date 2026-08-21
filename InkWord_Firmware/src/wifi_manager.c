/**
 * @file wifi_manager.c
 * @brief WiFi 联网实现 (Task F-17)
 */
#include "wifi_manager.h"
#include "debug_log.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "WIFI";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5

static EventGroupHandle_t s_wifi_events;
static int s_retry = 0;
static bool s_connected = false;
static bool s_init_done = false;

/* SoftAP 配网门户与异步连接状态 */
static bool s_ap_active = false;
static esp_netif_t *s_ap_netif = NULL;
static volatile wconn_state_t s_wconn = WCONN_IDLE;

/* 断线慢速重连定时器（5 次快速重试失败后启用） */
static esp_timer_handle_t s_recon_timer = NULL;

static void recon_timer_cb(void *arg)
{
    (void)arg;
    LOG_W("wifi slow-reconnect: retrying");
    s_retry = 0;
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            LOG_W("retry connect to AP (%d/%d)", s_retry, MAX_RETRY);
        } else {
            /* 快速重试用尽：转 30s 慢速重连（路由器重启/临时断网恢复后自动回网，
             * 不再永久离线）；回调运行于 esp_timer 任务，不阻塞事件循环 */
            if (!s_recon_timer) {
                const esp_timer_create_args_t t = {
                    .callback = recon_timer_cb,
                    .name = "wifi_recon",
                };
                esp_timer_create(&t, &s_recon_timer);
            }
            esp_timer_stop(s_recon_timer); /* 防重复叠加 */
            esp_timer_start_once(s_recon_timer, 30 * 1000000ULL);
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            LOG_E("connect failed, slow-reconnect in 30s");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        LOG_I("got ip:" IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t load_credentials(wifi_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t ssid_len = sizeof(cfg->sta.ssid);
    size_t pass_len = sizeof(cfg->sta.password);
    esp_err_t r1 = nvs_get_str(h, "ssid", (char *)cfg->sta.ssid, &ssid_len);
    esp_err_t r2 = nvs_get_str(h, "pass", (char *)cfg->sta.password, &pass_len);
    nvs_close(h);
    return (r1 == ESP_OK && r2 == ESP_OK) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int wifi_manager_init(void)
{
    if (s_init_done) return 0;

    /* 网络栈前置初始化：esp_netif 系列与 esp_http_server(socket) 均依赖
     * tcpip 线程；纯 IDF 调用路径下 Arduino 框架不会代劳。
     * 两者均幂等（重复调用返回 ESP_ERR_INVALID_STATE，安全忽略） */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t lo = esp_event_loop_create_default();
    if (lo != ESP_OK && lo != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(lo);
    }

    s_wifi_events = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id, got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &got_ip);

    wifi_config_t wifi_cfg = { 0 };
    bool have_cred = (load_credentials(&wifi_cfg) == ESP_OK);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* 关闭 Wi-Fi Modem Sleep：本机作为 HTTP/mDNS 服务端需随时响应 ARP 与 TCP。
     * 默认 WIFI_PS_MIN_MODEM 下设备间歇休眠，ARP 请求无应答、局域网单播
     * 完全不可达（mDNS 组播因设备主动发包仍通，极具迷惑性）。
     * 代价：STA 功耗升高（测试版可接受，量产可换回并配合保活策略） */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (have_cred) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        LOG_I("connecting to saved SSID: %s", wifi_cfg.sta.ssid);
    } else {
        LOG_W("no saved WiFi credentials, waiting for provisioning");
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    s_init_done = true;

    if (have_cred) {
        /* 等待连接结果 */
        EventGroupHandle_t ev = s_wifi_events;
        xEventGroupWaitBits(ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                            pdFALSE, pdFALSE, portMAX_DELAY);
    }
    return 0;
}

int wifi_connect(const char *ssid, const char *password)
{
    if (!s_init_done) wifi_manager_init();

    /* 保存到 NVS */
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", password);
        nvs_commit(h);
        nvs_close(h);
    }

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);

    s_retry = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    esp_wifi_disconnect();
    esp_wifi_connect();

    EventGroupHandle_t ev = s_wifi_events;
    EventBits_t bits = xEventGroupWaitBits(ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) ? 0 : -1;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

void wifi_disconnect(void)
{
    esp_wifi_disconnect();
    s_connected = false;
}

void wifi_radio_off(void)
{
    esp_wifi_stop();    /* 停 STA/AP + 射频断电（深睡前彻底关断） */
    s_connected = false;
}

bool wifi_has_saved_credentials(void)
{
    wifi_config_t cfg;
    return (load_credentials(&cfg) == ESP_OK);
}

int wifi_scan(wifi_ap_record_t *results, int max)
{
    if (!s_init_done) {
        wifi_manager_init();
    }
    if (!results || max <= 0) return 0;

    /* 主动扫描，阻塞至完成 */
    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        LOG_E("wifi scan start failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* 清除上次扫描缓存，再读取本次结果
     * 注：clear 必须在 scan_start 之前调用，否则会清掉刚扫到的结果 */
    uint16_t ap_num = (uint16_t)max;
    err = esp_wifi_scan_get_ap_records(&ap_num, results);
    if (err != ESP_OK) {
        LOG_E("wifi scan get_ap_records failed: %s", esp_err_to_name(err));
        return -1;
    }

    LOG_I("wifi scan found %d networks", ap_num);
    return (int)ap_num;
}

/* ============================================================
 * SoftAP 配网门户 + 异步连接 (captive portal)
 * ============================================================ */

int wifi_start_softap(void)
{
    if (!s_init_done) wifi_manager_init();
    if (s_ap_active) return 0;

    /* STA netif 已在 wifi_manager_init 创建；AP netif 仅创建一次 */
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    /* 运行中切换模式需先 stop（重新 start 后 STA 自动重连已保存网络） */
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, "InkWord-Setup", sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen("InkWord-Setup");
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;   /* 开放网络：连上即可弹出门户 */
    ap.ap.max_connection = 2;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_active = true;
    s_retry = 0;
    LOG_I("SoftAP 'InkWord-Setup' started (APSTA mode)");
    return 0;
}

void wifi_stop_softap(void)
{
    if (!s_ap_active) return;
    s_ap_active = false;

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* WIFI_EVENT_STA_START 回调自动 esp_wifi_connect()，
     * 沿用 RAM 中 STA config（异步连接成功前已 set_config）重连 */
    LOG_I("SoftAP stopped, back to STA mode");
}

bool wifi_softap_active(void)
{
    return s_ap_active;
}

/* 异步连接任务：参数为堆上 strdup 的 [ssid, pass] 二维指针 */
static void connect_task(void *arg)
{
    char **creds = (char **)arg;
    s_wconn = WCONN_CONNECTING;
    int r = wifi_connect(creds[0], creds[1]);
    free(creds[0]);
    free(creds[1]);
    free(creds);
    s_wconn = (r == 0) ? WCONN_OK : WCONN_FAIL;
    LOG_I("async connect %s (state=%d)", r == 0 ? "OK" : "FAIL", s_wconn);
    vTaskDelete(NULL);
}

int wifi_connect_async(const char *ssid, const char *password)
{
    if (!ssid || !*ssid || strlen(ssid) > 32) return -1;
    if (password && strlen(password) > 64) return -1;

    char **creds = calloc(2, sizeof(char *));
    if (!creds) return -1;
    creds[0] = strdup(ssid);
    creds[1] = password ? strdup(password) : strdup("");
    if (!creds[0] || !creds[1]) {
        free(creds[0]); free(creds[1]); free(creds);
        return -1;
    }

    if (xTaskCreate(connect_task, "wconn", 4096, creds, 5, NULL) != pdPASS) {
        free(creds[0]); free(creds[1]); free(creds);
        return -1;
    }
    return 0;
}

wconn_state_t wifi_connect_state(void)
{
    return s_wconn;
}

bool wifi_get_sta_ip(char *buf, size_t len)
{
    if (!buf || len == 0) return false;
    buf[0] = '\0';
    if (!s_connected) return false;

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!nif) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(nif, &ip) != ESP_OK) return false;
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return true;
}
