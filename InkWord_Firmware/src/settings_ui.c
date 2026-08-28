/**
 * @file settings_ui.c
 * @brief 设置页覆盖层实现（v1.2 T2.5，见 settings_ui.h 分层边界）
 *
 * 渲染自足（不依赖 menu_ui 内部几何宏）：状态栏「设置」+ 10 行项
 * （反选高亮同复习词表范式）+ 底部提示栏（TINY 档省略）；进入全刷，
 * 移动/切换局刷内容区（菜单翻页同策略）。几何按布局档位派生
 * （2026-08-25 起，TINY 竖屏修正见 draw_page 注）。
 *
 * 滚动窗口（2026-08-27）：7 行后短屏放不下（MID 240 高横屏可用
 * 184px < 7×44，原居中公式 top=-30 末行 y=234 出屏——屏幕方向项
 * 不可见真机实测），行数超容量时改滚动窗口：选中行驱动窗口滚动
 * （复习词表范式），状态栏右侧「n/7」位置指示（题号局刷同款）。
 * 全显档位（TINY 竖屏/LARGE）居中版式不变。
 */
#include "settings_ui.h"
#include "daily_plan.h"

#include "epd_driver.h"
#include "cjk_text.h"
#include "layout_profile.h"   /* 2026-08-25：档位判定（原 h<200 启发式误判竖屏） */
#include "es8311.h"      /* 2026-08-27 音量：setter 内即时 apply codec */
#include "debug_log.h"
#include "nvs.h"

#include <stdio.h>
#include <stdint.h>

static const char *TAG = "SET";

/* ---- 取值 API：惰性缓存（-1 未加载→NVS 首读；setter 即写即更） ---- */

static int8_t s_audio = -1;
static int8_t s_haptic = -1;
static int8_t s_font = -1;
static int8_t s_wordsize = -1;   /* 2026-08-27 P1b：单词字号偏好（大/中/小） */
static int8_t s_bold = -1;       /* 2026-08-27 P2：英文粗细（标准/加粗） */
static int8_t s_quizgrid = -1;   /* v1.5 T5.1：测验快答（2×2 方向直选） */
static int8_t s_rotmode = -1;    /* 2026-08-26 屏幕方向（0=默认/1=竖/2=横） */
static int8_t s_vol = -1;        /* 2026-08-27 音量（0~100 步进10，默认75） */

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
    return s_font > 2 ? 2 : s_font;   /* 三档钳位（脏值防御）：0/1/2 */
}

int settings_word_size(void)
{
    if (s_wordsize < 0) s_wordsize = load_u8("set_word", 0);
    return s_wordsize > 2 ? 2 : s_wordsize;   /* 0=大/1=中/2=小（脏值钳位） */
}

bool settings_bold_enabled(void)
{
    if (s_bold < 0) s_bold = load_u8("set_bold", 0);
    return s_bold != 0;   /* 默认关：常规表，视觉零变化铁律 */
}

bool settings_quiz_grid(void)
{
    if (s_quizgrid < 0) s_quizgrid = load_u8("set_quizgrid", 0);
    return s_quizgrid != 0;
}

int settings_rotation_mode(void)
{
    if (s_rotmode < 0) s_rotmode = load_u8("set_rot", 0);
    return (s_rotmode >= 0 && s_rotmode <= 2) ? s_rotmode : 0;
}

int settings_volume(void)
{
    if (s_vol < 0) s_vol = load_u8("set_vol", 75);   /* 75=0dB 历史听感 */
    return s_vol;
}

void settings_volume_set(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_vol = (int8_t)v;
    save_u8("set_vol", (uint8_t)v);
    es8311_set_volume(v);   /* 即时生效（codec 未起播时 -1 无害：dac_start 回写） */
}

/* ---- 覆盖层 UI（menu_ui 范式镜像） ---- */

#define SET_ITEMS 10

static bool s_active = false;
static int  s_sel = 0;          /* 当前编辑行 */
static int  s_win = 0;          /* 滚动窗口首行（全显档位无意义恒 0） */

/* 行标签（2026-08-25 TINY 短版）：106/112px 正文宽下 5 字标签（80px）
 * 与右对齐值列（如「99 天」40px）压字，两枚 5 字标签缩 3 字；
 * 其余档位文案不变（menu_ui TINY 省略徽标同族降级先例）。
 * 「屏幕方向」保持 4 字全名（2026-08-27 真机勘误：曾缩「方向」
 * 两字致 wft0290 用户逐行找「屏幕方向」未识别出末行目标项——
 * 4 字 64px + 值列 32px = 96px，106/112px 正文宽均容纳，「测验
 * 快答」同长度先例未缩） */
static const char *set_label(int i)
{
    static const char *k_full[SET_ITEMS] = {
        "每日新词量", "发音", "震动", "字号", "单词大小", "粗细", "测验快答",
        "考试倒计时", "屏幕方向", "音量",
    };
    static const char *k_tiny[SET_ITEMS] = {
        "新词量", "发音", "震动", "字号", "单词大小", "粗细", "测验快答",
        "倒计时", "屏幕方向", "音量",
    };
    return layout_profile_get()->kind == LAYOUT_TINY ? k_tiny[i]
                                                      : k_full[i];
}

/* 屏幕方向值文案（与 set_rot 三态对应） */
static const char *rot_value_text(void)
{
    static const char *k[3] = { "默认", "竖屏", "横屏" };
    return k[settings_rotation_mode()];
}

/* 行取值文案（draw_page 滚动窗口统一消费） */
static void row_value(int i, char *buf, size_t bufsz)
{
    switch (i) {
    case 0: snprintf(buf, bufsz, "%d", daily_plan_goal()); break;
    case 1: snprintf(buf, bufsz, "%s", settings_audio_enabled() ? "开" : "关"); break;
    case 2: snprintf(buf, bufsz, "%s", settings_haptic_enabled() ? "开" : "关"); break;
    case 3: {   /* 字号三档（2026-08-27 P1a）：意图存档，渲染层按
        布局档位钳位（TINY/SMALL 屏特大渲染等价大字，见 main
        UI_MEAN_LEVEL）——set_rot 意图相对面板表达同哲学 */
        static const char *k[3] = { "标准", "大字", "特大" };
        snprintf(buf, bufsz, "%s", k[settings_font_mode()]);
        break;
    }
    case 4: {   /* 单词大小（2026-08-27 P1b）：fit 起步档 4/3/2（大/中/小） */
        static const char *k[3] = { "大", "中", "小" };
        snprintf(buf, bufsz, "%s", k[settings_word_size()]);
        break;
    }
    case 5: snprintf(buf, bufsz, "%s", settings_bold_enabled() ? "加粗" : "标准"); break;
    case 6: snprintf(buf, bufsz, "%s", settings_quiz_grid() ? "开" : "关"); break;
    case 7: {   /* 考试倒计时（v1.5 T5.5）：以「N 天」表达，存绝对 ymd */
        int d = exam_days_left();
        if (d > 0) snprintf(buf, bufsz, "%d 天", d);
        else       snprintf(buf, bufsz, "%s", "关");
        break;
    }
    default: snprintf(buf, bufsz, "%s", rot_value_text()); break;
    case 9:                              /* 音量：0=静音文案，其余数字档 */
        if (settings_volume() == 0) snprintf(buf, bufsz, "%s", "静音");
        else                         snprintf(buf, bufsz, "%d", settings_volume());
        break;
    }
}

static void draw_row(int row, const char *label, const char *value,
                     int top, int lh, int font_lvl)
{
    int w = epd_gfx_width();
    int margin = w > 200 ? 16 : 8;
    int y = top + (row - s_win) * lh;    /* 滚动窗口：行号-窗口首行 */

    if (row == s_sel)                       /* 反选：黑底白字整行 */
        epd_gfx_fill_rect(margin, y, w - 2 * margin, lh - 4, EPD_GFX_BLACK);
    int fg = (row == s_sel) ? EPD_GFX_WHITE : EPD_GFX_BLACK;
    /* cjk_text_draw 的 y 是字形 cell 顶（cjk_text.h 坐标语义，非
     * FreeSans 基线），行内垂直居中 = 顶 + (行高-cell 高)/2。原基线
     * 式 lh*3/4 把字压低 14/17px：选中行字溢出反选框（白字落框外
     * 白底不可见）、邻行墨迹与框重叠——2.9" 真机 2026-08-28 反馈 */
    int base = y + (lh - (16 + font_lvl * 4)) / 2;

    cjk_text_draw(margin + 4, base, font_lvl, label, fg);

    int vw = cjk_text_width(font_lvl, value);
    cjk_text_draw(w - margin - 4 - vw, base, font_lvl, value, fg);
}

static void draw_page(bool full)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    int margin = w > 200 ? 16 : 8;
    /* 档位派生（2026-08-25 修正）：原「h<200」启发式按屏高判小屏，
     * TINY 竖屏（高 250/296）误走大屏分支——opm021eb 第 6 行负 top
     * 出屏、wft0290 末行压提示栏；改 LAYOUT 枚举判定（menu_ui MU_TINY
     * 同款先例），SMALL/MID/LARGE 输出与原启发式精确一致（视觉零变化） */
    bool compact = layout_profile_get()->kind <= LAYOUT_SMALL;  /* TINY/SMALL */
    int status_h = compact ? 24 : 32;
    int font_lvl = compact ? 0 : 1;         /* 行文字 16/20px */
    int lh = compact ? 28 : 44;             /* 复习词表同款行高 */
    /* 滚动窗口（2026-08-27）：可用高度装不下全部行时（MID 240 高横屏
     * 7×44=308>184、SMALL/TINY 横屏同溢），改选中行驱动滚动；全显
     * 档位保持居中版式（视觉零变化铁律）。2026-08-26 曾直接改 7 行
     * 导致 MID/SMALL 横屏末行出屏——「屏幕方向」不可见真机实测 */
    int avail = h - status_h - (compact ? 18 : 24);
    int vis = avail / lh;
    if (vis < 1) vis = 1;
    bool scroll = vis < SET_ITEMS;
    int top = scroll ? status_h + 4
                     : status_h + (avail - SET_ITEMS * lh) / 2;

    if (full) {
        epd_gfx_fill_screen(EPD_GFX_WHITE);
        cjk_text_draw(margin, (status_h - 16) / 2, 0, "设置", EPD_GFX_BLACK);
        epd_gfx_draw_hline(margin, status_h, w - 2 * margin, EPD_GFX_BLACK);
    } else {
        epd_gfx_fill_rect(0, status_h, w, h - status_h, EPD_GFX_WHITE);
    }

    int first = 0, last = SET_ITEMS;
    if (scroll) {
        /* 窗口对齐选中行（上下越界滚动），再钳全局范围 */
        if (s_sel < s_win) s_win = s_sel;
        if (s_sel >= s_win + vis) s_win = s_sel - vis + 1;
        if (s_win > SET_ITEMS - vis) s_win = SET_ITEMS - vis;
        if (s_win < 0) s_win = 0;
        first = s_win;
        last = s_win + vis;

        /* 位置指示「n/7」：状态栏右侧（题号同款）；清除旧值区域防
         * 数字宽度变化残留，局刷窗口含状态栏（题号局刷同策略） */
        char pos[10];
        snprintf(pos, sizeof(pos), "%d/%d", s_sel + 1, SET_ITEMS);
        int pw = cjk_text_width(0, pos);
        epd_gfx_fill_rect(w - margin - pw - 8, 4, pw + 8, status_h - 8,
                          EPD_GFX_WHITE);
        cjk_text_draw(w - margin - pw, (status_h - 16) / 2, 0, pos,
                      EPD_GFX_BLACK);
    }

    for (int i = first; i < last; i++) {
        char val[16];
        row_value(i, val, sizeof(val));
        draw_row(i, set_label(i), val, top, lh, font_lvl);
    }

    /* 底部提示栏：TINY 档省略（menu_ui MU_HINT_H=0 同款——提示行 16px
     * 全长约 216px 超 106/112px 正文宽）；SMALL 及以上照画，滚动档位
     * 末行底 status_h+4+vis*lh ≤ h-(18|24)+4 与提示行 (h-16|h-18)
     * 无重叠（SMALL/MID 实算验证） */
    if (full && layout_profile_get()->kind != LAYOUT_TINY)
        cjk_text_draw(margin, h - (compact ? 16 : 18), 0,
                      "上/下 选择 · 中 切换 · SET 退出", EPD_GFX_BLACK);

    if (full)
        epd_gfx_flush();
    else if (scroll)   /* 含状态栏（位置指示随选中行变化，题号局刷同策略） */
        epd_gfx_flush_window(0, 0, w, h);
    else                                    /* 局刷内容区（菜单翻页同策略） */
        epd_gfx_flush_window(0, status_h, w, h - status_h);
}

void settings_ui_enter(void)
{
    if (s_active) return;                   /* 幂等 */
    s_active = true;
    s_sel = 0;
    s_win = 0;                              /* 滚动窗口复位（进入即顶行） */
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
extern void ui_apply_rotation(void);   /* 屏幕方向生效链（下方 case 6） */

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
        case 3:                             /* 字号档循环 标准→大字→特大（P1a） */
            s_font = (settings_font_mode() + 1) % 3;
            save_u8("set_font", (uint8_t)s_font);
            break;
        case 4:                             /* 单词大小循环 大→中→小（P1b） */
            s_wordsize = (settings_word_size() + 1) % 3;
            save_u8("set_word", (uint8_t)s_wordsize);
            break;
        case 5:                             /* 粗细（2026-08-27 P2）：仅
            英文/ASCII 路径（FreeSans 表切换，单词/状态栏/菜单；CJK
            点阵不受影响）；即时 apply（音量 setter 同范式），退出
            设置的全刷链负责重绘 */
            s_bold = settings_bold_enabled() ? 0 : 1;
            save_u8("set_bold", (uint8_t)s_bold);
            epd_gfx_set_bold(s_bold != 0);
            break;
        case 6:                             /* 测验快答（v1.5 T5.1） */
            s_quizgrid = settings_quiz_grid() ? 0 : 1;
            save_u8("set_quizgrid", (uint8_t)s_quizgrid);
            break;
        case 7: {                           /* 考试倒计时（v1.5 T5.5）：
            关→1→…→99→关 循环；存目标日 ymd（自治钟重启不失真）；
            时钟未同步时 exam_set_days 拒写，回显保持「关」 */
            int d = exam_days_left() + 1;
            if (d > 99) d = 0;
            exam_set_days(d);
            break;
        }
        case 8: {                           /* 屏幕方向（2026-08-26）：
            默认→竖屏→横屏 循环，即改即存即生效（ui_apply_rotation
            重建画布/失效布局，见 main.cpp）；几何已变，本页须全刷
            重排而非局刷内容区，故不走下方统一 draw_page(false) */
            s_rotmode = (settings_rotation_mode() + 1) % 3;
            save_u8("set_rot", (uint8_t)s_rotmode);
            ui_apply_rotation();
            draw_page(true);
            LOG_I("settings: rotation -> %d", s_rotmode);
            return;
        }
        case 9:                             /* 音量（2026-08-27）：
            +10 循环 0→10→…→100→0（与「每日新词量 +5 循环」同范式；
            连续微调走菜单「音量」页上/下键） */
            settings_volume_set(settings_volume() >= 100 ? 0
                                                         : settings_volume() + 10);
            break;
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
