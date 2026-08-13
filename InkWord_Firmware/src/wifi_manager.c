/**
 * @file wifi_manager.c
 * @brief WiFi 联网实现 (Task F-17)
 */
#include "wifi_manager.h"
#include "debug_log.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "WIFI";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5

static EventGroupHandle_t s_wifi_events;
static int s_retry = 0;
static bool s_connected = false;
static bool s_init_done = false;

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
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            LOG_E("connect to AP failed");
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
