/**
 * @file wifi_config_ui.c
 * @brief Wi-Fi 配置页面 UI（扫描 / 选择 / 密码输入 / 连接）
 *
 * 使用 epd_driver GFX 包装函数绘图（1bpp 帧缓冲）。
 * 独立 FreeRTOS 任务处理所有 UI 逻辑，按键事件通过队列非阻塞转发。
 *
 * 适配屏幕：416x240 (DEPG0370 横屏 GFX 层)
 * 颜色：EPD_GFX_BLACK (1), EPD_GFX_WHITE (0)
 * 字体大小：1=小, 2=中, 3=大, 4=特大
 *
 * 布局与刷新（2026-08-21 五向版重排）：
 *   - 键盘按横屏 416px 宽重排放大，4 行居中，不再与底栏重叠
 *     （旧版按 240px 竖屏设计：第 4 行 y210~238 压到底栏 y218 文字）
 *   - 页面切换/首帧：整页重绘 + 全刷；光标移动/输入/删除：重绘
 *     内容区（标题栏以下）+ 无窗口局刷单 pass，经 refresh_scheduler
 *     计数达阈值转全刷保养 —— 消除每次按键的整屏黑白闪烁
 */
#include "wifi_config_ui.h"
#include "wifi_manager.h"
#include "button_handler.h"
#include "epd_driver.h"
#include "debug_log.h"
#include "study_mode_machine.h"
#include "refresh_scheduler.h"

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

/* 字体大小映射 */
#define FONT_SM   1
#define FONT_MD   2
#define FONT_LG   3
#define FONT_XL   4

/* 颜色 */
#define C_BLACK  EPD_GFX_BLACK
#define C_WHITE  EPD_GFX_WHITE

/* ---- 页面布局（Phase 4 去硬编码：Y 向由屏高/前序元素派生，
 *      416x240 下与旧字面精确相等；键盘/列表居中已用 SCR_W 动态） ---- */
#define TITLE_H            30    /* 标题栏高（黑底白字，文字基线 21） */
#define BOTTOM_LINE_Y      (epd_gfx_height() - 20) /* 底栏分隔线 y（键盘/列表均止于此之上；240→220，底边距 20） */
#define CONTENT_TOP        TITLE_H /* 局刷重绘区顶：标题栏以下全部重绘 */

/* ---- 密码框 ---- */
#define PWD_BOX_Y          (TITLE_H + 4)  /* 密码框顶：标题栏下 4（34） */
#define PWD_BOX_H          32
#define PWD_SHOW_MAX       36    /* 18pt '*' 掩码最多显示个数（防溢出 400px 框） */

/* ---- 键盘几何（4 行均居中，键宽 416 宽设计值；SMALL 档需按档位缩放，
 *      Phase 7 SMALL 屏接入时处理，见 PANEL_COMPAT_DESIGN §8.2。
 *      2026-08-23 TINY 档（2.13"/2.9" 标签屏竖屏 122~128px 宽）：全键盘
 *      不可行（36px 键宽×9 列=324px>屏宽），本 UI 不适配 TINY——该档
 *      配网走 AP 门户路径（手机浏览器连 InkWord 热点，main.cpp LAN
 *      页左通道），SSSID 列表/键盘页均不进入） ---- */
#define KB_START_Y         (PWD_BOX_Y + PWD_BOX_H + 8) /* 键盘顶：密码框下留 8px（74） */
#define KB_KEY_W           36    /* 行 0/1 字母键宽 */
#define KB_KEY_H           32
#define KB_GAP             3
/* 行 2：Shift(48) + zxcvbnm(7x36) + Del(48)，含 gap 总宽 372 */
static const int kb_w_r2[9] = { 48, 36, 36, 36, 36, 36, 36, 36, 48 };
/* 行 3 功能行：Mode(64) + Space(180) + OK(112)，总宽 362 */
static const int kb_w_r3[3] = { 64, 180, 112 };

/* ---- 列表几何（4 项完整显示且不压底栏） ---- */
#define LIST_ITEM_H        44
#define LIST_START_Y       (TITLE_H + 4) /* 列表顶：标题栏下 4（34） */
#define LIST_MAX_VISIBLE   4

/* ---- 刷新策略 ---- */
#define WIFI_UI_PARTIAL_MAX  10  /* 局刷阈值：达次数转全刷保养（待机12/学习8 之间） */

/* 屏幕尺寸 */
#define SCR_W   epd_gfx_width()
#define SCR_H   epd_gfx_height()

/* ---- 键盘数据 ---- */
typedef enum { KBM_LOWER, KBM_UPPER, KBM_NUM } kb_mode_t;

static const char *kb_chars[3][3] = {
    { "qwertyuiop", "asdfghjkl", "zxcvbnm"   },
    { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"   },
    { "1234567890", "-/:;()$&@", ".,?!'\"#+" },
};

enum {
    KBF_NONE = 0,
    KBF_SHIFT,
    KBF_DEL,
    KBF_MODE,
    KBF_SPACE,
    KBF_CONNECT,
};

static const int kb_row_lens[4] = { 10, 9, 9, 3 };

/* ---- 模块状态 ---- */
typedef struct {
    nav_key_t      id;
    button_event_t event;
} ui_event_t;

#define IS_ENTER_EVT(e)  ((e).id == NAV_KEY_COUNT)

typedef enum {
    WUI_IDLE = 0,
    WUI_LIST,
    WUI_PASSWORD,
    WUI_CONNECT,
    WUI_RESULT,
} wui_state_t;

static QueueHandle_t       s_queue = NULL;
static bool                s_active = false;
static bool                s_inited = false;

static wui_state_t s_state = WUI_IDLE;

static wifi_ap_record_t s_ap_list[MAX_AP_RESULTS];
static int s_ap_count = 0;
static int s_selected = 0;
static int s_list_off = 0;

static char s_ssid[33]     = { 0 };
static char s_password[64] = { 0 };
static int  s_pwd_len      = 0;
static int  s_kb_row       = 0;
static int  s_kb_col       = 0;
static kb_mode_t s_kb_mode = KBM_LOWER;

static bool s_connect_ok = false;

/* ============================================================
 * GFX 绘图封装
 * ============================================================ */

static void ui_clear(void)
{
    epd_gfx_fill_screen(C_WHITE);
}

static void ui_flush(void)
{
    epd_power_on();
    epd_gfx_flush();
    epd_power_off();
}

/**
 * 在矩形区域内居中绘制文本。
 * 注意：GFX setCursor 的 y 坐标是文本基线位置，
 * getTextBounds 返回的 h 包含 ascender+descender。
 */
static void ui_text_center(int bx, int by, int bw, int bh,
                           const char *str, int font_size,
                           bool black_text)
{
    uint16_t color = black_text ? C_BLACK : C_WHITE;
    int tw, th;
    epd_gfx_text_bounds(str, font_size, &tw, &th);
    int cx = bx + (bw - tw) / 2;
    /* 近似基线偏移：将文本垂直居中 */
    int cy = by + (bh - th) / 2 + th * 3 / 4;
    epd_gfx_draw_text(cx, cy, str, color, font_size);
}

/**
 * 在指定位置绘制文本
 */
static void ui_text(int x, int y, const char *str, int font_size, bool black_text)
{
    uint16_t color = black_text ? C_BLACK : C_WHITE;
    epd_gfx_draw_text(x, y, str, color, font_size);
}

/**
 * 文本超宽时截断加省略号（横屏列表 SSID 用，替代旧版固定 15 字符截断）
 */
static void ellipsize(char *buf, int font, int max_w)
{
    int w, h;
    epd_gfx_text_bounds(buf, font, &w, &h);
    while (w > max_w && strlen(buf) > 4) {
        buf[strlen(buf) - 1] = 0;
        int n = (int)strlen(buf);
        if (n >= 4) { buf[n - 1] = '.'; buf[n - 2] = '.'; buf[n - 3] = '.'; }
        epd_gfx_text_bounds(buf, font, &w, &h);
    }
}

/**
 * 局刷阈值预检（带计数副作用）：返回 true 表示本次操作走局刷
 * （计数 +1，调用方重绘内容区后必须执行局刷配对）；返回 false 表示
 * 局刷次数已达保养阈值，调用方需整页重绘走全刷。
 */
static bool partial_ok(void)
{
    return !refresh_gfx_before_partial_n(WIFI_UI_PARTIAL_MAX);
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

static int kb_col_w(int row, int col)
{
    if (row == 2) return kb_w_r2[col];
    if (row == 3) return kb_w_r3[col];
    return KB_KEY_W;
}

static void kb_pixel_rect(int row, int col, int *px, int *py, int *pw, int *ph)
{
    int len = kb_row_lens[row];

    *ph = KB_KEY_H;
    *py = KB_START_Y + row * (KB_KEY_H + KB_GAP);

    int total = (len - 1) * KB_GAP;
    for (int i = 0; i < len; i++) total += kb_col_w(row, i);

    int x = (SCR_W - total) / 2;
    for (int i = 0; i < col; i++) x += kb_col_w(row, i) + KB_GAP;

    *px = x;
    *pw = kb_col_w(row, col);
}

/* ============================================================
 * 页面绘制
 * ============================================================ */

static void draw_scanning(void)
{
    ui_clear();
    ui_text_center(0, SCR_H / 2 - 20, SCR_W, 40,
                   "Scanning Wi-Fi...", FONT_XL, true);
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

    uint16_t fill_c = inverted ? C_WHITE : C_BLACK;
    uint16_t outl_c = fill_c;
    int bw = 5, gap = 2, maxh = 22;

    for (int i = 0; i < 5; i++) {
        int h = maxh * (i + 1) / 5;
        int bx = x + i * (bw + gap);
        int by = y + maxh - h;
        if (i < bars)
            epd_gfx_fill_rect(bx, by, bw, h, fill_c);
        else
            epd_gfx_draw_rect(bx, by, bw, h, outl_c);
    }
}

static void draw_lock_icon(int x, int y, bool inverted)
{
    uint16_t c = inverted ? C_WHITE : C_BLACK;
    epd_gfx_draw_rect(x + 1, y + 6, 10, 8, c);
    epd_gfx_draw_vline(x + 3, y + 1, 5, c);
    epd_gfx_draw_vline(x + 8, y + 1, 5, c);
    epd_gfx_draw_hline(x + 3, y + 1, 6, c);
}

/* 列表内容（列表项 + 滚动条 + 底栏）；标题栏由全刷路径绘制 */
static void draw_list_body(void)
{
    if (s_ap_count == 0) {
        ui_text_center(0, 100, SCR_W, 30, "No networks", FONT_LG, true);
        ui_text_center(0, 140, SCR_W, 20, "Press OK to rescan", FONT_SM, true);
    } else {
        int visible = s_ap_count < LIST_MAX_VISIBLE ? s_ap_count : LIST_MAX_VISIBLE;
        for (int i = 0; i < visible; i++) {
            int idx = s_list_off + i;
            if (idx >= s_ap_count) break;
            int y = LIST_START_Y + i * LIST_ITEM_H;
            bool sel = (idx == s_selected);

            if (sel)
                epd_gfx_fill_rect(10, y, SCR_W - 20, LIST_ITEM_H - 4, C_BLACK);

            char ssid_buf[34];
            strncpy(ssid_buf, (char *)s_ap_list[idx].ssid, 32);
            ssid_buf[32] = 0;
            if (strlen(ssid_buf) == 0) strcpy(ssid_buf, "(hidden)");
            ellipsize(ssid_buf, FONT_LG, SCR_W - 116); /* SSID 可用宽：文本区 30 起至信号条左缘（416→300） */
            ui_text(30, y + 28, ssid_buf, FONT_LG, !sel);

            draw_signal_bars(SCR_W - 72, y + 12, s_ap_list[idx].rssi, sel);

            if (s_ap_list[idx].authmode != WIFI_AUTH_OPEN)
                draw_lock_icon(SCR_W - 28, y + 14, sel);
        }

        if (s_ap_count > LIST_MAX_VISIBLE) {
            int sb_x = SCR_W - 10;
            int sb_h = LIST_MAX_VISIBLE * LIST_ITEM_H;
            int sb_y = LIST_START_Y;
            epd_gfx_draw_rect(sb_x, sb_y, 4, sb_h, C_BLACK);
            int thumb_h = sb_h * LIST_MAX_VISIBLE / s_ap_count;
            int thumb_y = sb_y + sb_h * s_list_off / s_ap_count;
            epd_gfx_fill_rect(sb_x, thumb_y, 4, thumb_h, C_BLACK);
        }
    }

    /* 底部提示栏 */
    epd_gfx_draw_hline(0, BOTTOM_LINE_Y, SCR_W, C_BLACK);
    ui_text_center(0, BOTTOM_LINE_Y + 2, SCR_W, 18,
                   "U/D Select  OK Confirm  LEFT Exit", FONT_SM, true);
}

static void draw_list_page(bool partial)
{
    /* 局刷路径：清内容区 → 重绘 → 单 pass 局刷（无按键闪屏） */
    if (partial && partial_ok()) {
        epd_gfx_fill_rect(0, CONTENT_TOP, SCR_W, SCR_H - CONTENT_TOP, C_WHITE);
        draw_list_body();
        epd_gfx_flush_window_passes(0, CONTENT_TOP, SCR_W,
                                    SCR_H - CONTENT_TOP, 1);
        return;
    }

    ui_clear();

    /* 标题栏（黑底白字） */
    epd_gfx_fill_rect(0, 0, SCR_W, TITLE_H, C_BLACK);
    ui_text(6, 21, "Select Wi-Fi", FONT_MD, false);

    draw_list_body();
    ui_flush();
}

/* 密码内容（密码框 + 键盘 + 底栏）；标题栏由全刷路径绘制 */
static void draw_pwd_body(void)
{
    /* 密码输入框 + 掩码（超长只显示末尾，18pt 光标跟随） */
    epd_gfx_draw_rect(8, PWD_BOX_Y, SCR_W - 16, PWD_BOX_H, C_BLACK);

    int show = s_pwd_len > PWD_SHOW_MAX ? PWD_SHOW_MAX : s_pwd_len;
    char pwd_display[PWD_SHOW_MAX + 2];
    memset(pwd_display, '*', (size_t)show);
    pwd_display[show] = 0;

    int tw, th;
    epd_gfx_text_bounds(pwd_display, FONT_LG, &tw, &th);
    ui_text(14, PWD_BOX_Y + 23, show ? pwd_display : "", FONT_LG, true);
    epd_gfx_draw_vline(14 + tw + 4, PWD_BOX_Y + 6, PWD_BOX_H - 12, C_BLACK);

    /* 绘制键盘（单字符/OK 键用大字号，多字符功能键用中字号） */
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < kb_row_lens[row]; col++) {
            int kx, ky, kw, kh;
            kb_pixel_rect(row, col, &kx, &ky, &kw, &kh);
            bool sel = (row == s_kb_row && col == s_kb_col);

            if (sel)
                epd_gfx_fill_rect(kx, ky, kw, kh, C_BLACK);
            else
                epd_gfx_draw_rect(kx, ky, kw, kh, C_BLACK);

            char label[12];
            kb_label(row, col, label, sizeof(label));
            int kf = (strlen(label) <= 2) ? FONT_LG : FONT_MD;
            ui_text_center(kx, ky, kw, kh, label, kf, !sel);
        }
    }

    /* 底部提示栏 */
    epd_gfx_draw_hline(0, BOTTOM_LINE_Y, SCR_W, C_BLACK);
    ui_text_center(0, BOTTOM_LINE_Y + 2, SCR_W, 18,
                   "U/D L/R Move  OK Key  Hold-L Del  Hold-OK List", FONT_SM, true);
}

static void draw_password_page(bool partial)
{
    /* 局刷路径：光标移动/输入/删除只刷内容区，无按键闪屏 */
    if (partial && partial_ok()) {
        epd_gfx_fill_rect(0, CONTENT_TOP, SCR_W, SCR_H - CONTENT_TOP, C_WHITE);
        draw_pwd_body();
        epd_gfx_flush_window_passes(0, CONTENT_TOP, SCR_W,
                                    SCR_H - CONTENT_TOP, 1);
        return;
    }

    ui_clear();

    /* 标题：SSID */
    epd_gfx_fill_rect(0, 0, SCR_W, TITLE_H, C_BLACK);
    char title[48];
    snprintf(title, sizeof(title), "PWD: %.24s", s_ssid);
    ui_text(6, 21, title, FONT_MD, false);

    draw_pwd_body();
    ui_flush();
}

static void draw_connecting(void)
{
    ui_clear();
    ui_text_center(0, SCR_H / 2 - 20, SCR_W, 40,
                   "Connecting...", FONT_XL, true);
    ui_flush();
}

static void draw_result_page(void)
{
    ui_clear();
    if (s_connect_ok) {
        ui_text_center(0, SCR_H / 2 - 30, SCR_W, 40,
                       "Connected!", FONT_XL, true);
        ui_text_center(0, SCR_H / 2 + 20, SCR_W, 30,
                       "Returning...", FONT_LG, true);
    } else {
        ui_text_center(0, SCR_H / 2 - 30, SCR_W, 40,
                       "Connection Failed", FONT_XL, true);
        ui_text_center(0, SCR_H / 2 + 20, SCR_W, 30,
                       "OK: Retry   LEFT: Back", FONT_LG, true);
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
    study_mode_handle_action(1);
    LOG_I("wifi config UI exited");
}

static void handle_list(nav_key_t id, button_event_t evt)
{
    /* 中长按 / 左短按退出（长按左在 main 层为 AP 门户，此处仅容错旧习惯） */
    if (evt == BUTTON_EVENT_LONG_PRESS && id == NAV_CENTER) {
        exit_config();
        return;
    }
    if (evt != BUTTON_EVENT_SHORT_PRESS) return;

    switch (id) {
    case NAV_UP:
        if (s_selected > 0) s_selected--;
        if (s_selected < s_list_off) s_list_off = s_selected;
        draw_list_page(true);
        break;
    case NAV_DOWN:
        if (s_selected < s_ap_count - 1) s_selected++;
        if (s_selected >= s_list_off + LIST_MAX_VISIBLE)
            s_list_off = s_selected - LIST_MAX_VISIBLE + 1;
        draw_list_page(true);
        break;
    case NAV_CENTER:
        if (s_ap_count == 0) {
            do_scan();
            draw_list_page(false);
        } else {
            strncpy(s_ssid, (char *)s_ap_list[s_selected].ssid, 32);
            s_ssid[32] = 0;
            s_pwd_len = 0;
            s_password[0] = 0;
            s_kb_row = 0;
            s_kb_col = 0;
            s_kb_mode = KBM_LOWER;
            s_state = WUI_PASSWORD;
            draw_password_page(false);
        }
        break;
    case NAV_LEFT:
        exit_config();
        break;
    default:
        break;
    }
}

static void handle_password(nav_key_t id, button_event_t evt)
{
    /* 中长按返回列表；左长按快删（键盘 Del 键的按键侧快捷方式） */
    if (evt == BUTTON_EVENT_LONG_PRESS && id == NAV_CENTER) {
        s_state = WUI_LIST;
        draw_list_page(false);
        return;
    }
    if (evt == BUTTON_EVENT_LONG_PRESS && id == NAV_LEFT) {
        if (s_pwd_len > 0) s_password[--s_pwd_len] = 0;
        draw_password_page(true);
        return;
    }
    if (evt != BUTTON_EVENT_SHORT_PRESS) return;

    int row = s_kb_row, col = s_kb_col;

    switch (id) {
    case NAV_UP:
        if (row > 0) row--;
        if (col >= kb_row_lens[row]) col = kb_row_lens[row] - 1;
        break;
    case NAV_DOWN:
        if (row < 3) row++;
        if (col >= kb_row_lens[row]) col = kb_row_lens[row] - 1;
        break;
    case NAV_LEFT:
        if (col > 0) col--;
        break;
    case NAV_RIGHT:
        if (col < kb_row_lens[row] - 1) col++;
        break;
    case NAV_CENTER: {
        int func = kb_func_at(row, col);
        if (func == KBF_NONE) {
            char ch = kb_char_at(row, col);
            if (ch && s_pwd_len < MAX_PWD_LEN) {
                s_password[s_pwd_len++] = ch;
                s_password[s_pwd_len] = 0;
            }
            if (s_kb_mode == KBM_UPPER) s_kb_mode = KBM_LOWER;
        } else {
            int go_connect = 0;
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
                go_connect = 1;
                break;
            }
            if (go_connect) {
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
                ui_event_t dummy;
                while (xQueueReceive(s_queue, &dummy, 0) == pdPASS) {}
                return;
            }
        }
        break;
    }
    default:
        break;
    }

    s_kb_row = row;
    s_kb_col = col;
    draw_password_page(true);
}

static void handle_result(nav_key_t id, button_event_t evt)
{
    if (evt != BUTTON_EVENT_SHORT_PRESS) return;
    if (s_connect_ok) return;

    switch (id) {
    case NAV_CENTER:
        s_state = WUI_PASSWORD;
        draw_password_page(false);
        break;
    case NAV_LEFT:
        s_state = WUI_LIST;
        draw_list_page(false);
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
            draw_list_page(false);
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
            break;
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
    ui_event_t evt = { .id = NAV_KEY_COUNT, .event = BUTTON_EVENT_NONE };
    xQueueSend(s_queue, &evt, 0);
    LOG_I("wifi config UI enter requested");
}

void wifi_config_ui_on_button(nav_key_t id, button_event_t event)
{
    if (!s_active || !s_queue) return;
    ui_event_t evt = { .id = id, .event = event };
    xQueueSend(s_queue, &evt, 0);
}
