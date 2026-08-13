/**
 * @file wifi_config_ui.c
 * @brief Wi-Fi 配置页面 UI 实现（扫描 / 选择 / 密码输入 / 连接）
 *
 * 使用 EPDiy Highlevel API（4bpp PSRAM 帧缓冲 + MODE_GC16 全刷）。
 * 独立 FreeRTOS 任务处理所有 UI 逻辑，按键事件通过队列非阻塞转发。
 *
 * 颜色约定（EPDiy 4bpp）：0x00=黑, 0xF0=白
 */
#include "wifi_config_ui.h"
#include "wifi_manager.h"
#include "button_handler.h"
#include "epd_driver.h"
#include "debug_log.h"
#include "study_mode_machine.h"

#include "epdiy.h"
#include "epd_highlevel.h"
#include "fonts.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "WIFI_UI";

/* ---- 常量 ---- */
#define WIFI_UI_STACK      8192
#define WIFI_UI_QUEUE_LEN  8
#define MAX_AP_RESULTS     20
#define MAX_PWD_LEN        63

/* 键盘几何 */
#define KB_KEY_W           78
#define KB_KEY_H           78
#define KB_GAP             10
#define KB_START_Y         220

/* 列表几何 */
#define LIST_ITEM_H        56
#define LIST_START_Y       95
#define LIST_MAX_VISIBLE   11

/* ---- 键盘数据 ---- */
typedef enum { KBM_LOWER, KBM_UPPER, KBM_NUM } kb_mode_t;

/* 行 0-2 的字符集（行 2 的字符在 col 1-7） */
static const char *kb_chars[3][3] = {
    { "qwertyuiop", "asdfghjkl", "zxcvbnm"   },   /* KBM_LOWER */
    { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"   },   /* KBM_UPPER */
    { "1234567890", "-/:;()$&@", ".,?!'\"#+" },   /* KBM_NUM   */
};

/* 功能键类型 */
enum {
    KBF_NONE = 0,
    KBF_SHIFT,
    KBF_DEL,
    KBF_MODE,
    KBF_SPACE,
    KBF_CONNECT,
};

/* 每行导航列数 */
static const int kb_row_lens[4] = { 10, 9, 9, 3 };

/* ---- 模块状态 ---- */
typedef struct {
    button_id_t    id;
    button_event_t event;
} ui_event_t;

/* 特殊 ENTER 事件哨兵（id = BUTTON_COUNT 表示进入配置页） */
#define IS_ENTER_EVT(e)  ((e).id == BUTTON_COUNT)

typedef enum {
    WUI_IDLE = 0,
    WUI_LIST,
    WUI_PASSWORD,
    WUI_CONNECT,
    WUI_RESULT,
} wui_state_t;

static EpdiyHighlevelState s_hl;
static uint8_t            *s_fb    = NULL;
static QueueHandle_t       s_queue = NULL;
static bool                s_active = false;
static bool                s_inited = false;

static wui_state_t s_state = WUI_IDLE;

/* 列表页状态 */
static wifi_ap_record_t s_ap_list[MAX_AP_RESULTS];
static int s_ap_count = 0;
static int s_selected = 0;
static int s_list_off = 0;

/* 密码页状态 */
static char s_ssid[33]     = { 0 };
static char s_password[64] = { 0 };
static int  s_pwd_len      = 0;
static int  s_kb_row       = 0;
static int  s_kb_col       = 0;
static kb_mode_t s_kb_mode = KBM_LOWER;

/* 结果页状态 */
static bool s_connect_ok = false;

/* ============================================================
 * EPDiy 绘图封装
 * ============================================================ */

static void ui_clear(void)
{
    epd_hl_set_all_white(&s_hl);
}

static void ui_flush(void)
{
    epd_poweron();
    epd_hl_update_screen(&s_hl, MODE_GC16, 25);
    epd_poweroff();
}

static void ui_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    EpdRect r = { .x = x, .y = y, .width = w, .height = h };
    epd_fill_rect(r, color, s_fb);
}

static void ui_draw_rect(int x, int y, int w, int h, uint8_t color)
{
    EpdRect r = { .x = x, .y = y, .width = w, .height = h };
    epd_draw_rect(r, color, s_fb);
}

static void ui_hline(int x, int y, int len, uint8_t color)
{
    epd_draw_hline(x, y, len, color, s_fb);
}

static void ui_vline(int x, int y, int len, uint8_t color)
{
    epd_draw_vline(x, y, len, color, s_fb);
}

/* ---- 文本绘制 ---- */

static void ui_text(int x, int y, const char *str,
                    const EpdFont *font, bool black_text)
{
    EpdFontProperties props = epd_font_properties_default();
    props.fg_color = black_text ? EPD_FONT_FG_BLACK : EPD_FONT_FG_WHITE;
    props.bg_color = black_text ? EPD_FONT_FG_WHITE : EPD_FONT_FG_BLACK;
    int cx = x, cy = y;
    epd_write_string(font, str, &cx, &cy, s_fb, &props);
}

/* 测量文本宽高 */
static void ui_measure(const char *str, const EpdFont *font, int *w, int *h)
{
    int x = 0, y = 0, x1, y1;
    EpdFontProperties props = epd_font_properties_default();
    epd_get_text_bounds(font, str, &x, &y, &x1, &y1, w, h, &props);
}

/* 在矩形区域内居中绘制文本 */
static void ui_text_center(int bx, int by, int bw, int bh,
                           const char *str, const EpdFont *font,
                           bool black_text)
{
    int tw, th;
    ui_measure(str, font, &tw, &th);
    int cx = bx + (bw - tw) / 2;
    /* EPDiy cursor_y 是基线位置，需加上 ascender */
    int cy = by + (bh - th) / 2 + font->ascender;
    ui_text(cx, cy, str, font, black_text);
}

/* ============================================================
 * 键盘辅助函数
 * ============================================================ */

static int kb_func_at(int row, int col)
{
    if (row < 2) return KBF_NONE;
    if (row == 2) {
        if (col == 0) return KBF_SHIFT;
        if (col == 8) return KBF_DEL;
        return KBF_NONE;
    }
    switch (col) {
    case 0:  return KBF_MODE;
    case 1:  return KBF_SPACE;
    case 2:  return KBF_CONNECT;
    default: return KBF_NONE;
    }
}

static char kb_char_at(int row, int col)
{
    if (kb_func_at(row, col) != KBF_NONE) return 0;
    const char *rowstr = kb_chars[s_kb_mode][row < 2 ? row : 2];
    int idx = (row < 2) ? col : (col - 1);
    if (idx < 0 || idx >= (int)strlen(rowstr)) return 0;
    return rowstr[idx];
}

static void kb_label(int row, int col, char *buf, int bufsize)
{
    char ch = kb_char_at(row, col);
    if (ch) {
        snprintf(buf, bufsize, "%c", ch);
        return;
    }
    switch (kb_func_at(row, col)) {
    case KBF_SHIFT:   snprintf(buf, bufsize, "Shift"); break;
    case KBF_DEL:     snprintf(buf, bufsize, "Del");   break;
    case KBF_MODE:    snprintf(buf, bufsize, s_kb_mode == KBM_NUM ? "ABC" : "123"); break;
    case KBF_SPACE:   snprintf(buf, bufsize, "Space"); break;
    case KBF_CONNECT: snprintf(buf, bufsize, "OK");    break;
    default:          buf[0] = 0; break;
    }
}

/* 计算键盘的像素矩形 */
static void kb_pixel_rect(int row, int col, int *px, int *py, int *pw, int *ph)
{
    *ph = KB_KEY_H;
    *py = KB_START_Y + row * (KB_KEY_H + KB_GAP);

    if (row < 3) {
        int len = kb_row_lens[row];
        int total = len * KB_KEY_W + (len - 1) * KB_GAP;
        int sx = (EPD_WIDTH - total) / 2;
        *pw = KB_KEY_W;
        *px = sx + col * (KB_KEY_W + KB_GAP);
    } else {
        /* 第 3 行：三个宽键 */
        int widths[] = { 140, 400, 200 };
        int total = widths[0] + widths[1] + widths[2] + 2 * KB_GAP;
        int sx = (EPD_WIDTH - total) / 2;
        int x = sx;
        for (int i = 0; i < col; i++) x += widths[i] + KB_GAP;
        *pw = widths[col];
        *px = x;
    }
}

/* ============================================================
 * 页面绘制
 * ============================================================ */

static void draw_scanning(void)
{
    ui_clear();
    ui_text_center(0, EPD_HEIGHT / 2 - 20, EPD_WIDTH, 40,
                   "Scanning Wi-Fi...", &FiraSans_24, true);
    ui_flush();
}

static void draw_signal_bars(int x, int y, int8_t rssi, bool inverted)
{
    int bars;
    if (rssi >= -55)      bars = 5;
    else if (rssi >= -66) bars = 4;
    else if (rssi >= -77) bars = 3;
    else if (rssi >= -88) bars = 2;
    else                  bars = 1;

    uint8_t fill_c = inverted ? EPD_DRAW_WHITE : EPD_DRAW_BLACK;
    uint8_t outl_c = fill_c;
    int bw = 5, gap = 2, maxh = 22;

    for (int i = 0; i < 5; i++) {
        int h = maxh * (i + 1) / 5;
        int bx = x + i * (bw + gap);
        int by = y + maxh - h;
        if (i < bars)
            ui_fill_rect(bx, by, bw, h, fill_c);
        else
            ui_draw_rect(bx, by, bw, h, outl_c);
    }
}

static void draw_lock_icon(int x, int y, bool inverted)
{
    uint8_t c = inverted ? EPD_DRAW_WHITE : EPD_DRAW_BLACK;
    /* 简易挂锁：锁体矩形 + 锁弧（用线条近似） */
    ui_draw_rect(x + 1, y + 6, 10, 8, c);
    ui_vline(x + 3, y + 1, 5, c);
    ui_vline(x + 8, y + 1, 5, c);
    ui_hline(x + 3, y + 1, 6, c);
}

static void draw_list_page(void)
{
    ui_clear();

    /* 标题栏（黑底白字） */
    ui_fill_rect(0, 0, EPD_WIDTH, 70, EPD_DRAW_BLACK);
    ui_text(20, 20, "Wi-Fi Setup - Select Network", &FiraSans_24, false);

    if (s_ap_count == 0) {
        ui_text_center(0, 300, EPD_WIDTH, 30, "No networks found", &FiraSans_18, true);
        ui_text_center(0, 350, EPD_WIDTH, 20, "Press C to rescan", &FiraSans_12, true);
    } else {
        int visible = s_ap_count < LIST_MAX_VISIBLE ? s_ap_count : LIST_MAX_VISIBLE;
        for (int i = 0; i < visible; i++) {
            int idx = s_list_off + i;
            if (idx >= s_ap_count) break;
            int y = LIST_START_Y + i * LIST_ITEM_H;
            bool sel = (idx == s_selected);

            /* 选中项反白 */
            if (sel)
                ui_fill_rect(10, y, EPD_WIDTH - 20, LIST_ITEM_H - 4, EPD_DRAW_BLACK);

            /* SSID（截断至 28 字符） */
            char ssid_buf[34];
            strncpy(ssid_buf, (char *)s_ap_list[idx].ssid, 32);
            ssid_buf[32] = 0;
            if (strlen(ssid_buf) == 0) strcpy(ssid_buf, "(hidden)");
            if ((int)strlen(ssid_buf) > 28) {
                ssid_buf[28] = 0;
            }
            ui_text(30, y + 18, ssid_buf, &FiraSans_18, !sel);

            /* 信号强度 */
            draw_signal_bars(EPD_WIDTH - 200, y + 14, s_ap_list[idx].rssi, sel);

            /* 加密图标 */
            if (s_ap_list[idx].authmode != WIFI_AUTH_OPEN)
                draw_lock_icon(EPD_WIDTH - 80, y + 16, sel);
        }

        /* 滚动条 */
        if (s_ap_count > LIST_MAX_VISIBLE) {
            int sb_x = EPD_WIDTH - 12;
            int sb_h = LIST_MAX_VISIBLE * LIST_ITEM_H;
            int sb_y = LIST_START_Y;
            ui_draw_rect(sb_x, sb_y, 4, sb_h, EPD_DRAW_BLACK);
            int thumb_h = sb_h * LIST_MAX_VISIBLE / s_ap_count;
            int thumb_y = sb_y + sb_h * s_list_off / s_ap_count;
            ui_fill_rect(sb_x, thumb_y, 4, thumb_h, EPD_DRAW_BLACK);
        }
    }

    /* 底部提示栏 */
    ui_hline(0, 760, EPD_WIDTH, EPD_DRAW_BLACK);
    ui_text_center(0, 770, EPD_WIDTH, 20,
                   "A/B Select   C Confirm   D(Long) Exit", &FiraSans_12, true);

    ui_flush();
}

static void draw_password_page(void)
{
    ui_clear();

    /* 标题：SSID */
    ui_fill_rect(0, 0, EPD_WIDTH, 50, EPD_DRAW_BLACK);
    char title[48];
    snprintf(title, sizeof(title), "Password: %.32s", s_ssid);
    ui_text(20, 14, title, &FiraSans_24, false);

    /* 密码输入框 */
    ui_draw_rect(20, 65, EPD_WIDTH - 40, 55, EPD_DRAW_BLACK);

    /* 密码星号显示 */
    char pwd_display[68];
    int p = 0;
    for (int i = 0; i < s_pwd_len && p < 66; i++)
        pwd_display[p++] = '*';
    pwd_display[p] = 0;
    ui_text(35, 78, pwd_display, &FiraSans_18, true);

    /* 光标指示 */
    if (p > 0) {
        int tw, th;
        ui_measure(pwd_display, &FiraSans_18, &tw, &th);
        ui_vline(35 + tw + 3, 75, 35, EPD_DRAW_BLACK);
    } else {
        ui_vline(37, 75, 35, EPD_DRAW_BLACK);
    }

    /* 绘制键盘 */
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < kb_row_lens[row]; col++) {
            int kx, ky, kw, kh;
            kb_pixel_rect(row, col, &kx, &ky, &kw, &kh);
            bool sel = (row == s_kb_row && col == s_kb_col);

            if (sel)
                ui_fill_rect(kx, ky, kw, kh, EPD_DRAW_BLACK);
            else
                ui_draw_rect(kx, ky, kw, kh, EPD_DRAW_BLACK);

            char label[12];
            kb_label(row, col, label, sizeof(label));
            const EpdFont *kf = (row == 3) ? &FiraSans_18 : &FiraSans_24;
            ui_text_center(kx, ky, kw, kh, label, kf, !sel);
        }
    }

    /* 底部提示 */
    ui_hline(0, 760, EPD_WIDTH, EPD_DRAW_BLACK);
    ui_text_center(0, 770, EPD_WIDTH, 20,
                   "A/B/E/F Move   C Input   D Del   D(Long) Back",
                   &FiraSans_12, true);

    ui_flush();
}

static void draw_connecting(void)
{
    ui_clear();
    ui_text_center(0, EPD_HEIGHT / 2 - 20, EPD_WIDTH, 40,
                   "Connecting...", &FiraSans_24, true);
    ui_flush();
}

static void draw_result_page(void)
{
    ui_clear();
    if (s_connect_ok) {
        ui_text_center(0, EPD_HEIGHT / 2 - 30, EPD_WIDTH, 40,
                       "Connected!", &FiraSans_24, true);
        ui_text_center(0, EPD_HEIGHT / 2 + 20, EPD_WIDTH, 30,
                       "Returning...", &FiraSans_18, true);
    } else {
        ui_text_center(0, EPD_HEIGHT / 2 - 30, EPD_WIDTH, 40,
                       "Connection Failed", &FiraSans_24, true);
        ui_text_center(0, EPD_HEIGHT / 2 + 20, EPD_WIDTH, 30,
                       "C: Retry   D: Back", &FiraSans_18, true);
    }
    ui_flush();
}

/* ============================================================
 * 事件处理
 * ============================================================ */

static void do_scan(void)
{
    draw_scanning();
    s_ap_count = wifi_scan(s_ap_list, MAX_AP_RESULTS);
    s_selected = 0;
    s_list_off = 0;
    if (s_ap_count < 0) s_ap_count = 0;
    LOG_I("scan complete: %d networks", s_ap_count);
}

static void exit_config(void)
{
    s_active = false;
    s_state  = WUI_IDLE;
    ui_clear();
    ui_flush();
    /* 恢复学习模式画面 */
    study_mode_handle_action(1);
    LOG_I("wifi config UI exited");
}

/* 列表页按键处理 */
static void handle_list(button_id_t id, button_event_t evt)
{
    if (evt == BUTTON_EVENT_LONG_PRESS && id == BUTTON_D) {
        exit_config();
        return;
    }
    if (evt != BUTTON_EVENT_SHORT_PRESS) return;

    switch (id) {
    case BUTTON_A:   /* 上移 */
        if (s_selected > 0) s_selected--;
        if (s_selected < s_list_off) s_list_off = s_selected;
        draw_list_page();
        break;
    case BUTTON_B:   /* 下移 */
        if (s_selected < s_ap_count - 1) s_selected++;
        if (s_selected >= s_list_off + LIST_MAX_VISIBLE)
            s_list_off = s_selected - LIST_MAX_VISIBLE + 1;
        draw_list_page();
        break;
    case BUTTON_C:   /* 确认 */
        if (s_ap_count == 0) {
            do_scan();
            draw_list_page();
        } else {
            strncpy(s_ssid, (char *)s_ap_list[s_selected].ssid, 32);
            s_ssid[32] = 0;
            s_pwd_len = 0;
            s_password[0] = 0;
            s_kb_row = 0;
            s_kb_col = 0;
            s_kb_mode = KBM_LOWER;
            s_state = WUI_PASSWORD;
            draw_password_page();
        }
        break;
    case BUTTON_D:   /* 短按退出 */
        exit_config();
        break;
    default:
        break;
    }
}

/* 密码页按键处理 */
static void handle_password(button_id_t id, button_event_t evt)
{
    /* 长按 D：返回列表页 */
    if (evt == BUTTON_EVENT_LONG_PRESS && id == BUTTON_D) {
        s_state = WUI_LIST;
        draw_list_page();
        return;
    }
    if (evt != BUTTON_EVENT_SHORT_PRESS) return;

    int row = s_kb_row, col = s_kb_col;

    switch (id) {
    case BUTTON_A:   /* 上 */
        if (row > 0) row--;
        if (col >= kb_row_lens[row]) col = kb_row_lens[row] - 1;
        break;
    case BUTTON_B:   /* 下 */
        if (row < 3) row++;
        if (col >= kb_row_lens[row]) col = kb_row_lens[row] - 1;
        break;
    case BUTTON_E:   /* 左 */
        if (col > 0) col--;
        break;
    case BUTTON_F:   /* 右 */
        if (col < kb_row_lens[row] - 1) col++;
        break;
    case BUTTON_C: { /* 输入 / 确认 */
        int func = kb_func_at(row, col);
        if (func == KBF_NONE) {
            char ch = kb_char_at(row, col);
            if (ch && s_pwd_len < MAX_PWD_LEN) {
                s_password[s_pwd_len++] = ch;
                s_password[s_pwd_len] = 0;
            }
            /* 大写模式输入后自动回小写 */
            if (s_kb_mode == KBM_UPPER) s_kb_mode = KBM_LOWER;
        } else {
            switch (func) {
            case KBF_SHIFT:
                s_kb_mode = (s_kb_mode == KBM_UPPER) ? KBM_LOWER : KBM_UPPER;
                break;
            case KBF_DEL:
                if (s_pwd_len > 0) s_password[--s_pwd_len] = 0;
                break;
            case KBF_MODE:
                s_kb_mode = (s_kb_mode == KBM_NUM) ? KBM_LOWER : KBM_NUM;
                break;
            case KBF_SPACE:
                if (s_pwd_len < MAX_PWD_LEN) {
                    s_password[s_pwd_len++] = ' ';
                    s_password[s_pwd_len] = 0;
                }
                break;
            case KBF_CONNECT:
                /* 发起连接 */
                s_state = WUI_CONNECT;
                draw_connecting();
                s_password[s_pwd_len] = 0;
                LOG_I("connecting to '%s'...", s_ssid);
                int ret = wifi_connect(s_ssid, s_password);
                s_connect_ok = (ret == 0);
                s_state = WUI_RESULT;
                draw_result_page();
                if (s_connect_ok) {
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    exit_config();
                }
                /* 清理连接期间积累的队列事件 */
                ui_event_t dummy;
                while (xQueueReceive(s_queue, &dummy, 0) == pdPASS) {}
                return;
            }
        }
        break;
    }
    case BUTTON_D:   /* 短按删除 */
        if (s_pwd_len > 0) s_password[--s_pwd_len] = 0;
        break;
    default:
        break;
    }

    s_kb_row = row;
    s_kb_col = col;
    draw_password_page();
}

/* 结果页按键处理 */
static void handle_result(button_id_t id, button_event_t evt)
{
    if (evt != BUTTON_EVENT_SHORT_PRESS) return;
    if (s_connect_ok) return;

    switch (id) {
    case BUTTON_C:   /* 重试：回密码页 */
        s_state = WUI_PASSWORD;
        draw_password_page();
        break;
    case BUTTON_D:   /* 返回列表 */
        s_state = WUI_LIST;
        draw_list_page();
        break;
    default:
        break;
    }
}

/* ============================================================
 * UI 任务
 * ============================================================ */

static void wifi_ui_task(void *arg)
{
    (void)arg;
    ui_event_t evt;

    while (1) {
        if (xQueueReceive(s_queue, &evt, portMAX_DELAY) != pdPASS) continue;
        if (!s_active) continue;

        if (IS_ENTER_EVT(evt)) {
            s_state = WUI_LIST;
            do_scan();
            draw_list_page();
            continue;
        }

        switch (s_state) {
        case WUI_LIST:
            handle_list(evt.id, evt.event);
            break;
        case WUI_PASSWORD:
            handle_password(evt.id, evt.event);
            break;
        case WUI_CONNECT:
            break;   /* 连接过程中忽略按键 */
        case WUI_RESULT:
            handle_result(evt.id, evt.event);
            break;
        default:
            break;
        }
    }
}

/* ============================================================
 * 公共 API
 * ============================================================ */

void wifi_config_ui_init(void)
{
    if (s_inited) return;

    /* 初始化 EPDiy Highlevel（分配 PSRAM 帧缓冲） */
    s_hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    s_fb = epd_hl_get_framebuffer(&s_hl);

    s_queue = xQueueCreate(WIFI_UI_QUEUE_LEN, sizeof(ui_event_t));
    xTaskCreate(wifi_ui_task, "wifi_ui", WIFI_UI_STACK, NULL, 6, NULL);

    s_inited = true;
    LOG_I("wifi config UI module initialized");
}

bool wifi_config_ui_is_active(void)
{
    return s_active;
}

void wifi_config_ui_enter(void)
{
    if (!s_inited) wifi_config_ui_init();
    s_active = true;
    /* 发送 ENTER 事件触发首次扫描 */
    ui_event_t evt = { .id = BUTTON_COUNT, .event = BUTTON_EVENT_NONE };
    xQueueSend(s_queue, &evt, 0);
    LOG_I("wifi config UI enter requested");
}

void wifi_config_ui_on_button(button_id_t id, button_event_t event)
{
    if (!s_active || !s_queue) return;
    ui_event_t evt = { .id = id, .event = event };
    xQueueSend(s_queue, &evt, 0);
}
