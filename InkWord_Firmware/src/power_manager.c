/**
 * @file power_manager.c
 * @brief SoC 深睡与定时唤醒实现 (P5)
 *
 * 模块边界：只负责"入睡"与"唤醒原因"，静默心跳会话的业务链
 * （校时/上报/OTA）在 main.cpp（复用其 static 凭据/上报函数）。
 *
 * S3 深睡要点（2026-08-21）：
 *   - 按键唤醒走 ext1：本构建链（Arduino core 2.0.17 / IDF 4.4.7）
 *     的 S3 SOC_PM_SUPPORT_EXT_WAKEUP=1，ext1 可用（esp_deep_sleep_
 *     enable_gpio_wakeup 是 IDF 5.x 才为 S3 启用的替代 API，其能力宏
 *     SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP 在本 sdk 头中未定义）；
 *     P6 框架迁移时须换 esp_deep_sleep_enable_gpio_wakeup 并改用
 *     ESP_SLEEP_WAKEUP_GPIO 判别（见 arm_button_wakeup）；
 *   - 中键 GPIO21 为 RTC 域最后一个脚（S3 RTC GPIO 0~21），深睡中
 *     数字域断电，须 rtc_gpio_pullup_en 维持上拉（否则输入悬空，
 *     COM 线上干扰可反复误唤醒）；
 *   - gpio_hold 越过深睡存活：EPD CS 锁存防总线悬浮致 COG 误读，
 *     唤醒后必须 gpio_hold_dis 释放（power_init 已处理）；
 *   - 板级余量（SD 卡/功放/USB 芯片无断电域）决定实测底电流高于
 *     SoC 标称 ~7µA，<5µA 指标待量产 PCB 断电域设计（PRD §8.1 注）。
 */
#include "power_manager.h"
#include "gpio_config.h"
#include "debug_log.h"
#include "haptic.h"
#include "audio_player.h"
#include "epd_driver.h"
#include "learning_state.h"
#include "standby_page.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "lan_display_server.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"

static const char *TAG = "POWER";

static esp_sleep_wakeup_cause_t s_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static int64_t s_last_activity_us = 0;
static bool s_periph_online = false; /* 本会话外设（屏/SD/音频）是否初始化 */

esp_sleep_wakeup_cause_t power_init(void)
{
    /* gpio_hold 跨深睡存活（含冷启动前的残留）：必须先释放，
     * 否则 SPI 驱动接管不了被锁存的 CS/DC 引脚 */
    gpio_hold_dis(EPD_CS_PIN);
    gpio_deep_sleep_hold_dis();

    s_wakeup_cause = esp_sleep_get_wakeup_cause();
    s_last_activity_us = esp_timer_get_time();

    switch (s_wakeup_cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        LOG_I("wakeup: RTC TIMER (silent heartbeat session)");
        break;
    case ESP_SLEEP_WAKEUP_EXT1:
        LOG_I("wakeup: button (CENTER key)");
        break;
    default:
        LOG_I("boot: power-on / external reset");
        break;
    }
    return s_wakeup_cause;
}

bool power_woke_by_button(void)
{
    return s_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1;
}

void power_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

void power_mark_periph_online(void)
{
    s_periph_online = true;
}

void power_maybe_sleep(void)
{
    /* 禁睡条件：屏幕被接管（配网 UI / LAN 接收页）或 AP 门户开热点
     * ——均处于用户交互或直传会话中，睡掉即断会话 */
    if (wifi_config_ui_is_active() || lan_server_is_active() ||
        wifi_softap_active())
        return;

    int64_t idle_s = (esp_timer_get_time() - s_last_activity_us) / 1000000;
    if (idle_s < (int64_t)PM_SLEEP_TIMEOUT_MIN * 60) return;

    power_enter_sleep(PM_HEARTBEAT_PERIOD_S);   /* 不返回 */
}

/* 挂按键唤醒源：ext1 + RTC 上拉。IDF 4.4.7（本构建链）S3 仍支持
 * ext1；IDF 5.x 起改为 esp_deep_sleep_enable_gpio_wakeup(mask,
 * ESP_GPIO_WAKEUP_GPIO_LOW) + 唤醒原因 ESP_SLEEP_WAKEUP_GPIO（P6 迁移项） */
static void arm_button_wakeup(void)
{
    /* 数字域深睡断电：改走 RTC 域上拉维持中键低电平有效输入 */
    rtc_gpio_set_direction((gpio_num_t)NAV_CENTER_PIN,
                           RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)NAV_CENTER_PIN);
    rtc_gpio_pullup_en((gpio_num_t)NAV_CENTER_PIN);

    esp_sleep_enable_ext1_wakeup(1ULL << NAV_CENTER_PIN,
                                 ESP_EXT1_WAKEUP_ALL_LOW);
}

void power_enter_sleep(uint32_t timer_s)
{
    LOG_I("deep sleep in: timer=%lus idle=%llds periph=%d",
          (unsigned long)timer_s,
          (long long)((esp_timer_get_time() - s_last_activity_us) / 1000000),
          s_periph_online);

    /* 1. 自治钟基准对交接（两会话通用：静默会话 HTTP 校准后的基准
     *    也需刷新入 NVS，否则下级唤醒回到旧基准；时间未同步时 no-op；
     *    RTC 差分跨多轮睡醒仍连续，不刷新虽正确但会累积慢钟漂移） */
    standby_time_checkpoint();

    if (s_periph_online) {
        /* 2. 学习状态强制落盘（脏标记清零，NVS 真值先于断电；
         *    入睡时队列中事件必已被 background_task 周期 flush 或本就
         *    无法上报：事件仅由按键产生，而按键刷新活动计时，
         *    10 分钟空闲期内不再有新事件入队） */
        learning_state_save();

        /* 3. 外设收口：音频 I2S 卸载、马达极性感知关断
         *    （低有效模块防睡中常震；未接线时 GPIO 空操作） */
        audio_stop();
        audio_deinit();
        haptic_off();

        /* 4. 屏 COG 深睡锁电荷（0x07/0xA5）画面驻留。交互期局刷后
         *    不 COG 深睡是实测定稿（缩短切换耗时）；SoC 深睡必须补：
         *    UC8253 波形参数依赖电荷锁定的状态一致性起点。唤醒后
         *    GxEPD2 首次刷新自动硬复位重新初始化 COG，无需额外处理 */
        epd_deep_sleep();

        /* 5. Wi-Fi 射频关断：esp_wifi_stop 停射频（disconnect 只断连） */
        wifi_radio_off();

        /* 6. EPD 总线锁存：深睡中数字域断电，CS 拉高保持非选中，
         *    防 MOSI/SCK/DC 悬浮电平被 COG 误读为命令 */
        gpio_hold_en(EPD_CS_PIN);
        gpio_deep_sleep_hold_en();
    }

    /* 7. 唤醒源：中键（任意会话都挂）+ RTC TIMER（0 = 仅按键） */
    arm_button_wakeup();
    if (timer_s > 0)
        esp_sleep_enable_timer_wakeup((uint64_t)timer_s * 1000000ULL);

    LOG_I("deep sleep now");
    esp_deep_sleep_start();   /* 不返回 */
}
