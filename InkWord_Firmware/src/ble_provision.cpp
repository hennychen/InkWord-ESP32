/**
 * @file ble_provision.cpp
 * @brief BLE 配网服务实现（NimBLE-Arduino 1.4.x）
 *
 * 线程模型：
 *   - NimBLE host 任务：执行 GATT 读写回调（onRead/onWrite）
 *   - scan_task：独立任务执行阻塞的 wifi_scan（1~2 秒），
 *     BLE 回调只置标志，绝不阻塞 host
 *   - monitor_task：1 秒轮询 Wi-Fi 状态/IP，变化时刷新广播 +
 *     notify status 订阅端（去抖，未变化不重发）
 *
 * 内存预算：NimBLE host 栈 4KB + 堆约 50KB（弃用 Bluedroid 130KB），
 * 初始化前后打印 heap 对比（M2.5 验收 >80KB 余量）。
 */
#include "ble_provision.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include <NimBLEDevice.h>
#include "cJSON.h"
#include "esp_log.h"
#include "debug_log.h"   /* 还原被 esp32-hal-log 劫持的 ESP_LOGx（NimBLE 头拉入了 Arduino.h） */
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wifi_config_ui.h"
#include "wifi_manager.h"

static const char *TAG = "BLE_PROV";

/* ============================================================
 * 协议常量（与 App 端 InkWord_App/lib/core/epd_protocol.dart 单点同步，
 * 两侧必须一起改）
 * ============================================================ */
static const char *kServiceUuid = "cc5a0001-7e8b-4c3a-9d2e-5f6a7b8c9d0e";
static const char *kCredsUuid   = "cc5a0002-7e8b-4c3a-9d2e-5f6a7b8c9d0e";
static const char *kStatusUuid  = "cc5a0003-7e8b-4c3a-9d2e-5f6a7b8c9d0e";
static const char *kScanUuid    = "cc5a0004-7e8b-4c3a-9d2e-5f6a7b8c9d0e";

#define BLE_MSD_COMPANY_ID  0x02E5  /* Espressif，小端两字节 E5 02 */
#define BLE_PROTO_VERSION   1       /* App 校验版本号，不匹配即忽略广播 */
#define BLE_SCAN_MAX_APS    20      /* 与 HTTP 端 /api/wifi/scan 一致 */
#define BLE_SCAN_TASK_STACK 4096
#define BLE_MONITOR_TASK_STACK 3072
#define BLE_MONITOR_PERIOD_MS  1000

static NimBLEServer         *s_server;
static NimBLECharacteristic *s_creds_chr;
static NimBLECharacteristic *s_status_chr;
static NimBLECharacteristic *s_scan_chr;
static bool    s_ready;
static char    s_dev_name[16];          /* "InkWord-XXXX" */
static SemaphoreHandle_t s_adv_lock;    /* 广播重建互斥（多任务并发刷新） */

/* 扫描任务触发（回调置位，任务执行后自清） */
static volatile bool s_scan_busy;

/* 状态去抖快照（monitor 任务与 ble_adv_update_state 共用） */
static int      s_last_state = -1;
static uint32_t s_last_ip    = 0;

/* ============================================================
 * 状态快照与 JSON 构建
 * ============================================================ */

/* wconn_state_t -> 广播/状态字符串编码：0 idle / 1 connecting / 2 ok / 3 fail */
static int current_wifi_state(void)
{
    switch (wifi_connect_state()) {
    case WCONN_CONNECTING: return 1;
    case WCONN_OK:         return 2;
    case WCONN_FAIL:       return 3;
    default:               return 0;
    }
}

static uint32_t current_ip4(void)
{
    char buf[16];
    if (!wifi_get_sta_ip(buf, sizeof(buf)))
        return 0;
    unsigned a, b, c, d;
    if (sscanf(buf, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* status 特征值：{"state":"idle|connecting|ok|fail","ip":"x.x.x.x"} */
static std::string build_status_json(void)
{
    static const char *names[] = { "idle", "connecting", "ok", "fail" };
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return "{}";
    cJSON_AddStringToObject(o, "state", names[current_wifi_state()]);

    char ip[16];
    if (wifi_get_sta_ip(ip, sizeof(ip)))
        cJSON_AddStringToObject(o, "ip", ip);
    else
        cJSON_AddStringToObject(o, "ip", "");

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    std::string out = s ? s : "{}";
    if (s)
        free(s);
    return out;
}

/* 配网拒绝：status 特征 notify {"state":"error","reason":...}
 * （reason 取值与 App provision_wizard 提示一一对应） */
static void notify_status_error(const char *reason)
{
    if (!s_status_chr)
        return;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return;
    cJSON_AddStringToObject(o, "state", "error");
    cJSON_AddStringToObject(o, "reason", reason);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        s_status_chr->notify(std::string(s));
        free(s);
    }
}

/* status 特征推送当前状态（订阅者不存在时 NimBLE 内部忽略） */
static void push_status_notify(void);
static void notify_scan_error(const char *err);
static void push_status_notify(void)
{
    if (!s_status_chr)
        return;
    s_status_chr->setValue(build_status_json());
    s_status_chr->notify();
}

/* ============================================================
 * GATT 回调
 * ============================================================ */

class CredsCallbacks : public NimBLECharacteristicCallbacks {
public:
    /* 写入 Wi-Fi 凭据 JSON {"ssid","pass"}；并发规则对齐 HTTP 端 503 语义 */
    void onWrite(NimBLECharacteristic *p, ble_gap_conn_desc * /*desc*/) override
    {
        if (wifi_config_ui_is_active()) {
            ESP_LOGW(TAG, "creds rejected: config UI active");
            notify_status_error("device-config-ui");
            return;
        }
        if (wifi_connect_state() == WCONN_CONNECTING) {
            ESP_LOGW(TAG, "creds rejected: connecting in progress");
            notify_status_error("connecting");
            return;
        }

        std::string v = p->getValue();
        cJSON *j = cJSON_Parse(v.c_str());
        if (!j) {
            notify_status_error("bad-json");
            return;
        }
        const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
        const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));

        /* 802.11 SSID <=32 字节，WPA 密码 <=63 字符 */
        if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32 ||
            (pass && strlen(pass) > 63)) {
            cJSON_Delete(j);
            notify_status_error("invalid");
            return;
        }

        int r = wifi_connect_async(ssid, pass ? pass : "");
        cJSON_Delete(j);
        if (r != 0) {
            /* wifi_connect_async 仅参数非法/任务创建失败才返回 <0 */
            notify_status_error("invalid");
            return;
        }
        ESP_LOGI(TAG, "creds accepted, connecting to \"%s\"", ssid);
        push_status_notify();          /* 立即推 connecting */
        ble_adv_update_state();        /* 广播同步切 connecting */
    }
};

class StatusCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic *p, ble_gap_conn_desc * /*desc*/) override
    {
        p->setValue(build_status_json());
    }
};

class ScanCallbacks : public NimBLECharacteristicCallbacks {
public:
    /* 写任意字节触发设备侧 Wi-Fi 扫描；忙时回 {"end":true,"err":"busy"} */
    void onWrite(NimBLECharacteristic * /*p*/, ble_gap_conn_desc * /*desc*/) override
    {
        if (wifi_config_ui_is_active() || s_scan_busy ||
            wifi_connect_state() == WCONN_CONNECTING) {
            ESP_LOGW(TAG, "scan rejected: busy");
            notify_scan_error("busy");
            return;
        }
        s_scan_busy = true;  /* 扫描任务轮询此标志（<=100ms 延迟） */
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer * /*p*/, ble_gap_conn_desc * /*desc*/) override
    {
        ESP_LOGI(TAG, "client connected");
    }

    void onDisconnect(NimBLEServer * /*p*/, ble_gap_conn_desc * /*desc*/) override
    {
        ESP_LOGI(TAG, "client disconnected, restart advertising");
        NimBLEDevice::startAdvertising();
    }
};

/* ============================================================
 * 广播数据（发现闭环：MSD 携带 Wi-Fi 状态 + IP）
 * ============================================================ */
void ble_adv_update_state(void)
{
    if (!s_ready)
        return;

    if (s_adv_lock && xSemaphoreTake(s_adv_lock, pdMS_TO_TICKS(500)) != pdTRUE)
        return;

    /* manufacturer data：companyId(2, 小端) + [版本, 状态, IP×4] = 8 字节 */
    uint32_t ip = current_ip4();
    uint8_t msd[8] = {
        (uint8_t)(BLE_MSD_COMPANY_ID & 0xFF),
        (uint8_t)(BLE_MSD_COMPANY_ID >> 8),
        BLE_PROTO_VERSION,
        (uint8_t)current_wifi_state(),
        (uint8_t)(ip >> 24), (uint8_t)(ip >> 16),
        (uint8_t)(ip >> 8),  (uint8_t)ip,
    };

    NimBLEAdvertisementData d;
    d.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    d.setName(std::string(s_dev_name));
    d.setManufacturerData(std::string((const char *)msd, sizeof(msd)));

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv->isAdvertising())
        adv->stop();
    adv->setAdvertisementData(d);
    adv->start();

    /* 同步去抖快照，monitor 任务据此判断变化 */
    s_last_state = current_wifi_state();
    s_last_ip = ip;

    if (s_adv_lock)
        xSemaphoreGive(s_adv_lock);
}

/* ============================================================
 * 后台任务
 * ============================================================ */

static void notify_scan_error(const char *err)
{
    if (!s_scan_chr)
        return;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return;
    cJSON_AddBoolToObject(o, "end", true);
    cJSON_AddStringToObject(o, "err", err);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        s_scan_chr->notify(std::string(s));
        free(s);
    }
}

/* 独立任务执行阻塞扫描（1~2 秒），逐 AP notify，结束推 end 标记 */
static void ble_scan_task(void * /*arg*/)
{
    static wifi_ap_record_t aps[BLE_SCAN_MAX_APS];

    for (;;) {
        if (!s_scan_busy) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int n = wifi_scan(aps, BLE_SCAN_MAX_APS);
        if (n < 0) {
            ESP_LOGW(TAG, "wifi_scan failed: %d", n);
            s_scan_busy = false;
            notify_scan_error("scan_failed");
            continue;
        }
        ESP_LOGI(TAG, "wifi_scan found %d APs, streaming", n);

        for (int i = 0; i < n && s_scan_chr; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o)
                continue;
            cJSON_AddNumberToObject(o, "i", i);
            cJSON_AddNumberToObject(o, "n", n);
            cJSON_AddStringToObject(o, "ssid", (const char *)aps[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(o, "auth", aps[i].authmode != WIFI_AUTH_OPEN);
            char *s = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (s) {
                s_scan_chr->notify(std::string(s));
                free(s);
            }
            vTaskDelay(pdMS_TO_TICKS(20));  /* 防 notify 洪泛丢包 */
        }

        if (s_scan_chr) {
            cJSON *o = cJSON_CreateObject();
            if (o) {
                cJSON_AddBoolToObject(o, "end", true);
                cJSON_AddNumberToObject(o, "n", n);
                char *s = cJSON_PrintUnformatted(o);
                cJSON_Delete(o);
                if (s) {
                    s_scan_chr->notify(std::string(s));
                    free(s);
                }
            }
        }
        s_scan_busy = false;
    }
}

/* 轮询 Wi-Fi 状态/IP：变化 -> 刷新广播 + notify status（配网结果推送） */
static void ble_monitor_task(void * /*arg*/)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BLE_MONITOR_PERIOD_MS));
        if (!s_ready)
            continue;
        int st = current_wifi_state();
        uint32_t ip = current_ip4();
        if (st != s_last_state || ip != s_last_ip) {
            ESP_LOGI(TAG, "wifi state %d->%d, ip 0x%08" PRIx32 "->0x%08" PRIx32,
                     s_last_state, st, s_last_ip, ip);
            push_status_notify();
            ble_adv_update_state();  /* 内部更新 s_last_* 快照 */
        }
    }
}

/* ============================================================
 * 公共接口
 * ============================================================ */
int ble_provision_init(void)
{
    if (s_ready)
        return 0;

    size_t heap_before = esp_get_free_heap_size();

    s_adv_lock = xSemaphoreCreateMutex();
    if (!s_adv_lock)
        return -1;

    NimBLEDevice::init("InkWord");

    /* 设备名 InkWord-XXXX：MAC 后 4 hex（与手机端展示一致，区分多台设备） */
    std::string addr = NimBLEDevice::getAddress().toString();  /* "xx:xx:xx:xx:xx:xx" */
    if (addr.size() >= 17)
        snprintf(s_dev_name, sizeof(s_dev_name), "InkWord-%c%c%c%c",
                 addr[12], addr[13], addr[15], addr[16]);
    else
        snprintf(s_dev_name, sizeof(s_dev_name), "InkWord-0000");
    NimBLEDevice::setDeviceName(std::string(s_dev_name));
    NimBLEDevice::setMTU(247);  /* creds JSON 最长约 130 字节，默认 23 不够 */

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(new ServerCallbacks());

    NimBLEService *svc = s_server->createService(NimBLEUUID(kServiceUuid));

    s_creds_chr = svc->createCharacteristic(NimBLEUUID(kCredsUuid),
                                            NIMBLE_PROPERTY::WRITE);
    s_creds_chr->setCallbacks(new CredsCallbacks());

    s_status_chr = svc->createCharacteristic(NimBLEUUID(kStatusUuid),
                                             NIMBLE_PROPERTY::READ |
                                             NIMBLE_PROPERTY::NOTIFY);
    s_status_chr->setCallbacks(new StatusCallbacks());
    s_status_chr->setValue(build_status_json());

    s_scan_chr = svc->createCharacteristic(NimBLEUUID(kScanUuid),
                                           NIMBLE_PROPERTY::WRITE |
                                           NIMBLE_PROPERTY::NOTIFY);
    s_scan_chr->setCallbacks(new ScanCallbacks());

    svc->start();

    /* 扫描任务常驻（栈 4KB），回调侧只置标志 */
    if (xTaskCreate(ble_scan_task, "ble_scan", BLE_SCAN_TASK_STACK,
                    NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "scan task create failed");
        return -1;
    }
    if (xTaskCreate(ble_monitor_task, "ble_mon", BLE_MONITOR_TASK_STACK,
                    NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "monitor task create failed");
        return -1;
    }

    ble_adv_update_state();  /* 构建广播并启动 */

    s_ready = true;
    size_t heap_after = esp_get_free_heap_size();
    ESP_LOGI(TAG, "ready as \"%s\", heap %u -> %u (-%u)",
             s_dev_name, (unsigned)heap_before, (unsigned)heap_after,
             (unsigned)(heap_before - heap_after));
    return 0;
}

bool ble_provision_ready(void)
{
    return s_ready;
}
