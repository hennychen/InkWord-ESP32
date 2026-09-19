/**
 * @file settings_ui.c
 * @brief 大屏精简版设置页实现 —— 三区范式 + 行局刷（2026-09-17 UI 重设计）
 *
 * 三区（对齐词卡/菜单页范式）：
 *   顶栏 0~120 黑底白字：「设置」
 *   主区 120~1000 白底：行项 140px 垂直居中（发音 开/关、字号
 *        标准/大字/特大、音量 0-100 步进 10 + 十格量程条、关于）。
 *        标签与值统一 32px 点阵（含 ASCII 数字——FreeSans 退出
 *        本页），焦点条黑底白字（PAD 收口全宽，反白即焦点）
 *   底栏 1000~1080 白底：键位提示（上下 选择 · 中键 更改 · SET 返回）
 *
 * 刷新分档：进页/恢复 = GC16 全刷；光标移动/值变更 = DU 列表区
 * 窗口局刷（仅重绘受影响行；4 项 x140=560 < 880 无滚动）。
 *
 * 色彩纪律：EPD_GFX_WHITE/BLACK 宏（本页由暗色底改亮色词典风，
 * 与词卡/菜单页统一——2026-09-17 UI 重设计有意变更）。
 */
#include "settings_ui.h"

#include <stdio.h>
#include <string.h>

#include "cjk_font.h"
#include "cjk_text.h"
#include "debug_log.h"
#include "epd_gfx.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include <stdint.h>

static const char *TAG = "SET";

/* ---- 几何（XLARGE 专属常量，对齐词卡三区） ---- */
#define SET_TOP_H    120   /* 顶栏高（黑底白字） */
#define SET_BOTTOM_H  80   /* 底栏高（键位提示带） */
#define SET_PAD       80   /* 左右收口 */
#define SET_ROW_H    140   /* 行项高 */
#define SET_LIST_Y    SET_TOP_H
#define SET_LIST_H    (1080 - SET_TOP_H - SET_BOTTOM_H)   /* 880 */
/* 4 行 x140=560，主区垂直居中 */
#define SET_ROWS_Y0   (SET_LIST_Y + (SET_LIST_H - 4 * SET_ROW_H) / 2)

/* ---- NVS 键（与 settings_keys.h 同命名空间） ---- */
#define NVS_NS        "inkword"
#define NVS_KEY_AUDIO "set_audio"
#define NVS_KEY_VOL   "set_vol"
#define NVS_KEY_FONT  "set_font"

/* ---- 设置项索引 ---- */
typedef enum {
    SI_AUDIO = 0,   /* 发音 */
    SI_FONT,        /* 字号 */
    SI_VOLUME,      /* 音量 */
    SI_ABOUT,       /* 关于 */
    SI_COUNT
} setting_item_t;

/* ---- 模块状态（静态零初始化） ---- */
static bool s_active = false;
static int  s_sel    = 0;

/* 字号档（0=标准/1=大字/2=特大） */
static int s_font_mode = 0;

/* ---- NVS 读写（惰性缓存） ---- */

static bool nvs_read_bool(const char *key, bool def)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return def;
    uint8_t v = def ? 1 : 0;
    esp_err_t err = nvs_get_u8(h, key, &v);
    nvs_close(h);
    return (err == ESP_OK) ? (v != 0) : def;
}

static void nvs_write_bool(const char *key, bool v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static int nvs_read_int(const char *key, int def)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return def;
    uint8_t v = (uint8_t)def;
    esp_err_t err = nvs_get_u8(h, key, &v);
    nvs_close(h);
    return (err == ESP_OK) ? (int)v : def;
}

static void nvs_write_int(const char *key, int v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, (uint8_t)v);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- 取值 API ---- */

bool settings_audio_enabled(void)
{
    return nvs_read_bool(NVS_KEY_AUDIO, true);
}

int settings_volume(void)
{
    return nvs_read_int(NVS_KEY_VOL, 75);
}

void settings_volume_set(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    nvs_write_int(NVS_KEY_VOL, v);
}

/* ---- 绘制辅助 ---- */

static void draw_title(void)
{
    epd_gfx_fill_rect(0, 0, 1920, SET_TOP_H, EPD_GFX_BLACK);
    cjk_text_draw(SET_PAD, (SET_TOP_H - cjk_glyph_cell_size(2)) / 2,
                  2, "设置", EPD_GFX_WHITE);
}

static void draw_hint(void)
{
    static const char hint[] = "上下 选择 · 中键 更改 · SET 返回";
    cjk_text_draw((1920 - cjk_text_width(1, hint)) / 2,
                  1080 - SET_BOTTOM_H +
                      (SET_BOTTOM_H - cjk_glyph_cell_size(1)) / 2,
                  1, hint, EPD_GFX_BLACK);
}

/* 音量十格量程条（值数字左侧；底线标量程，实心格标当前值） */
static void draw_volume_bar(int y, uint16_t fg)
{
    int vol = settings_volume();
    int cell_w = 48, cell_h = 28, gap = 12;
    int bar_w = 10 * cell_w + 9 * gap;
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d", vol);
    int bx = 1920 - SET_PAD - 40 - cjk_text_width(3, vbuf) - 60 - bar_w;
    int by = y + (SET_ROW_H - cell_h) / 2;

    epd_gfx_fill_rect(bx, by + cell_h + 8, bar_w, 4, fg);   /* 量程底线 */
    for (int i = 0; i < 10; i++) {
        if (vol > i * 10)   /* vol=100 全亮；vol=0 全灭仅底线 */
            epd_gfx_fill_rect(bx + i * (cell_w + gap), by, cell_w,
                              cell_h, fg);
    }
}

/* 行项绘制（idx=全局索引，selected=焦点反白条） */
static void draw_row(int idx, bool selected)
{
    int y = SET_ROWS_Y0 + idx * SET_ROW_H;
    epd_gfx_fill_rect(SET_PAD, y, 1920 - 2 * SET_PAD, SET_ROW_H,
                      selected ? EPD_GFX_BLACK : EPD_GFX_WHITE);
    uint16_t fg = selected ? EPD_GFX_WHITE : EPD_GFX_BLACK;

    const char *label = NULL;
    char value[32] = {0};

    switch (idx) {
    case SI_AUDIO:
        label = "发音";
        snprintf(value, sizeof(value), "%s",
                 settings_audio_enabled() ? "开" : "关");
        break;
    case SI_FONT: {
        static const char *fl[] = { "标准", "大字", "特大" };
        label = "字号";
        snprintf(value, sizeof(value), "%s", fl[s_font_mode]);
        break;
    }
    case SI_VOLUME:
        label = "音量";
        snprintf(value, sizeof(value), "%d", settings_volume());
        break;
    case SI_ABOUT:
        label = "关于";
        snprintf(value, sizeof(value), "v1.0");
        break;
    default:
        return;
    }

    int ty = y + (SET_ROW_H - cjk_glyph_cell_size(3)) / 2;
    cjk_text_draw(SET_PAD + 40, ty, 3, label, fg);
    cjk_text_draw(1920 - SET_PAD - 40 - cjk_text_width(3, value), ty,
                  3, value, fg);
    if (idx == SI_VOLUME) draw_volume_bar(y, fg);
}

/* 全量绘制 + GC16 全刷（进页/render 恢复路径） */
static void draw_main(void)
{
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title();
    draw_hint();
    for (int i = 0; i < SI_COUNT; i++) draw_row(i, i == s_sel);
    epd_gfx_flush();
}

/* 局部重绘行 a（可选联动行 b）+ 列表区窗口 DU 局刷 */
static void redraw_rows(int a, int b)
{
    draw_row(a, a == s_sel);
    if (b >= 0 && b != a) draw_row(b, b == s_sel);
    epd_gfx_flush_window(0, SET_LIST_Y, 1920, SET_LIST_H);
}

/* ---- 动作（activate） ---- */

static void act_audio(void)
{
    bool cur = settings_audio_enabled();
    nvs_write_bool(NVS_KEY_AUDIO, !cur);
    ESP_LOGI(TAG, "audio %s", !cur ? "ON" : "OFF");
    redraw_rows(SI_AUDIO, -1);
}

static void act_font(void)
{
    s_font_mode = (s_font_mode + 1) % 3;
    nvs_write_int(NVS_KEY_FONT, s_font_mode);
    ESP_LOGI(TAG, "font mode=%d", s_font_mode);
    redraw_rows(SI_FONT, -1);
}

static void act_volume(void)
{
    int v = settings_volume();
    v = (v + 10) % 110;  /* 0→10→...→100→0 */
    settings_volume_set(v);
    ESP_LOGI(TAG, "volume=%d", v);
    redraw_rows(SI_VOLUME, -1);
}

static void act_about(void)
{
    /* TODO: 关于页（PSRAM/Flash/版本详情） */
    ESP_LOGI(TAG, "about: bigscreen-app v1.0");
}

static void activate(int idx)
{
    switch (idx) {
    case SI_AUDIO:  act_audio(); break;
    case SI_FONT:   act_font(); break;
    case SI_VOLUME: act_volume(); break;
    case SI_ABOUT:  act_about(); break;
    default: break;
    }
}

/* ---- 退出 ---- */

static void settings_ui_exit(void)
{
    if (!s_active) return;
    s_active = false;
    page_router_pop_if(&g_settings_ui_page);
}

/* ---- 页面协议 ---- */

static void settings_ui_render(void)
{
    draw_main();
}

static bool settings_ui_on_button_internal(nav_key_t id, button_event_t event)
{
    if (event == BUTTON_EVENT_LONG_PRESS) return true;
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;

    switch (id) {
    case NAV_UP:
        s_sel = (s_sel - 1 + SI_COUNT) % SI_COUNT;
        redraw_rows((s_sel + 1) % SI_COUNT, s_sel);
        return true;
    case NAV_DOWN:
        s_sel = (s_sel + 1) % SI_COUNT;
        redraw_rows((s_sel - 1 + SI_COUNT) % SI_COUNT, s_sel);
        return true;
    case NAV_CENTER:
        activate(s_sel);
        return true;
    case NAV_SET:
    case NAV_RST:
        settings_ui_exit();
        return true;
    default:
        return true;
    }
}

void settings_ui_on_button(nav_key_t id, button_event_t event)
{
    settings_ui_on_button_internal(id, event);
}

void settings_ui_enter(void)
{
    if (s_active) return;
    s_active = true;
    s_sel = 0;
    s_font_mode = nvs_read_int(NVS_KEY_FONT, 0);
    page_router_push(&g_settings_ui_page);
}

const page_t g_settings_ui_page = {
    .name = "settings",
    .render = settings_ui_render,
    .on_button = settings_ui_on_button_internal,
    .enter = NULL,
    .exit = NULL,
    .owns_display = false,
};
