/**
 * @file main.cpp
 * @brief 应用主入口 (Task F-20)
 *
 * 启动流程：日志 -> NVS -> 存储(SD) -> 屏幕 -> 音频 -> 按键 ->
 *          WiFi -> 词库 -> 进入上次模式 -> 主循环（事件驱动）。
 *
 * 五向导航按键映射（2026-08 取代 6 独立按键；SET/RST 侧键同月接入）：
 *   上 短按=上一条 / 长按=清残影全刷；
 *   下 短按=下一条 / 长按=切换学习模式；
 *   中 短按=发音 / 长按=进入 Wi-Fi 配置；
 *   左 短按预留（释义滚动扩展）/ 长按=进入 AP 直连/配网门户
 *        （手机连 InkWord-Setup 热点直传，绕开路由器隔离；任意键退出）；
 *   右 短按预留 / 长按=进入 LAN 接收页（同网浏览器直传，任意键退出）；
 *   SET 短按=遮蔽/揭晓释义（闪卡自测；待机页=轮换下一条引文）/ 长按预留 SRS「记得」；
 *   RST 短按=回到当前模式第一条 / 长按预留 SRS「忘了」。
 *
 * 无词库待机页（词库为空时默认显示，见 standby_page.c）：
 *   时钟/日历/天气整页；待机态长按语义与学习页一致（配网/清残影/门户/LAN），
 *   短按中=立即拉取天气，其余短按忽略。
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
#include "lan_display_server.h"
#include "standby_page.h"
#include "ble_provision.h"

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

/* BLE 配网服务默认禁用：Arduino 预编译库未编入 Wi-Fi/BLE coexistence
 * （CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE 编译期固定关闭），Wi-Fi controller
 * 活动时 esp_bt_controller_enable 经 coex_enable() abort（真机崩溃循环）。
 * 迁移到可开 coex 的构建（IDF 框架/自编译 libs）后置 1 启用。 */
#ifndef INKWORD_BLE_PROVISION
#define INKWORD_BLE_PROVISION 0
#endif

static WordEntry s_word_pool[MAX_WORDS];

/* ============================================================
 * 单词卡片 UI 渲染 + 局部刷新策略 (Task F-16)
 *
 * 布局（GFX 横屏 416x240，rotation=1，FreeSans 基线 y 语义）：
 *   y[0,32)    状态栏：模式名（左）/ 序号（右）/ 分隔线
 *   左栏 x[16,248)  单词(24pt 超宽自动降级) + 音标(9pt) + 底部标签
 *   竖分隔线 x=248；右栏 x[264,400) 释义(12pt 自动断行 ≤6 行)
 *
 * 刷新策略：
 *   - 首帧 / 模式切换 / 清残影后：整屏重绘 + 全刷（epd_gfx_flush）
 *   - 同模式翻页：仅重绘内容区 + 局刷（epd_gfx_flush_window），
 *     状态栏不动；GxEPD2 自动维护 previous 缓冲，UC8253 局刷波形
 *     对像素对差分，只有变化的像素被翻转（无闪烁）
 *   - 残影管理：局刷次数达到阈值时先清屏全刷再整屏重绘
 * ============================================================ */

#define UI_STATUS_H     32    /* 状态栏高度（rotation=1 下局刷窗口 y/h 需 8 对齐） */
#define UI_MARGIN_X     16    /* 左右留白 */
#define UI_STATUS_BASE  22    /* 状态栏文字基线 y */
#define UI_WORD_BASE    100   /* 单词基线 y（左栏，24pt 超宽自动降级） */
#define UI_PHON_BASE    132   /* 音标基线 y（左栏，9pt） */
#define UI_VSEP_X       248   /* 左右分栏竖线 x */
#define UI_MEAN_X       264   /* 释义起始 x（右栏） */
#define UI_MEAN_BASE    62    /* 释义首行基线 y（右栏，12pt） */
#define UI_MEAN_LH      26    /* 释义行距 */
#define UI_MEAN_LINES   6     /* 释义最大行数 */
#define UI_MEAN_MAX_W   (EPD_GFX_WIDTH - UI_MEAN_X - UI_MARGIN_X) /* 右栏文本宽 */
#define UI_WORD_MAX_W   (UI_VSEP_X - 2 * UI_MARGIN_X)             /* 左栏文本宽 */
#define UI_FOOT_BASE    224   /* 左栏底部标签基线 y（9pt） */

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

/* 字号自适应：从 start_size 逐级降到能放进 max_w 的字号 */
static int ui_fit_font(const char *text, int start_size, int max_w)
{
    int tw, th;
    for (int fs = start_size; fs >= 1; fs--) {
        epd_gfx_text_bounds(text, fs, &tw, &th);
        if (tw <= max_w) return fs;
    }
    return 1;
}

/* 绘制状态栏：模式名（左）+ 序号（右）+ 分隔线 */
static void ui_draw_status(study_mode_t mode, int index)
{
    epd_gfx_fill_rect(0, 0, EPD_GFX_WIDTH, UI_STATUS_H, EPD_GFX_WHITE);

    epd_gfx_draw_text(UI_MARGIN_X, UI_STATUS_BASE,
                      study_mode_name(mode), EPD_GFX_BLACK, 1);

    char buf[24];
    int total = word_parser_get_count();
    int tw, th;
    snprintf(buf, sizeof(buf), "%d/%d", total ? index + 1 : 0, total);
    epd_gfx_text_bounds(buf, 1, &tw, &th);
    epd_gfx_draw_text(EPD_GFX_WIDTH - UI_MARGIN_X - tw, UI_STATUS_BASE,
                      buf, EPD_GFX_BLACK, 1);

    epd_gfx_draw_hline(UI_MARGIN_X, UI_STATUS_H,
                       EPD_GFX_WIDTH - 2 * UI_MARGIN_X, EPD_GFX_BLACK);
}

/* 绘制内容区：左栏单词卡片 + 竖分隔线 + 右栏释义 */
static void ui_draw_content(const WordEntry *w)
{
    epd_gfx_fill_rect(0, UI_STATUS_H, EPD_GFX_WIDTH,
                      EPD_GFX_HEIGHT - UI_STATUS_H, EPD_GFX_WHITE);

    epd_gfx_draw_text(UI_MARGIN_X, UI_WORD_BASE, w->text, EPD_GFX_BLACK,
                      ui_fit_font(w->text, 4, UI_WORD_MAX_W));

    if (w->phonetic[0])
        epd_gfx_draw_text(UI_MARGIN_X, UI_PHON_BASE,
                          w->phonetic, EPD_GFX_BLACK, 1);

    epd_gfx_draw_vline(UI_VSEP_X, UI_STATUS_H + 16,
                       EPD_GFX_HEIGHT - UI_STATUS_H - 32, EPD_GFX_BLACK);

    int lines;
    if (study_mode_is_revealed()) {
        lines = ui_wrap_meaning(w->meaning, 2, UI_MEAN_MAX_W);
        for (int i = 0; i < lines; i++)
            epd_gfx_draw_text(UI_MEAN_X, UI_MEAN_BASE + i * UI_MEAN_LH,
                              s_mean_lines[i], EPD_GFX_BLACK, 2);
    } else {
        /* 遮蔽自测态：右栏仅提示，不画释义（SET 揭晓） */
        epd_gfx_draw_text(UI_MEAN_X, UI_MEAN_BASE,
                          "[SET] to reveal", EPD_GFX_BLACK, 1);
    }

    if (w->tag[0])
        epd_gfx_draw_text(UI_MARGIN_X, UI_FOOT_BASE, w->tag, EPD_GFX_BLACK, 1);
}

/* LAN 直传外部内容整帧直刷后调用：GFX previous 缓冲已失配，
 * 置 s_last_mode 无效值强制下一次学习界面渲染走全刷 */
extern "C" void ui_force_full_refresh_next(void)
{
    s_last_mode = MODE_COUNT;
}

/* 单词卡片渲染入口：状态机每次画面变化时调用 */
extern "C" void ui_render_word(study_mode_t mode, int index)
{
    if (wifi_config_ui_is_active()) return; /* 配置页期间不绘制学习页 */
    if (lan_server_is_active()) return;     /* LAN 接收页期间不绘制学习页 */

    int total = word_parser_get_count();
    const WordEntry *w = total ? word_parser_get(index % total) : NULL;

    if (!w) { /* 词库为空：切换到待机页（时钟/日历/天气） */
        standby_render_full();
        s_last_mode = MODE_COUNT; /* 保证日后有词时首帧全刷 */
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
        /* 仅局刷内容区（rotation=1 要求 y/h 8 对齐：y=32, h=208） */
        epd_gfx_flush_window(0, UI_STATUS_H,
                             EPD_GFX_WIDTH, EPD_GFX_HEIGHT - UI_STATUS_H);
    }
    s_last_mode = mode;

    LOG_I("[%s] #%d %s", study_mode_name(mode), index, w->text);
}

/* 当前应显示页面的统一渲染入口：有词库走学习页，无词库走待机页
 * （LAN/配网退出与模式切换后的恢复路径均经此路由） */
static void ui_render_current(void)
{
    if (word_parser_get_count() > 0)
        ui_render_word(study_mode_current(), 0);
    else
        standby_render_full();
}

/* 五向导航按键事件回调 */
static void on_button(nav_key_t id, button_event_t event)
{
    /* Wi-Fi 配置页激活时，按键全部转发 */
    if (wifi_config_ui_is_active()) {
        wifi_config_ui_on_button(id, event);
        return;
    }

    /* LAN 接收页 / AP portal 激活时，任意按键退出并回到学习界面
     * （portal 模式下 lan_portal_exit 关热点回 STA；均为幂等调用） */
    if (lan_server_is_active()) {
        lan_portal_exit();
        lan_server_leave_receive_page();
        ui_render_current();
        return;
    }

    /* 待机页激活时（词库为空），按键交给待机页处理 */
    if (standby_is_active()) {
        standby_on_button(id, event);
        return;
    }

    /* 长按功能集中在五键上：中=配网，上=清残影，下=模式切换，
     * 左=AP 门户（隔离环境下 STA 页面不可达时的可靠通道），
     * 右=LAN 接收页 */
    if (event == BUTTON_EVENT_LONG_PRESS) {
        switch (id) {
        case NAV_CENTER:
            wifi_config_ui_enter();
            return;
        case NAV_UP:
            LOG_I("user requested ghost-clear full refresh");
            refresh_force_full();
            return;
        case NAV_DOWN:
            /* 切换学习模式并重绘（ui_render_word 检测到模式变化自动全刷） */
            study_mode_switch_next();
            ui_render_current();
            return;
        case NAV_LEFT:
            lan_portal_enter();
            return;
        case NAV_RIGHT:
            /* 幂等启动服务器并显示访问 URL */
            lan_server_enter_receive_page();
            return;
        default:
            return;
        }
    }

    /* 短按：上/下翻词，中=发音，SET=遮蔽/揭晓释义，RST=回第一条；
     * 左/右预留（释义滚动扩展）；SET/RST 长按预留 SRS 记得/忘了评分
     * （SM-2 闭环接入学习记录上报后启用，见 srs_engine.h 质量分） */
    if (event != BUTTON_EVENT_SHORT_PRESS) return;

    switch (id) {
    case NAV_UP:
        study_mode_handle_action(0);   /* prev */
        return;
    case NAV_DOWN:
        study_mode_handle_action(1);   /* next */
        return;
    case NAV_CENTER:
        study_mode_handle_action(3);   /* speak */
        return;
    case NAV_SET:
        study_mode_handle_action(2);   /* confirm：遮蔽/揭晓释义 */
        return;
    case NAV_RST:
        study_mode_reset_cursor();     /* 回到当前模式第一条 */
        return;
    default:
        return;
    }
}

/* 后台心跳 + OTA 检查任务。
 * 启动阶段：每 2s 轮询，联网即立即启动 LAN 直传服务（不设上限：
 *           即使路由器后启动/断电恢复，联网后也能尽快拉起服务）；
 * 之后转为 10 分钟周期：心跳 + OTA 检查（含服务兜底重启，幂等） */
static void background_task(void *arg)
{
    (void)arg;
    while (!lan_server_is_running()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (wifi_is_connected()) {
            lan_server_start(); /* 幂等 */
        }
    }
    const TickType_t period = pdMS_TO_TICKS(10 * 60 * 1000); /* 10 分钟 */
    int wx_poll_cnt = 2; /* 待机页天气轮询计数：初始 2 -> 首个周期即拉取 */
    while (1) {
        vTaskDelay(period);
        if (wifi_is_connected()) {
            lan_server_start(); /* 兜底：服务异常停止则重启（幂等） */

            int bat = 100; /* TODO: 读取 ADC 电量 */
            sync_heartbeat(bat, FW_VERSION);

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

    /* 4.5 Wi-Fi 配置：无凭据时自动开启 AP 配网门户
     *     （手机连 InkWord-Setup 热点后自动弹出配置页）；
     *     软键盘配置 UI 仍可长按 C 进入 */
    wifi_config_ui_init();
    if (!wifi_has_saved_credentials()) {
        LOG_W("no saved WiFi, starting AP portal");
        lan_portal_enter();
    }

    /* 4.6 BLE 配网服务（App 扫描发现/配网；失败仅告警，
     *     Portal 与软键盘配网路径不受影响）。
     *     默认禁用，根因见文件头 INKWORD_BLE_PROVISION 注释 */
#if INKWORD_BLE_PROVISION
    ble_provision_init();
#endif

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

    /* 7. 初始化待机页（恢复 NVS 天气缓存），进入上次学习模式；
     *    无词库时渲染待机页（时钟/日历/天气） */
    standby_init();
    study_mode_init();
    if (word_parser_get_count() > 0) {
        study_mode_handle_action(1);    /* 渲染第一条 */
    } else {
        standby_render_full();
    }

    /* 8. 启动后台任务（心跳/OTA） */
    xTaskCreate(background_task, "bg", 6 * 1024, NULL, 4, NULL);

    LOG_I("=== InkWord ready ===");
}

/* Arduino loop - 主循环（事件驱动；待机页分钟级心跳由 standby_tick 承载，
 * 非待机状态时零开销返回） */
void loop()
{
    standby_tick();
    vTaskDelay(pdMS_TO_TICKS(1000));
}
