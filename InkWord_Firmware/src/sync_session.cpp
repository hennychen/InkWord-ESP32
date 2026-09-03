/**
 * @file sync_session.cpp
 * @brief 云端同步会话编排实现（P2 巨石拆分：自 main.cpp 迁出，
 *        行为零变化——6 函数 + 头注释整体搬家）
 *
 * 自 main.cpp 迁入的静态链：sync_credentials_load（NVS 凭据恢复）→
 * sync_try_register（MAC 幂等注册）→ sync_recover_auth（401 换钥
 * 自愈）→ sync_flush_pending（上报队列逐条 flush）；静默心跳会话
 * （silent_heartbeat_session，不返回）与后台任务（background_task，
 * 经 sync_background_task_start 拉起）为两条执行路径。
 */
#include "sync_session.h"

#include "sync_client.h"
#include "power_manager.h"      /* power_enter_sleep / PM_* 周期宏 */
#include "wifi_manager.h"       /* wifi_manager_init / is_connected /
                                 * has_saved_credentials */
#include "standby_page.h"       /* standby_time_set / weather_update */
#include "max17048.h"           /* max17048_percent（心跳电量） */
#include "ota_manager.h"        /* ota_check_for_update / perform_upgrade */
#include "lan_display_server.h" /* lan_server_is_running / start */
#include "learning_state.h"     /* 上报事件队列（peek/drop/count） */
#include "word_parser.h"        /* WordEntry / cloud_id 过滤 */
#include "debug_log.h"

#include "esp_system.h"         /* ESP_ERROR_CHECK */
#include "esp_mac.h"            /* esp_read_mac：首次注册的设备身份 */
#include "nvs_flash.h"
#include "nvs.h"
#include "settings_keys.h"   /* P2b：NVS 键权威表 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "SYNC_SESS";

/* 固件版本（心跳上报字段）：main.cpp 定义（FW_VERSION 宏单一事实源），
 * menu_ui / selftest_frame 同款 extern 取用 */
extern "C" const char *fw_version(void);

/* ============================================================
 * 云端同步凭据与上报 flush (P2)
 * 凭据链：NVS "inkword"/{api_url, dev_key} → sync_set_*；无 key 时
 * 联网后按 MAC 幂等注册（后端返回既有 ApiKey）并回写 NVS。
 * ============================================================ */

void sync_credentials_load(void)
{
    char url[128], key[64];
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        sync_set_base_url(INKWORD_API_BASE);
        return;
    }
    size_t len = sizeof(url);
    if (nvs_get_str(h, NVS_KEY_API_URL, url, &len) == ESP_OK)
        sync_set_base_url(url);
    else
        sync_set_base_url(INKWORD_API_BASE);
    len = sizeof(key);
    if (nvs_get_str(h, NVS_KEY_DEV_KEY, key, &len) == ESP_OK)
        sync_set_device_key(key);
    nvs_close(h);
}

static void sync_try_register(void)
{
    if (sync_has_device_key()) return;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char key[64];
    if (sync_register(mac_str, NULL, key, sizeof(key)) == 0) {
        sync_set_device_key(key);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_KEY_DEV_KEY, key);
            nvs_commit(h);
            nvs_close(h);
        }
        LOG_I("device registered, key persisted");
    } else {
        LOG_W("register failed, retry next cycle");
    }
}

/* v2.0 绑定换发自愈（ADR-001 §五）：App 绑定设备后云端换发 ApiKey，
 * 旧钥即刻 401。清内存/NVS 钥 → sync_try_register 按 MAC 幂等重注册
 * 取回新钥（后端 register 返回既有记录的钥，即换发后的新钥）。网络
 * 未连/后端不可达时注册失败，key 保持空下周期再试（离线优先红线：
 * 本地学习全链路不依赖钥）。 */
static void sync_recover_auth(void)
{
    LOG_W("device key rejected (401), re-register by MAC");
    sync_set_device_key("");
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_DEV_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    sync_try_register();
}

/* 上报队列 flush：逐条发送（人手按键频次下 HTTP 开销可忽略；攒批优化
 * 待设备规模上来后）。无 cloudId 的词（本地导入）直接丢弃；任一条
 * 失败即停，队列保留待下周期重试（timestamp=0 由服务器落地时间代替） */
static void sync_flush_pending(void)
{
    int guard = learning_state_event_count();
    while (guard-- > 0) {
        lr_event_t ev;
        if (!learning_state_event_peek(0, &ev)) break;

        const WordEntry *w = word_parser_get(ev.word_idx);
        if (!w || !w->cloud_id[0]) {
            learning_state_event_drop(1); /* 本地词：无云端身份，事件无价值 */
            continue;
        }

        if (ev.quality >= 0) {
            ProgressItem it = {};   /* 全零初始化（quality/word_id/timestamp） */
            it.quality = (uint8_t)ev.quality;
            it.timestamp = 0;
            strncpy(it.word_id, w->cloud_id, sizeof(it.word_id) - 1);
            int rc = sync_push_progress(&it, 1);
            if (rc == SYNC_ERR_AUTH) { sync_recover_auth(); return; }
            if (rc != 0) return;
        } else {
            int rc = sync_push_collect(w->cloud_id, ev.collected);
            if (rc == SYNC_ERR_AUTH) { sync_recover_auth(); return; }
            if (rc != 0) return;
        }
        learning_state_event_drop(1);
    }
}

/* ============================================================
 * P5 静默心跳会话：RTC TIMER 唤醒后的极简启动路径（不返回）
 * 屏/SD/音频/学习状态全不初始化：墨水屏驻留末帧不碰 COG，
 * sync_flush_pending 的 guard=learning_state_event_count()=0（静态
 * 零初始化）自然空转——事件队列是内存态且仅由按键产生，入睡时已
 * 论证必空（见 power_enter_sleep 注释）。NVS 必须初始化（凭据/时钟
 * checkpoint 均在 NVS）。业务链：Wi-Fi 快连（10s 超时失败静默回睡，
 * 不重试不闪屏）→ HTTP Date 校时（standby_page 静态基准对无需
 * standby_init 即可写）→ 注册/上报/心跳/OTA 检查 → 回睡。
 * ============================================================ */
void silent_heartbeat_session(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_manager_init();
    sync_credentials_load();

    /* 未配网的新设备（无凭据即入睡）：不白等超时直接回睡 */
    if (!wifi_has_saved_credentials()) {
        LOG_W("silent session: no wifi credentials, back to sleep");
        power_enter_sleep(PM_HEARTBEAT_PERIOD_S);
    }

    /* 连接为事件驱动异步（wifi_manager_init 内自动连已存网络），
     * 轮询等待：路由器在线典型 2~3s，离线等满 10s 静默回睡 */
    int waited_s = 0;
    while (!wifi_is_connected() && waited_s < PM_WAKE_WIFI_TIMEOUT_S) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited_s++;
    }
    if (!wifi_is_connected()) {
        LOG_W("silent session: wifi timeout (%ds), back to sleep", waited_s);
        power_enter_sleep(PM_HEARTBEAT_PERIOD_S);
    }

    /* HTTP Date 校时：刷新 standby 自治钟内存基准（回睡前由
     * power_enter_sleep 内 checkpoint 落 NVS，下级唤醒用新基准） */
    int64_t now = sync_fetch_http_time();
    if (now > 0) {
        standby_time_set(now);
        LOG_I("silent session: clock calibrated (epoch=%lld)", (long long)now);
    }

    /* 云端闭环（与 background_task 周期段同链）：幂等注册 + 上报 flush */
    sync_try_register();
    sync_flush_pending();
    int bat = max17048_percent();   /* T2.6：实数（模块不在位回退占位） */
    if (bat < 0) bat = 100;
    if (sync_heartbeat(bat, fw_version()) == SYNC_ERR_AUTH)
        sync_recover_auth();   /* 换钥后回睡，下个心跳周期新钥生效 */

    /* OTA 检查：升级成功即重启进新固件（走正常启动路径 ota_mark_valid） */
    char url[256], md5[64];
    int size = 0;
    if (ota_check_for_update(url, sizeof(url), md5, sizeof(md5), &size)) {
        LOG_I("OTA update found (silent session), size=%d", size);
        ota_perform_upgrade(url, md5);
    }

    power_enter_sleep(PM_HEARTBEAT_PERIOD_S);   /* 不返回 */
}

/* 后台心跳 + OTA 检查任务。
 * 启动阶段：每 2s 轮询，联网即立即启动 LAN 直传服务（不设上限：
 *           即使路由器后启动/断电恢复，联网后也能尽快拉起服务）；
 * 之后转为 10 分钟周期：上报队列 flush + 首次注册 + 心跳 + OTA
 * 检查（含服务兜底重启，幂等） */
static void background_task(void *arg)
{
    (void)arg;
    while (!lan_server_is_running()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (wifi_is_connected()) {
            lan_server_start(); /* 幂等 */
        }
    }
    /* 开机即注册（幂等）：原首个周期要等 10 分钟——新设备开机立即
     * 进 AI 对话预检 key=0 被拒（2026-09-01 真机实测发现）；LAN 就绪
     * 时 Wi-Fi 必已连，已注册设备此调用零网络开销 */
    sync_try_register();
    sync_flush_pending();
    const TickType_t period = pdMS_TO_TICKS(10 * 60 * 1000); /* 10 分钟 */
    int wx_poll_cnt = 2; /* 待机页天气轮询计数：初始 2 -> 首个周期即拉取 */
    while (1) {
        vTaskDelay(period);
        if (wifi_is_connected()) {
            lan_server_start(); /* 兜底：服务异常停止则重启（幂等） */

            /* 云端闭环（P2）：首次注册（幂等）+ 评分/收藏上报 flush */
            sync_try_register();
            sync_flush_pending();

            int bat = max17048_percent();   /* T2.6：实数（不在位回退占位） */
            if (bat < 0) bat = 100;
            if (sync_heartbeat(bat, fw_version()) == SYNC_ERR_AUTH)
                sync_recover_auth();   /* 新钥本周期即取回，下周期正常 */

            /* 待机页天气：每 3 个周期（约 30 分钟）拉取一次，失败下周期重试；
             * 仅待机页激活时拉取（学习页不耗流量） */
            if (standby_is_active() && ++wx_poll_cnt >= 3) {
                weather_info_t wx;
                if (sync_fetch_weather(&wx) == 0) {
                    standby_weather_update(&wx);
                    wx_poll_cnt = 0;
                } else {
                    wx_poll_cnt = 2;
                }
            }

            /* 顺带检查 OTA */
            char url[256], md5[64];
            int size = 0;
            if (ota_check_for_update(url, sizeof(url), md5, sizeof(md5), &size)) {
                LOG_I("OTA update found, size=%d", size);
                /* 自动升级可改为需用户确认 */
                ota_perform_upgrade(url, md5);
            }
        }
    }
}

/* 后台任务拉起（原 main.cpp setup 内 xTaskCreate，P2 拆分随任务内聚；
 * 栈/优先级随任务走，调用方不再关心任务细节） */
void sync_background_task_start(void)
{
    xTaskCreate(background_task, "bg", 6 * 1024, NULL, 4, NULL);
}
