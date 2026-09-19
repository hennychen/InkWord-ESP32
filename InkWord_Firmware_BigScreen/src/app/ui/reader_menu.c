/**
 * @file reader_menu.c
 * @brief 大屏阅读菜单实现（小屏 reader_menu 裁剪移植）
 *
 * 子视图状态机：VIEW_MAIN → 书架/目录/书签/阅读设置 → RST 回
 * VIEW_MAIN；VIEW_MAIN 下 RST 退出菜单（return false → pop →
 * reader_page 重渲染）。跳转/选书只改游标不渲染（reader_page_goto
 * 等），pop 后 render_top 统一恢复——避免双重全刷。
 *
 * 几何（menu_ui 同范式）：标题栏 80px 黑底（cjk 白字 + 右侧书名）、
 * 列表项 100px（标签 32px 反选高亮、徽标 20px cjk 支持中文）、
 * 提示栏 60px 黑底（FreeSans 24px）、左右边距 80px、9 项可见 +
 * 右缘滚动条。大屏恒全刷。
 */
#include "reader_menu.h"

#include <stdio.h>
#include <string.h>

#include "bookmark_mgr.h"
#include "chapter_index.h"
#include "cjk_font.h"
#include "cjk_text.h"
#include "debug_log.h"
#include "epd_gfx.h"
#include "reader_engine.h"
#include "reader_page.h"
#include "storage_manager.h"

static const char *TAG = "RMENU";

/* ---- 子视图 ---- */
typedef enum {
    VIEW_MAIN = 0,
    VIEW_SHELF,     /* 书架（内置演示书 + SPIFFS 书库） */
    VIEW_TOC,       /* 目录 */
    VIEW_BM,        /* 书签 */
    VIEW_SET,       /* 阅读设置（字号/行距） */
} sub_view_t;

/* ---- 几何（menu_ui 范式本地副本；XLARGE 1920x1080） ---- */
#define RM_TITLE_H     80
#define RM_ITEM_H      100
#define RM_HINT_H      60
#define RM_MARGIN_X    80
#define RM_ITEM_W      (1920 - 2 * RM_MARGIN_X)
#define RM_LIST_TOP    (RM_TITLE_H + 10)
#define RM_LIST_H      (1080 - RM_TITLE_H - RM_HINT_H)
#define RM_VISIBLE     (RM_LIST_H / RM_ITEM_H)   /* 9 项可见 */

/* ---- 排版级别/字号 ---- */
#define RM_TITLE_LVL   2     /* 标题栏（24px 白字） */
#define RM_ITEM_LVL    3     /* 列表标签（32px，menu_ui 同级） */
#define RM_BADGE_LVL   1     /* 徽标（20px；cjk 绘制支持中文） */
#define RM_EMPTY_LVL   2     /* 空态提示（24px） */
#define RM_HINT_FSZ    2     /* 提示栏 FreeSans（24px） */

/* ---- 主菜单 ---- */
#define MAIN_ITEMS 4
static const char *main_labels[MAIN_ITEMS] = {
    "书架",
    "目录",
    "书签",
    "阅读设置",
};

/* ---- 模块状态 ---- */
static sub_view_t s_view = VIEW_MAIN;
static int s_main_sel = 0;    /* 主菜单选中 */
static int s_sub_sel  = 0;    /* 子视图选中 */
static int s_sub_off  = 0;    /* 子视图滚动偏移 */

/* 书架缓存（进入 VIEW_SHELF 时枚举刷新） */
static char s_shelf[STORAGE_SHELF_MAX][STORAGE_BOOK_NAME_MAX];
static int  s_shelf_n = 0;    /* SPIFFS 书数（不含演示书项） */

/* ---- 通用绘制 ---- */

static void draw_title(const char *title)
{
    int w = epd_gfx_width();
    epd_gfx_fill_rect(0, 0, w, RM_TITLE_H, EPD_GFX_BLACK);
    cjk_text_draw(RM_MARGIN_X,
                  (RM_TITLE_H - cjk_glyph_cell_size(RM_TITLE_LVL)) / 2,
                  RM_TITLE_LVL, title, EPD_GFX_WHITE);

    /* 右侧当前书名小徽标（知道自己在哪本书里） */
    const char *bk = reader_engine_book_title();
    if (bk && bk[0]) {
        int bw = cjk_text_width(RM_BADGE_LVL, bk);
        cjk_text_draw(w - RM_MARGIN_X - bw,
                      (RM_TITLE_H - cjk_glyph_cell_size(RM_BADGE_LVL)) / 2,
                      RM_BADGE_LVL, bk, EPD_GFX_WHITE);
    }
}

static void draw_hint(const char *hint)
{
    int w = epd_gfx_width();
    int hy = epd_gfx_height() - RM_HINT_H;
    epd_gfx_fill_rect(0, hy, w, RM_HINT_H, EPD_GFX_BLACK);
    epd_gfx_draw_text(RM_MARGIN_X, hy + 36, hint, EPD_GFX_WHITE, RM_HINT_FSZ);
}

/* 列表项（vis_idx=可视序号；selected=反选高亮；徽标右对齐） */
static void draw_list_item(int vis_idx, const char *label, const char *badge,
                           bool selected)
{
    int w = epd_gfx_width();
    int y = RM_LIST_TOP + vis_idx * RM_ITEM_H;

    if (selected)
        epd_gfx_fill_rect(RM_MARGIN_X, y, RM_ITEM_W, RM_ITEM_H - 4,
                          EPD_GFX_BLACK);
    uint16_t fg = selected ? EPD_GFX_WHITE : EPD_GFX_BLACK;

    cjk_text_draw(RM_MARGIN_X + 24,
                  y + (RM_ITEM_H - cjk_glyph_cell_size(RM_ITEM_LVL)) / 2,
                  RM_ITEM_LVL, label, fg);

    if (badge && badge[0]) {
        int bw = cjk_text_width(RM_BADGE_LVL, badge);
        cjk_text_draw(w - RM_MARGIN_X - 24 - bw,
                      y + (RM_ITEM_H - cjk_glyph_cell_size(RM_BADGE_LVL)) / 2,
                      RM_BADGE_LVL, badge, fg);
    }
}

/* 右缘滚动条（total > 可见数时） */
static void draw_scrollbar(int total)
{
    int w = epd_gfx_width();
    int vis = RM_VISIBLE;
    if (vis < 1 || total <= vis) return;
    int sb_h = vis * RM_LIST_H / total;
    if (sb_h < 8) sb_h = 8;
    int sb_y = s_sub_off * (RM_LIST_H - sb_h) / (total - vis);
    epd_gfx_fill_rect(w - 8, RM_LIST_TOP + sb_y, 3, sb_h, EPD_GFX_BLACK);
}

static void draw_empty(const char *msg)
{
    cjk_text_draw(RM_MARGIN_X + 24, RM_LIST_TOP + 40, RM_EMPTY_LVL, msg,
                  EPD_GFX_BLACK);
}

/* ---- 主菜单 ---- */

static void draw_main(void)
{
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("阅读菜单");

    for (int i = 0; i < MAIN_ITEMS; i++) {
        char badge[32];
        badge[0] = '\0';
        switch (i) {
        case 0:   /* 书架：演示书 + 书库总本数 */
            snprintf(badge, sizeof(badge), "%d 本", s_shelf_n + 1);
            break;
        case 1:   /* 目录 */
            snprintf(badge, sizeof(badge), "%d 章", chapter_index_count());
            break;
        case 2:   /* 书签 */
            snprintf(badge, sizeof(badge), "%d 枚", bookmark_count());
            break;
        case 3: { /* 阅读设置：当前字号·行距即时回显 */
            int lh = reader_line_spacing();
            snprintf(badge, sizeof(badge), "%dpx %d.%dx",
                     cjk_glyph_cell_size(reader_font_level()),
                     lh / 10, lh % 10);
            break;
        }
        default:
            break;
        }
        draw_list_item(i, main_labels[i], badge, i == s_main_sel);
    }
    draw_hint("UP/DN:Sel  MID:Ok  SET:+BM  RST:Back");
    epd_gfx_flush();
}

/* ---- 书架子视图 ---- */

static const char *shelf_name(int idx)
{
    return idx == 0 ? "内置演示书" : s_shelf[idx - 1];
}

/* 是否当前书（文件书按去扩展名书名比较——engine 口径同源） */
static bool shelf_is_current(int idx)
{
    const char *cur = reader_engine_book_title();
    if (!cur || !cur[0]) return false;
    if (idx == 0) return strcmp(cur, "内置演示书") == 0;

    char nm[STORAGE_BOOK_NAME_MAX];
    snprintf(nm, sizeof(nm), "%s", s_shelf[idx - 1]);
    char *dot = strrchr(nm, '.');
    if (dot && dot > nm) *dot = '\0';
    return strcmp(cur, nm) == 0;
}

static void draw_shelf(void)
{
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("书架");

    int total = 1 + s_shelf_n;
    for (int i = 0; i < RM_VISIBLE && s_sub_off + i < total; i++) {
        int idx = s_sub_off + i;
        draw_list_item(i, shelf_name(idx),
                       shelf_is_current(idx) ? "*" : NULL, idx == s_sub_sel);
    }
    draw_scrollbar(total);
    draw_hint("UP/DN:Sel  MID:Open  RST:Back");
    epd_gfx_flush();
}

/* ---- 目录子视图 ---- */

static void draw_toc(void)
{
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("目录");

    int total = chapter_index_count();
    if (total <= 0) {
        draw_empty("未检测到章节");
    } else {
        for (int i = 0; i < RM_VISIBLE && s_sub_off + i < total; i++) {
            int idx = s_sub_off + i;
            const chapter_entry_t *ch = chapter_index_at(idx);
            if (!ch) break;
            char badge[16];
            snprintf(badge, sizeof(badge), "P%d", ch->start_page + 1);
            draw_list_item(i, ch->title, badge, idx == s_sub_sel);
        }
        draw_scrollbar(total);
    }
    draw_hint("UP/DN:Sel  MID:Jump  RST:Back");
    epd_gfx_flush();
}

/* ---- 书签子视图 ---- */

static void draw_bm(void)
{
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("书签");

    int total = bookmark_count();
    if (total <= 0) {
        draw_empty("暂无书签（阅读页按 SET 添加）");
    } else {
        for (int i = 0; i < RM_VISIBLE && s_sub_off + i < total; i++) {
            int idx = s_sub_off + i;
            const bookmark_t *bm = bookmark_at(idx);
            if (!bm) break;
            char label[64];
            if (bm->note[0])
                snprintf(label, sizeof(label), "第%d页 %s",
                         bm->page + 1, bm->note);
            else
                snprintf(label, sizeof(label), "第%d页", bm->page + 1);
            draw_list_item(i, label, NULL, idx == s_sub_sel);
        }
        draw_scrollbar(total);
    }
    draw_hint("UP/DN:Sel  MID:Jump  SET:Del  RST:Back");
    epd_gfx_flush();
}

/* ---- 阅读设置子视图（字号/行距两行，步进即时回显） ---- */

#define SET_ROWS 2

static void draw_set(void)
{
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    draw_title("阅读设置");

    static const char *labels[SET_ROWS] = { "字号", "行距" };
    char badges[SET_ROWS][24];
    snprintf(badges[0], sizeof(badges[0]), "%dpx",
             cjk_glyph_cell_size(reader_font_level()));
    int lh = reader_line_spacing();
    snprintf(badges[1], sizeof(badges[1]), "%d.%d 倍", lh / 10, lh % 10);

    for (int i = 0; i < SET_ROWS; i++)
        draw_list_item(i, labels[i], badges[i], i == s_sub_sel);

    draw_hint("UP/DN:Row  L/R:-/+  MID:+  RST:Back");
    epd_gfx_flush();
}

/* ---- 统一分发 ---- */

static void draw_current(void)
{
    switch (s_view) {
    case VIEW_MAIN:  draw_main();  break;
    case VIEW_SHELF: draw_shelf(); break;
    case VIEW_TOC:   draw_toc();   break;
    case VIEW_BM:    draw_bm();    break;
    case VIEW_SET:   draw_set();   break;
    }
}

static int sub_total(void)
{
    switch (s_view) {
    case VIEW_SHELF: return 1 + s_shelf_n;
    case VIEW_TOC:   return chapter_index_count();
    case VIEW_BM:    return bookmark_count();
    case VIEW_SET:   return SET_ROWS;
    default:         return 0;
    }
}

/* 上下滚动（钳位不循环；选中项保持可见） */
static void sub_scroll(int delta)
{
    int total = sub_total();
    if (total <= 0) return;
    int ns = s_sub_sel + delta;
    if (ns < 0 || ns >= total) return;
    s_sub_sel = ns;
    int vis = RM_VISIBLE;
    if (vis < 1) vis = 1;
    if (s_sub_sel < s_sub_off) s_sub_off = s_sub_sel;
    if (s_sub_sel >= s_sub_off + vis) s_sub_off = s_sub_sel - vis + 1;
    draw_current();
}

/* 进子视图（状态复位 + 各视图定位 + 首帧） */
static void enter_view(sub_view_t v)
{
    s_view = v;
    s_sub_sel = 0;
    s_sub_off = 0;

    if (v == VIEW_SHELF) {
        s_shelf_n = storage_books_list(s_shelf, STORAGE_SHELF_MAX);
        int total = 1 + s_shelf_n;
        for (int i = 0; i < total; i++)       /* 光标落当前书 */
            if (shelf_is_current(i)) { s_sub_sel = i; break; }
    } else if (v == VIEW_TOC) {
        s_sub_sel = chapter_index_find_by_page(reader_page_current());
        if (s_sub_sel < 0) s_sub_sel = 0;
    }

    int vis = RM_VISIBLE;                     /* 滚动到选中可见 */
    if (vis < 1) vis = 1;
    if (s_sub_sel >= vis) s_sub_off = s_sub_sel - vis + 1;
    draw_current();
}

/* ---- 各视图按键 ---- */

static bool on_main_button(nav_key_t id)
{
    switch (id) {
    case NAV_UP:
        if (s_main_sel > 0) { s_main_sel--; draw_main(); }
        return true;
    case NAV_DOWN:
        if (s_main_sel < MAIN_ITEMS - 1) { s_main_sel++; draw_main(); }
        return true;
    case NAV_CENTER:
        switch (s_main_sel) {
        case 0: enter_view(VIEW_SHELF); break;
        case 1: enter_view(VIEW_TOC);   break;
        case 2: enter_view(VIEW_BM);    break;
        case 3: enter_view(VIEW_SET);   break;
        default: break;
        }
        return true;
    case NAV_SET: {
        /* 当前页书签切换（便捷操作，小屏同款；徽标即时刷新） */
        int cur = reader_page_current();
        if (cur >= 0) {
            if (bookmark_exists(cur)) {
                bookmark_remove(cur);
                LOG_I("bookmark removed page %d", cur);
            } else {
                bookmark_add(cur, NULL);
                LOG_I("bookmark added page %d", cur);
            }
        }
        draw_main();
        return true;
    }
    case NAV_RST:
        return false;   /* 退出菜单（pop → 阅读页重渲染） */
    default:
        return true;
    }
}

static bool on_shelf_button(nav_key_t id)
{
    switch (id) {
    case NAV_UP:   sub_scroll(-1); return true;
    case NAV_DOWN: sub_scroll(+1); return true;
    case NAV_CENTER: {
        char path[STORAGE_BOOK_NAME_MAX + 24];
        if (s_sub_sel == 0)
            path[0] = '\0';    /* 内置演示书 */
        else
            storage_book_path(path, sizeof(path), s_shelf[s_sub_sel - 1]);
        if (reader_page_load_book(path) == 0)
            return false;      /* 选书成功 → 退出菜单看书 */
        draw_shelf();          /* 加载失败留在书架（原书未动） */
        return true;
    }
    case NAV_RST:
        s_view = VIEW_MAIN;
        draw_main();
        return true;
    default:
        return true;
    }
}

static bool on_toc_button(nav_key_t id)
{
    switch (id) {
    case NAV_UP:   sub_scroll(-1); return true;
    case NAV_DOWN: sub_scroll(+1); return true;
    case NAV_CENTER: {
        int page = chapter_index_jump_to(s_sub_sel);
        LOG_I("chapter jump to page %d", page);
        reader_page_goto(page);
        return false;          /* 退出菜单 → 阅读页跳转渲染 */
    }
    case NAV_RST:
        s_view = VIEW_MAIN;
        draw_main();
        return true;
    default:
        return true;
    }
}

static bool on_bm_button(nav_key_t id)
{
    switch (id) {
    case NAV_UP:   sub_scroll(-1); return true;
    case NAV_DOWN: sub_scroll(+1); return true;
    case NAV_CENTER: {
        const bookmark_t *bm = bookmark_at(s_sub_sel);
        if (!bm) return true;
        LOG_I("bookmark jump to page %d", bm->page);
        reader_page_goto(bm->page);
        return false;
    }
    case NAV_SET: {
        /* 删除选中书签（游标钳位到剩余表内） */
        const bookmark_t *bm = bookmark_at(s_sub_sel);
        if (bm) {
            bookmark_remove(bm->page);
            LOG_I("bookmark deleted page %d", bm->page);
            int total = bookmark_count();
            if (s_sub_sel >= total) s_sub_sel = total > 0 ? total - 1 : 0;
            if (s_sub_off > s_sub_sel) s_sub_off = s_sub_sel;
        }
        draw_bm();
        return true;
    }
    case NAV_RST:
        s_view = VIEW_MAIN;
        draw_main();
        return true;
    default:
        return true;
    }
}

static bool on_set_button(nav_key_t id)
{
    switch (id) {
    case NAV_UP:
        if (s_sub_sel > 0) { s_sub_sel = 0; draw_set(); }
        return true;
    case NAV_DOWN:
        if (s_sub_sel < SET_ROWS - 1) { s_sub_sel = SET_ROWS - 1; draw_set(); }
        return true;
    case NAV_LEFT:
    case NAV_RIGHT:
    case NAV_CENTER: {
        int dir = (id == NAV_LEFT) ? -1 : +1;
        if (s_sub_sel == 0)
            reader_page_font_step(dir);
        else
            reader_page_spacing_step(dir);
        /* 步进即持久化（rd_font/rd_lh 随进度落 NVS，断电不丢） */
        reader_engine_save_progress(reader_page_current());
        draw_set();
        return true;
    }
    case NAV_RST:
        s_view = VIEW_MAIN;
        draw_main();
        return true;
    default:
        return true;
    }
}

/* ---- 页面协议 ---- */

static void rm_enter(void)
{
    s_view = VIEW_MAIN;
    s_main_sel = 0;
    s_sub_sel = 0;
    s_sub_off = 0;
    /* 进菜单即枚举书库：主菜单“N 本”徽标首次进入就准确（书架
     * 子视图进入时会再刷新一次，容忍书库中途变化） */
    s_shelf_n = storage_books_list(s_shelf, STORAGE_SHELF_MAX);
    draw_current();
    LOG_I("reader menu entered");
}

static void rm_render(void)
{
    draw_current();
}

static bool rm_on_button(nav_key_t id, button_event_t event)
{
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;   /* 长按忽略防误触 */

    switch (s_view) {
    case VIEW_MAIN:  return on_main_button(id);
    case VIEW_SHELF: return on_shelf_button(id);
    case VIEW_TOC:   return on_toc_button(id);
    case VIEW_BM:    return on_bm_button(id);
    case VIEW_SET:   return on_set_button(id);
    default:         return true;
    }
}

const page_t g_reader_menu_page = {
    .name = "reader_menu",
    .render = rm_render,
    .on_button = rm_on_button,
    .enter = rm_enter,
    .exit = NULL,            /* 无跨页编排，纯状态机清态在 enter 复位 */
    .owns_display = true,
};
