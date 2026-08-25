/**
 * @file settings_ui.c
 * @brief 设置页覆盖层实现（v1.2 T2.5，见 settings_ui.h 分层边界）
 *
 * 渲染自足（不依赖 menu_ui 内部几何宏）：状态栏「设置」+ 4 行项
 * （反选高亮同复习词表范式）+ 底部提示栏；进入全刷，移动/切换局刷
 * 内容区（菜单翻页同策略）。
 */
#include "settings_ui.h"
#include "daily_plan.h"

#include "epd_driver.h"
#include "cjk_text.h"
#include "debug_log.h"
#include "nvs.h"

#include <stdio.h>
#include <stdint.h>

static const char *TAG = "SET";

/* ---- 取值 API：惰性缓存（-1 未加载→NVS 首读；setter 即写即更） ---- */

static int8_t s_audio = -1;
static int8_t s_haptic = -1;
static int8_t s_font = -1;
static int8_t s_quizgrid = -1;   /* v1.5 T5.1：测验快答（2×2 方向直选） */

static int8_t load_u8(const char *key, int8_t def)
{
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        bool hit = nvs_get_u8(h, key, &v) == ESP_OK;
        nvs_close(h);
        if (hit) return (int8_t)v;
    }
    return def;
}

static void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool settings_audio_enabled(void)
{
    if (s_audio < 0) s_audio = load_u8("set_audio", 1);
    return s_audio != 0;
}

bool settings_haptic_enabled(void)
{
    if (s_haptic < 0) s_haptic = load_u8("set_haptic", 1);
    return s_haptic != 0;
}

int settings_font_mode(void)
{
    if (s_font < 0) s_font = load_u8("set_font", 0);
    return s_font ? 1 : 0;
}

bool settings_quiz_grid(void)
{
    if (s_quizgrid < 0) s_quizgrid = load_u8("set_quizgrid", 0);
    return s_quizgrid != 0;
}

/* ---- 覆盖层 UI（menu_ui 范式镜像） ---- */

#define SET_ITEMS 6

static bool s_active = false;
static int  s_sel = 0;          /* 当前编辑行 */

static void draw_row(int row, const char *label, const char *value,
                     int top, int lh, int font_lvl)
{
    int w = epd_gfx_width();
    int margin = w > 200 ? 16 : 8;
    int y = top + row * lh;

    if (row == s_sel)                       /* 反选：黑底白字整行 */
        epd_gfx_fill_rect(margin, y, w - 2 * margin, lh - 4, EPD_GFX_BLACK);
    int fg = (row == s_sel) ? EPD_GFX_WHITE : EPD_GFX_BLACK;
    int base = y + lh * 3 / 4;

    cjk_text_draw(margin + 4, base, font_lvl, label, fg);

    int vw = cjk_text_width(font_lvl, value);
    cjk_text_draw(w - margin - 4 - vw, base, font_lvl, value, fg);
}

static void draw_page(bool full)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    int margin = w > 200 ? 16 : 8;
    int status_h = h < 200 ? 24 : 32;
    int font_lvl = h < 200 ? 0 : 1;         /* 行文字 16/20px */
    int lh = h < 200 ? 28 : 44;             /* 复习词表同款行高 */
    int top = status_h + (h - status_h - (h < 200 ? 18 : 24) - SET_ITEMS * lh) / 2;

    if (full) {
        epd_gfx_fill_screen(EPD_GFX_WHITE);
        cjk_text_draw(margin, (status_h - 16) / 2, 0, "设置", EPD_GFX_BLACK);
        epd_gfx_draw_hline(margin, status_h, w - 2 * margin, EPD_GFX_BLACK);
    } else {
        epd_gfx_fill_rect(0, status_h, w, h - status_h, EPD_GFX_WHITE);
    }

    char v[6][16];
    snprintf(v[0], sizeof(v[0]), "%d", daily_plan_goal());
    {   /* 考试倒计时（v1.5 T5.5）：设置以「N 天后」表达，存绝对 ymd */
        int d = exam_days_left();
        if (d > 0) snprintf(v[4], sizeof(v[4]), "%d 天", d);
        else       snprintf(v[4], sizeof(v[4]), "%s", "关");
    }

    draw_row(0, "每日新词量", v[0], top, lh, font_lvl);
    draw_row(1, "发音", settings_audio_enabled() ? "开" : "关",
             top, lh, font_lvl);
    draw_row(2, "震动", settings_haptic_enabled() ? "开" : "关",
             top, lh, font_lvl);
    draw_row(3, "字号", settings_font_mode() ? "大字" : "标准",
             top, lh, font_lvl);
    draw_row(4, "测验快答", settings_quiz_grid() ? "开" : "关",
             top, lh, font_lvl);
    draw_row(5, "考试倒计时", v[4], top, lh, font_lvl);

    if (full)                               /* 底部提示栏（TINY 档省略） */
        cjk_text_draw(margin, h - (h < 200 ? 16 : 18), 0,
                      "上/下 选择 · 中 切换 · SET 退出", EPD_GFX_BLACK);

    if (full)
        epd_gfx_flush();
    else                                    /* 局刷内容区（菜单翻页同策略） */
        epd_gfx_flush_window(0, status_h, w, h - status_h);
}

void settings_ui_enter(void)
{
    if (s_active) return;                   /* 幂等 */
    s_active = true;
    s_sel = 0;
    draw_page(true);
}

bool settings_ui_is_active(void)
{
    return s_active;
}

/* main.cpp 导出（menu_ui 引用同款先例）；ui_force_font_refresh：
 * 字号档变更后的排版失效标记（UI_MEAN_LEVEL 派生几何变化须全刷重排；
 * reader_engine 默认档仅影响下次无记忆恢复） */
extern void ui_render_current(void);
extern void ui_force_font_refresh(void);

void settings_ui_on_button(nav_key_t id, button_event_t event)
{
    if (!s_active) return;
    if (event == BUTTON_EVENT_LONG_PRESS) return;   /* 长按全忽略防误触 */

    if (event != BUTTON_EVENT_SHORT_PRESS) return;

    switch (id) {
    case NAV_UP:
        s_sel = (s_sel + SET_ITEMS - 1) % SET_ITEMS;
        draw_page(false);
        return;
    case NAV_DOWN:
        s_sel = (s_sel + 1) % SET_ITEMS;
        draw_page(false);
        return;
    case NAV_CENTER: {                      /* 切换当前项（即改即存） */
        switch (s_sel) {
        case 0: {                           /* 每日新词量 +5 循环 5~100 */
            int g = daily_plan_goal() + 5;
            if (g > 100) g = 5;
            daily_plan_set_goal(g);
            break;
        }
        case 1:                             /* 发音开关 */
            s_audio = settings_audio_enabled() ? 0 : 1;
            save_u8("set_audio", (uint8_t)s_audio);
            break;
        case 2:                             /* 震动开关 */
            s_haptic = settings_haptic_enabled() ? 0 : 1;
            save_u8("set_haptic", (uint8_t)s_haptic);
            break;
        case 3:                             /* 字号档循环 标准↔大字 */
            s_font = settings_font_mode() ? 0 : 1;
            save_u8("set_font", (uint8_t)s_font);
            break;
        case 4:                             /* 测验快答（v1.5 T5.1） */
            s_quizgrid = settings_quiz_grid() ? 0 : 1;
            save_u8("set_quizgrid", (uint8_t)s_quizgrid);
            break;
        case 5: {                           /* 考试倒计时（v1.5 T5.5）：
            关→1→…→99→关 循环；存目标日 ymd（自治钟重启不失真）；
            时钟未同步时 exam_set_days 拒写，回显保持「关」 */
            int d = exam_days_left() + 1;
            if (d > 99) d = 0;
            exam_set_days(d);
            break;
        }
        }
        draw_page(false);
        LOG_I("settings: row %d toggled", s_sel);
        return;
    }
    case NAV_SET:
    case NAV_RST:                           /* 退出（菜单退出同语义） */
        s_active = false;
        ui_force_font_refresh();            /* 字号档可能已变（见下） */
        ui_render_current();                /* 恢复学习页（全刷） */
        return;
    default:
        return;
    }
}
