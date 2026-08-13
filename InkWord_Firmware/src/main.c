/**
 * @file main.c
 * @brief 应用主入口 (Task F-20)
 *
 * 启动流程：日志 -> NVS -> 存储(SD) -> 屏幕 -> 音频 -> 按键 ->
 *          WiFi -> 词库 -> 进入上次模式 -> 主循环（事件驱动）。
 *
 * 按键映射（默认）：
 *   A 短按=上一条,  B 短按=下一条,  C 短按=发音 / 长按=进入 Wi-Fi 配置,
 *   D 短按=模式切换, D 长按=清残影全刷。
 *   E/F = 左/右方向键（仅在 Wi-Fi 配置页中使用）。
 */
#include "debug_log.h"
#include "gpio_config.h"
#include "epd_driver.h"
#include "audio_player.h"
#include "button_handler.h"
#include "storage_manager.h"
#include "refresh_scheduler.h"
#include "word_parser.h"
#include "srs_engine.h"
#include "study_mode_machine.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "sync_client.h"
#include "ota_manager.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "MAIN";
#define FW_VERSION  "1.0.0"
#define MAX_WORDS   2048

static WordEntry s_word_pool[MAX_WORDS];

/* 简易 UI 渲染：把单词文本写入帧缓冲（桩实现，真实字体渲染由字库组件完成） */
void ui_render_word(study_mode_t mode, int index)
{
    const WordEntry *w = word_parser_get(index);
    if (!w) return;

    /* TODO: 接入字库(如 esp_lcd + font) 将文本光栅化到帧缓冲。
     * 此处仅做日志输出，便于在串口验证状态机流转。 */
    LOG_I("[%s] #%d  %s  %s", study_mode_name(mode), index, w->text, w->meaning);
}

/* 按键事件回调 */
static void on_button(button_id_t id, button_event_t event)
{
    /* Wi-Fi 配置页激活时，按键全部转发 */
    if (wifi_config_ui_is_active()) {
        wifi_config_ui_on_button(id, event);
        return;
    }

    /* 长按 C 进入 Wi-Fi 配置页 */
    if (id == BUTTON_C && event == BUTTON_EVENT_LONG_PRESS) {
        wifi_config_ui_enter();
        return;
    }

    if (id == BUTTON_D) {
        if (event == BUTTON_EVENT_SHORT_PRESS) {
            /* 切换学习模式 */
            study_mode_switch_next();
            return;
        }
        if (event == BUTTON_EVENT_LONG_PRESS) {
            /* 长按 D：强制全屏清残影 */
            LOG_I("user requested ghost-clear full refresh");
            refresh_force_full();
            return;
        }
    }

    /* 其余按键仅响应短按 */
    if (event != BUTTON_EVENT_SHORT_PRESS) return;

    study_mode_t m = study_mode_current();
    int action = -1;
    switch (id) {
    case BUTTON_A: action = 0; break;   /* prev */
    case BUTTON_B: action = 1; break;   /* next */
    case BUTTON_C: action = 3; break;   /* speak */
    default: break;
    }
    (void)m;
    if (action >= 0) study_mode_handle_action(action);
}

/* 后台心跳 + OTA 检查任务 */
static void background_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(10 * 60 * 1000); /* 10 分钟 */
    while (1) {
        vTaskDelay(period);
        if (wifi_is_connected()) {
            int bat = 100; /* TODO: 读取 ADC 电量 */
            sync_heartbeat(bat, FW_VERSION);

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

void app_main(void)
{
    /* 1. 日志与 NVS */
    log_init();
    LOG_I("=== InkWord firmware %s booting ===", FW_VERSION);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. 存储 / 屏幕 / 音频 / 按键 */
    if (storage_init() == 0) {
        storage_list_dir(SD_MOUNT_POINT);
    } else {
        LOG_W("SD card init failed, running without word DB");
    }

    epd_driver_init();
    epd_clear_screen();                 /* 显示启动白屏 */

    audio_init();
    button_handler_init();
    button_register_callback(on_button);

    /* 3. 刷新调度器（局刷阈值=8） */
    refresh_scheduler_init(8);

    /* 4. WiFi 联网（失败不阻塞主流程） */
    wifi_manager_init();

    /* 4.5 Wi-Fi 配置 UI 初始化；无凭据时自动进入配置页 */
    wifi_config_ui_init();
    if (!wifi_has_saved_credentials()) {
        LOG_W("no saved WiFi, entering config UI");
        wifi_config_ui_enter();
    }

    /* 5. 标记当前固件有效，防止 OTA 回滚 */
    ota_mark_valid();

    /* 6. 加载词库 */
    const char *word_file = SD_MOUNT_POINT "/words.json";
    if (storage_file_exists(word_file)) {
        int n = word_parser_load(word_file, s_word_pool, MAX_WORDS);
        LOG_I("word DB loaded: %d entries", n);
    } else {
        LOG_W("words.json not found on SD card");
    }

    /* 7. 进入上次学习模式 */
    study_mode_init();
    study_mode_handle_action(1);        /* 渲染第一条 */

    /* 8. 启动后台任务（心跳/OTA） */
    xTaskCreate(background_task, "bg", 6 * 1024, NULL, 4, NULL);

    LOG_I("=== InkWord ready ===");

    /* 主任务可退出，工作由按键回调与后台任务事件驱动 */
}
