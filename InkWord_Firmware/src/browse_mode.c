/**
 * @file browse_mode.c
 * @brief 教材目录浏览三级视图实现（设计见头注；数据源 catalog_index）
 *
 * 列表范式抄 menu_ui（2026-08 真机验证）：反选高亮 + 右滚动条 +
 * 标题页码；刷新策略同款——进入/换级全刷（计数归零），光标移动清
 * 列表区单遍局刷（阈值 br_partial_threshold 同 menu_ui 公式：
 * desc 基准×菜单系数 400，经 refresh_scheduler 升级全刷保养；
 * 三色屏无局刷由 epd_gfx_flush_window_passes 内部自动降级，零特判）。
 *
 * 词表行 = text + 释义首行（'\n' 截断）混排单行，超宽由
 * cjk_text_draw_wrap 的 max_lines=1 截断（ASCII 按词断不拆词）。
 */
#include "browse_mode.h"
#include "page_router.h" /* T1.4 试点：g_browse_page 栈式接入（渲染/按键经栈顶） */
#include "catalog_index.h"
#include "study_mode_machine.h"
#include "word_parser.h"
#include "debug_log.h"
#include "epd_driver.h"
#include "cjk_text.h"
#include "layout_profile.h"
#include "refresh_scheduler.h"
#include "haptic.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "BROWSE";

/* ---- 几何派生（T1.5 档位参数表：布局值查 profile，与 menu_ui
 * MU_* 同源同值；列表可用高/可见数等派生式局部保留） ---- */
#define BR_TITLE_H  (layout_profile_get()->status_h)        /* 标题栏高（T1.5） */
#define BR_ITEM_H   (layout_profile_get()->item_h)          /* 列表行高（T1.5） */
#define BR_FONT_H   (layout_profile_get()->font_px_main)    /* 主内容字号 px（T1.5） */
#define BR_FONT_LVL (layout_profile_get()->font_lvl_main)   /* 主内容 cjk level（T1.5） */
#define BR_FONT_ASC (layout_profile_get()->ascii_size_main) /* ASCII FreeSans size（T1.5） */
#define BR_HINT_H   (layout_profile_get()->hint_h)          /* 底部提示栏高（TINY 省略；T1.5） */
#define BR_LIST_TOP (BR_TITLE_H + 2)
#define BR_LIST_H   (epd_gfx_height() - BR_TITLE_H - BR_HINT_H)
#define BR_VISIBLE  (BR_LIST_H / BR_ITEM_H)
#define BR_MARGIN_X (layout_profile_get()->margin_x)        /* 左右留白（T1.5） */
#define BR_ITEM_W   (epd_gfx_width() - 2 * BR_MARGIN_X)
#define BR_SB_W     4

/* 局刷保养阈值（menu_ui mu_partial_threshold 同款公式，2026-09-04
 * 由硬编码 10 公式化：desc 基准 × profile.partial_menu 400 / 100） */
static int br_partial_threshold(void)
{
    const epd_panel_desc_t *pd = epd_panel_desc();
    int base = (pd && pd->partial_count_full_refresh > 0)
             ? pd->partial_count_full_refresh : 8;
    return base * layout_profile_get()->partial_menu / 100;
}

/* ---- 模块状态（静态零初始化；reset 清态） ---- */
static browse_page_t s_page   = BROWSE_GRADE;
static int s_g     = 0;   /* 年级选中 */
static int s_u     = 0;   /* 单元选中 */
static int s_w_sel = 0;   /* 词表选中（单元内下标） */
static int s_w_off = 0;   /* 词表滚动偏移 */

/* ---- 小工具 ---- */

/* 当前页列表总数（年级数/单元数/词数） */
static int page_total(void)
{
    if (s_page == BROWSE_GRADE) return catalog_grade_count();
    if (s_page == BROWSE_UNIT)  return catalog_unit_count(s_g);
    int n = 0;
    catalog_unit_entries(s_g, s_u, &n);
    return n;
}

/* 滚动窗口跟随光标（下界 0 上界钳满屏） */
static void follow_offset(int sel, int *off)
{
    int total = page_total();
    if (sel < *off) *off = sel;
    if (total > BR_VISIBLE && sel >= *off + BR_VISIBLE)
        *off = sel - BR_VISIBLE + 1;
    if (*off > total - BR_VISIBLE) *off = total > BR_VISIBLE ? total - BR_VISIBLE : 0;
}

/* 反选行底（黑底白字，同 menu_ui draw_item） */
static void fill_sel(int y)
{
    epd_gfx_fill_rect(BR_MARGIN_X, y, BR_ITEM_W - BR_SB_W - 4,
                      BR_ITEM_H - 4, EPD_GFX_BLACK);
}

/* ---- 绘制 ---- */

/* 标题栏（页题 + 右侧序号 + 分隔线，抄 menu_ui draw_title） */
static void draw_title(const char *title, int sel_1based, int total)
{
    epd_gfx_fill_rect(0, 0, epd_gfx_width(), BR_TITLE_H, EPD_GFX_WHITE);
    cjk_text_draw(BR_MARGIN_X, (BR_TITLE_H - BR_FONT_H) / 2,
                  BR_FONT_LVL, title, EPD_GFX_BLACK);
    if (total > 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%d", sel_1based, total);
        int tw, th;
        epd_gfx_text_bounds(buf, BR_FONT_ASC, &tw, &th);
        epd_gfx_draw_text(epd_gfx_width() - BR_MARGIN_X - tw,
                          BR_TITLE_H - 10, buf, EPD_GFX_BLACK, BR_FONT_ASC);
    }
    epd_gfx_draw_hline(BR_MARGIN_X, BR_TITLE_H,
                       epd_gfx_width() - 2 * BR_MARGIN_X, EPD_GFX_BLACK);
}

/* ASCII 右徽标（词条数；FreeSans，同 menu_ui draw_badge 窄路径） */
static void draw_count_badge(int y, int count, bool sel)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", count);
    int tw, th;
    epd_gfx_text_bounds(buf, BR_FONT_ASC, &tw, &th);
    epd_gfx_draw_text(BR_MARGIN_X + BR_ITEM_W - BR_SB_W - 10 - tw,
                      y + BR_ITEM_H * 3 / 4, buf,
                      sel ? EPD_GFX_WHITE : EPD_GFX_BLACK, BR_FONT_ASC);
}

/* 滚动条（超一屏时；抄 menu_ui draw_scrollbar 滑块范式） */
static void draw_scrollbar(int total, int off)
{
    if (total <= BR_VISIBLE) return;
    int x  = epd_gfx_width() - BR_MARGIN_X;
    int h  = BR_VISIBLE * BR_ITEM_H;
    int y0 = BR_LIST_TOP;
    epd_gfx_draw_rect(x, y0, BR_SB_W, h, EPD_GFX_BLACK);
    int thumb_h = h * BR_VISIBLE / total;
    if (thumb_h < BR_SB_W * 2) thumb_h = BR_SB_W * 2;
    int thumb_y = y0 + (h - thumb_h) * off / (total - BR_VISIBLE);
    epd_gfx_fill_rect(x, thumb_y, BR_SB_W, thumb_h, EPD_GFX_BLACK);
}

static void draw_hint(void)
{
    if (BR_HINT_H == 0) return;
    epd_gfx_draw_hline(BR_MARGIN_X, epd_gfx_height() - BR_HINT_H,
                       epd_gfx_width() - 2 * BR_MARGIN_X, EPD_GFX_BLACK);
    cjk_text_draw(BR_MARGIN_X,
                  epd_gfx_height() - BR_HINT_H + (BR_HINT_H - 16) / 2,
                  0, "上/下 选择  中 进入  RST 返回", EPD_GFX_BLACK);
}

static void draw_flush(void)
{
    epd_power_on();
    epd_gfx_flush();
    epd_power_off();
    /* 进入/换级真全刷等价清残影，计数归零（menu_ui 同款） */
    refresh_notify_full_done();
}

/* 列表主体（按页分派；空索引居中空态） */
static void draw_body(void)
{
    int total = page_total();

    if (total <= 0) {   /* 空目录（未建索引/空词库防御，理论不到） */
        cjk_text_draw(BR_MARGIN_X,
                      BR_LIST_TOP + (BR_LIST_H - BR_FONT_H) / 2,
                      BR_FONT_LVL, "目录为空", EPD_GFX_BLACK);
        return;
    }

    if (s_page == BROWSE_WORDS) {
        int n = 0;
        const uint16_t *e = catalog_unit_entries(s_g, s_u, &n);
        for (int i = 0; i < BR_VISIBLE; i++) {
            int idx = s_w_off + i;
            if (idx >= n) break;
            const WordEntry *w = word_parser_get(e[idx]);
            if (!w) break;
            int y = BR_LIST_TOP + i * BR_ITEM_H;
            bool sel = (idx == s_w_sel);
            if (sel) fill_sel(y);

            /* text + 释义首行混排单行截断（'\n' 前段；无释义仅词） */
            char line[WORD_TEXT_MAX + 8];
            snprintf(line, sizeof(line), "%s", w->text);
            if (w->meaning[0]) {
                char first[96];
                snprintf(first, sizeof(first), "%.95s", w->meaning); /* 精度=有意截断（-Wformat-truncation） */
                char *nl = strchr(first, '\n');
                if (nl) *nl = '\0';
                size_t used = strlen(line);
                snprintf(line + used, sizeof(line) - used, "  %.60s", first);
            }
            cjk_text_draw_wrap(BR_MARGIN_X + 4,
                               y + (BR_ITEM_H - BR_FONT_H) / 2,
                               BR_ITEM_W - BR_SB_W - 12, BR_FONT_LVL,
                               BR_ITEM_H, 1, line,
                               sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
        }
        draw_scrollbar(n, s_w_off);
        return;
    }

    /* 年级/单元列表：桶名 + 右侧词条数徽标 */
    int sel = s_page == BROWSE_GRADE ? s_g : s_u;
    int off = sel;   /* 列表短（≤16），窗口跟随同式 */
    if (total > BR_VISIBLE) {
        if (off > total - BR_VISIBLE) off = total - BR_VISIBLE;
    } else {
        off = 0;
    }
    for (int i = 0; i < BR_VISIBLE; i++) {
        int idx = off + i;
        if (idx >= total) break;
        const catalog_bucket_t *b = s_page == BROWSE_GRADE
                                        ? catalog_grade(idx)
                                        : catalog_unit(s_g, idx);
        if (!b) break;
        int y = BR_LIST_TOP + i * BR_ITEM_H;
        bool is_sel = (idx == sel);
        if (is_sel) fill_sel(y);
        cjk_text_draw(BR_MARGIN_X + 4, y + (BR_ITEM_H - BR_FONT_H) / 2,
                      BR_FONT_LVL, b->name,
                      is_sel ? EPD_GFX_WHITE : EPD_GFX_BLACK);
        draw_count_badge(y, b->len, is_sel);
    }
    draw_scrollbar(total, off);
}

/* 页面绘制入口（partial=true 局刷 + 阈值升级全刷，抄 menu_ui） */
static void draw_page(bool partial)
{
    if (partial && !refresh_gfx_before_partial_n(br_partial_threshold())) {
        epd_gfx_fill_rect(0, BR_TITLE_H, epd_gfx_width(),
                          epd_gfx_height() - BR_TITLE_H, EPD_GFX_WHITE);
        draw_body();
        epd_gfx_flush_window_passes(0, BR_TITLE_H, epd_gfx_width(),
                                    epd_gfx_height() - BR_TITLE_H, 1);
        return;
    }
    epd_gfx_fill_screen(EPD_GFX_WHITE);

    int total = page_total();
    int sel_1 = 1;
    const char *title = "教材目录";
    if (s_page == BROWSE_UNIT) {
        title = catalog_grade_name(s_g);
        sel_1 = s_u + 1;
    } else if (s_page == BROWSE_WORDS) {
        const catalog_bucket_t *b = catalog_unit(s_g, s_u);
        title = b ? b->name : "词表";
        sel_1 = s_w_sel + 1;
    } else {
        sel_1 = total > 0 ? s_g + 1 : 0;
    }
    draw_title(title, sel_1, total);

    draw_body();
    draw_hint();
    draw_flush();
}

/* ---- 公共 API ---- */

void browse_mode_reset(void)
{
    s_page = BROWSE_GRADE;
    s_g = s_u = s_w_sel = s_w_off = 0;
}

void browse_mode_render(void)
{
    draw_page(false);
}

/* T1.4 页面协议（试点）：enter=browse_mode_reset（清态）；exit 无
 * （退出编排 study_mode_exit_browse 在各按键路径显式调用）；render
 * =browse_mode_render（render_top 首帧/重绘入口） */
static bool browse_page_on_button(nav_key_t id, button_event_t event)
{
    browse_mode_on_button(id, event);
    return true;   /* 栈顶总消费；退出编排模块内自管 */
}

const page_t g_browse_page = { "browse", browse_mode_render,
                               browse_page_on_button,
                               browse_mode_reset, NULL, true };

/* 选词跳转：seek 已切 FLASH 并渲染词卡（本视图自然终结，无需 exit） */
static void confirm_word(void)
{
    int n = 0;
    const uint16_t *e = catalog_unit_entries(s_g, s_u, &n);
    if (!e || s_w_sel >= n) {
        haptic_event(HAPTIC_ERROR);
        return;
    }
    haptic_event(HAPTIC_MODE);
    page_router_pop_if(&g_browse_page);   /* T1.4：seek 自带渲染词卡，
     * 出栈须先行（栈顶残留 browse 时后续 render_top 会误重绘旧视图） */
    study_mode_seek(e[s_w_sel]);
    LOG_I("browse seek word #%d", e[s_w_sel]);
}

void browse_mode_on_button(nav_key_t id, button_event_t event)
{
    /* RST 长按：任意层级直接退出回闪卡（游标恢复进视图前位置） */
    if (id == NAV_RST && event == BUTTON_EVENT_LONG_PRESS) {
        study_mode_exit_browse();
        page_router_exit(&g_browse_page);   /* P2：pop+render 两连收敛 */
        return;
    }
    if (event != BUTTON_EVENT_SHORT_PRESS) return;   /* 其余长按忽略 */

    int total = page_total();
    if (total <= 0) {   /* 空目录：任意短按退出（防御） */
        if (id == NAV_RST || id == NAV_SET || id == NAV_CENTER) {
            study_mode_exit_browse();
            page_router_exit(&g_browse_page);
        }
        return;
    }

    /* SET 短按 = RST 短按（返回上一级，menu_ui 二级页先例） */
    if (id == NAV_SET) id = NAV_RST;

    switch (s_page) {
    case BROWSE_GRADE:
        switch (id) {
        case NAV_UP:
            s_g = (s_g + total - 1) % total;
            draw_page(true);
            break;
        case NAV_DOWN:
            s_g = (s_g + 1) % total;
            draw_page(true);
            break;
        case NAV_CENTER:      /* 进入单元列表（清单元选中） */
            s_u = 0;
            s_page = BROWSE_UNIT;
            draw_page(false);
            break;
        case NAV_RST:         /* 顶层退出视图 */
            study_mode_exit_browse();
            page_router_exit(&g_browse_page);
            break;
        default:
            break;
        }
        break;

    case BROWSE_UNIT:
        switch (id) {
        case NAV_UP:
            s_u = (s_u + catalog_unit_count(s_g) - 1) % catalog_unit_count(s_g);
            draw_page(true);
            break;
        case NAV_DOWN:
            s_u = (s_u + 1) % catalog_unit_count(s_g);
            draw_page(true);
            break;
        case NAV_CENTER:      /* 进入词表（清词表选中/滚动） */
            s_w_sel = 0;
            s_w_off = 0;
            s_page = BROWSE_WORDS;
            draw_page(false);
            break;
        case NAV_RST:         /* 返回年级列表 */
            s_page = BROWSE_GRADE;
            draw_page(false);
            break;
        default:
            break;
        }
        break;

    case BROWSE_WORDS:
        switch (id) {
        case NAV_UP:
            s_w_sel = (s_w_sel + total - 1) % total;
            follow_offset(s_w_sel, &s_w_off);
            draw_page(true);
            break;
        case NAV_DOWN:
            s_w_sel = (s_w_sel + 1) % total;
            follow_offset(s_w_sel, &s_w_off);
            draw_page(true);
            break;
        case NAV_CENTER:
            confirm_word();
            break;
        case NAV_RST:         /* 返回单元列表 */
            s_page = BROWSE_UNIT;
            draw_page(false);
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}
