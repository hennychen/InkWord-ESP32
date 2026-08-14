/**
 * @file main.cpp
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
#include <Arduino.h>

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
#define MAX_WORDS   64  /* 暂时减少，避免 DRAM 溢出 */

static WordEntry s_word_pool[MAX_WORDS];

/* ============================================================
 * 单词卡片 UI 渲染 + 局部刷新策略 (Task F-16)
 *
 * 布局（240x416 竖屏，FreeSans 基线 y 语义）：
 *   y[0,40)   状态栏：模式名（左）/ 序号（右）/ 分隔线
 *   y[40,416) 内容区：单词(24pt) 音标(9pt) 分隔线 释义(12pt 自动断行) 标签
 *
 * 刷新策略：
 *   - 首帧 / 模式切换 / 清残影后：整屏重绘 + 全刷（epd_gfx_flush）
 *   - 同模式翻页：仅重绘内容区 + 局刷（epd_gfx_flush_window），
 *     状态栏不动；GxEPD2 自动维护 previous 缓冲，UC8253 局刷波形
 *     对像素对差分，只有变化的像素被翻转（无闪烁）
 *   - 残影管理：局刷次数达到阈值时先清屏全刷再整屏重绘
 * ============================================================ */

#define UI_STATUS_H     40    /* 状态栏高度 */
#define UI_MARGIN_X     16    /* 左右留白 */
#define UI_STATUS_BASE  26    /* 状态栏文字基线 y */
#define UI_WORD_BASE    140   /* 单词基线 y (24pt) */
#define UI_PHON_BASE    172   /* 音标基线 y (9pt) */
#define UI_SEP_Y        204   /* 释义区分隔线 y */
#define UI_MEAN_BASE    236   /* 释义首行基线 y (12pt) */
#define UI_MEAN_LH      24    /* 释义行距 */
#define UI_MEAN_LINES   7     /* 释义最大行数 */
#define UI_FOOT_BASE    402   /* 底部标签基线 y (9pt) */

static study_mode_t s_last_mode = MODE_COUNT; /* 无效值：首帧强制全刷 */
static char s_mean_lines[UI_MEAN_LINES][128]; /* 释义断行缓冲 */

/* 简易断行：按空格断词累积，超宽换行；返回实际行数（超出行丢弃） */
static int ui_wrap_meaning(const char *s, int font_size, int max_w)
{
    int n = 0, len = 0;
    if (!s) return 0;
    s_mean_lines[0][0] = '\0';

    while (*s && n < UI_MEAN_LINES) {
        while (*s == ' ' || *s == '\t') s++; /* 跳过前导空白 */
        if (!*s) break;

        const char *word = s; /* 取一个词 */
        while (*s && *s != ' ' && *s != '\t') s++;
        int wlen = (int)(s - word);

        /* 拼接尝试：当前行 + 空格 + 新词 */
        char trial[192];
        int tl = len;
        if (tl + 1 + wlen >= (int)sizeof(trial) - 1) break;
        memcpy(trial, s_mean_lines[n], tl);
        if (tl > 0) trial[tl++] = ' ';
        memcpy(trial + tl, word, wlen);
        tl += wlen;
        trial[tl] = '\0';

        int tw, th;
        epd_gfx_text_bounds(trial, font_size, &tw, &th);
        if (tw <= max_w || len == 0) {
            memcpy(s_mean_lines[n], trial, tl + 1);
            len = tl;
            if (tw > max_w) break; /* 单词自身超宽，独占一行后截断 */
        } else {
            n++;
            if (n >= UI_MEAN_LINES) break;
            if (wlen >= (int)sizeof(s_mean_lines[0]) - 1) wlen = sizeof(s_mean_lines[0]) - 1;
            memcpy(s_mean_lines[n], word, wlen);
            s_mean_lines[n][wlen] = '\0';
            len = wlen;
        }
    }
    return n + 1;
}

/* 绘制状态栏：模式名（左）+ 序号（右）+ 分隔线 */
static void ui_draw_status(study_mode_t mode, int index)
{
    epd_gfx_fill_rect(0, 0, EPD_WIDTH, UI_STATUS_H, EPD_GFX_WHITE);

    epd_gfx_draw_text(UI_MARGIN_X, UI_STATUS_BASE,
                      study_mode_name(mode), EPD_GFX_BLACK, 1);

    char buf[24];
    int total = word_parser_get_count();
    int tw, th;
    snprintf(buf, sizeof(buf), "%d/%d", total ? index + 1 : 0, total);
    epd_gfx_text_bounds(buf, 1, &tw, &th);
    epd_gfx_draw_text(EPD_WIDTH - UI_MARGIN_X - tw, UI_STATUS_BASE,
                      buf, EPD_GFX_BLACK, 1);

    epd_gfx_draw_hline(UI_MARGIN_X, UI_STATUS_H,
                       EPD_WIDTH - 2 * UI_MARGIN_X, EPD_GFX_BLACK);
}

/* 绘制内容区：单词卡片（调用前假定状态栏已存在或无需刷新） */
static void ui_draw_content(const WordEntry *w)
{
    epd_gfx_fill_rect(0, UI_STATUS_H, EPD_WIDTH, EPD_HEIGHT - UI_STATUS_H,
                      EPD_GFX_WHITE);

    epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, w->text, EPD_GFX_BLACK, 4);

    if (w->phonetic[0])
        epd_gfx_draw_text(UI_MARGIN_X, UI_PHON_BASE,
                          w->phonetic, EPD_GFX_BLACK, 1);

    epd_gfx_draw_hline(UI_MARGIN_X, UI_SEP_Y,
                       EPD_WIDTH - 2 * UI_MARGIN_X, EPD_GFX_BLACK);

    int lines = ui_wrap_meaning(w->meaning, 2,
                                EPD_WIDTH - 2 * UI_MARGIN_X);
    for (int i = 0; i < lines; i++)
        epd_gfx_draw_text(UI_MARGIN_X, UI_MEAN_BASE + i * UI_MEAN_LH,
                          s_mean_lines[i], EPD_GFX_BLACK, 2);

    if (w->tag[0])
        epd_gfx_draw_text(UI_MARGIN_X, UI_FOOT_BASE, w->tag, EPD_GFX_BLACK, 1);
}

/* 单词卡片渲染入口：状态机每次画面变化时调用 */
extern "C" void ui_render_word(study_mode_t mode, int index)
{
    if (wifi_config_ui_is_active()) return; /* 配置页期间不绘制学习页 */

    int total = word_parser_get_count();
    const WordEntry *w = total ? word_parser_get(index % total) : NULL;

    if (!w) { /* 词库为空：整屏提示 */
        ui_draw_status(mode, 0);
        epd_gfx_fill_rect(0, UI_STATUS_H, EPD_WIDTH, EPD_HEIGHT - UI_STATUS_H,
                          EPD_GFX_WHITE);
        epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE,
                          "No words", EPD_GFX_BLACK, 2);
        epd_gfx_flush();
        s_last_mode = mode;
        return;
    }

    bool need_full = (mode != s_last_mode);

    /* 残影管理：局刷达阈值时先清屏全刷（清屏后必须整屏重绘） */
    if (!need_full && refresh_gfx_before_partial()) need_full = true;

    if (need_full) {
        ui_draw_status(mode, index);
        ui_draw_content(w);
        epd_gfx_flush(); /* 整屏全刷 */
    } else {
        ui_draw_content(w);
        /* 仅局刷内容区（x/w 已 8 对齐），状态栏不动 */
        epd_gfx_flush_window(0, UI_STATUS_H,
                             EPD_WIDTH, EPD_HEIGHT - UI_STATUS_H);
    }
    s_last_mode = mode;

    LOG_I("[%s] #%d %s", study_mode_name(mode), index, w->text);
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
            /* 切换学习模式并重绘（ui_render_word 检测到模式变化自动全刷） */
            study_mode_switch_next();
            ui_render_word(study_mode_current(), 0);
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

/* Arduino setup - 初始化所有组件 */
void setup()
{
    Serial.begin(115200);
    delay(100);

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
}

/* Arduino loop - 主循环（事件驱动，此处为空） */
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
