/**
 * @file reader_menu.c
 * @brief 阅读器菜单覆盖层实现（2026-09-05 阅读器增强阶段六）
 *
 * 阅读模式下中键短按进入，主菜单 7 项 + 内嵌子视图（目录/书签/生词列表）。
 * 子视图在 reader_menu 内部状态机切换（不额外 push 覆盖层），
 * RST 任意层级退出回阅读页。
 *
 * 子视图状态机：
 *   VIEW_MAIN → 选中"目录"/"书签"/"查看生词" → VIEW_TOC/VIEW_BM/VIEW_WORD
 *   VIEW_TOC/VIEW_BM/VIEW_WORD → RST → VIEW_MAIN
 *   任意视图 → RST → 退出菜单（return false 请求 pop）
 *
 * 刷新策略：进入全刷；光标移动局刷列表区。
 */
#include "reader_menu.h"
#include "reader_engine.h"
#include "chapter_index.h"
#include "bookmark_mgr.h"
#include "reader_word_link.h"
#include "reader_search.h"
#include "book_shelf.h"
#include "study_mode_machine.h"
#include "page_router.h"
#include "epd_driver.h"
#include "cjk_text.h"
#include "cjk_font.h"
#include "layout_profile.h"
#include "refresh_scheduler.h"
#include "haptic.h"
#include "debug_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "RMENU";

/* ---- 子视图枚举 ---- */
typedef enum {
    VIEW_MAIN = 0,
    VIEW_TOC,        /* 目录列表 */
    VIEW_BM,         /* 书签列表 */
    VIEW_WORD,       /* 生词列表 */
    VIEW_STATS,      /* 阅读统计 */
} sub_view_t;

/* ---- 主菜单项 ---- */
#define MAIN_ITEMS 7
static const char *main_labels[MAIN_ITEMS] = {
    "我的书架",
    "目录",
    "书签",
    "搜索",
    "查看生词",
    "阅读设置",
    "阅读统计",
};

/* ---- 几何派生（menu_ui 同范式） ---- */
#define RM_STATUS_H     (layout_profile_get()->status_h)
#define RM_ITEM_H       (layout_profile_get()->item_h)
#define RM_MARGIN_X     (layout_profile_get()->margin_x)
#define RM_HINT_H       (layout_profile_get()->hint_h)
#define RM_FONT_LVL     (layout_profile_get()->font_lvl_main)
#define RM_LIST_TOP     (RM_STATUS_H + 2)
#define RM_LIST_H       (epd_gfx_height() - RM_STATUS_H - RM_HINT_H)
#define RM_VISIBLE      (RM_LIST_H / RM_ITEM_H)

/* ---- 静态状态 ---- */
static sub_view_t s_view = VIEW_MAIN;
static int s_main_sel = 0;     /* 主菜单选中 */
static int s_sub_sel  = 0;     /* 子视图选中 */
static int s_sub_off  = 0;     /* 子视图滚动偏移 */

/* 子视图数据缓存 */
static word_link_t s_words[WORD_LINK_MAX];
static int s_word_count = 0;

/* ---- 渲染辅助 ---- */

static void draw_title(const char *title)
{
    int w = epd_gfx_width();
    epd_gfx_fill_rect(0, 0, w, RM_STATUS_H, EPD_GFX_BLACK);
    epd_gfx_draw_text(RM_MARGIN_X, RM_STATUS_H - 6, title, EPD_GFX_WHITE, 2);
}

static void draw_hint(const char *hint)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    if (RM_HINT_H <= 0) return;
    int hy = h - RM_HINT_H;
    epd_gfx_fill_rect(0, hy, w, RM_HINT_H, EPD_GFX_WHITE);
    epd_gfx_draw_hline(0, hy, w, EPD_GFX_BLACK);
    epd_gfx_draw_text(RM_MARGIN_X, hy + RM_HINT_H - 4, hint, EPD_GFX_BLACK, 1);
}

static void draw_list_item(int idx, int vis_idx, const char *label,
                           const char *badge, bool selected)
{
    int w = epd_gfx_width();
    int y = RM_LIST_TOP + vis_idx * RM_ITEM_H;

    if (selected)
        epd_gfx_fill_rect(RM_MARGIN_X, y, w - 2 * RM_MARGIN_X,
                          RM_ITEM_H - 2, EPD_GFX_BLACK);
    uint16_t fg = selected ? EPD_GFX_WHITE : EPD_GFX_BLACK;

    /* 标签（CJK 点阵） */
    cjk_text_draw(RM_MARGIN_X + 4,
                  y + (RM_ITEM_H - cjk_glyph_cell_size(RM_FONT_LVL)) / 2 - 2,
                  RM_FONT_LVL, label, fg);

    /* 右侧徽标 */
    if (badge && badge[0]) {
        int bw, bh;
        epd_gfx_text_bounds(badge, 1, &bw, &bh);
        epd_gfx_draw_text(w - RM_MARGIN_X - bw - 4,
                          y + RM_ITEM_H / 2, badge, fg, 1);
    }
}

/* ---- 主菜单渲染 ---- */
static void draw_main(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    epd_gfx_fill_rect(0, RM_STATUS_H, w, h - RM_STATUS_H, EPD_GFX_WHITE);
    draw_title("[阅读菜单]");

    int vis = RM_VISIBLE;
    if (vis < 1) vis = 1;
    if (vis > MAIN_ITEMS) vis = MAIN_ITEMS;

    for (int i = 0; i < vis; i++) {
        /* 徽标 */
        char badge[32] = "";
        switch (i) {
        case 1: /* 目录 */
            snprintf(badge, sizeof(badge), "%d章", chapter_index_count());
            break;
        case 2: /* 书签 */
            snprintf(badge, sizeof(badge), "%d枚", bookmark_count());
            break;
        case 4: /* 生词 */
            snprintf(badge, sizeof(badge), "%d词", s_word_count);
            break;
        default:
            break;
        }
        draw_list_item(i, i, main_labels[i], badge, i == s_main_sel);
    }
    draw_hint("UP/DN:sel MID:go SET:+/-bm RST:back");
    epd_gfx_flush_window(0, RM_STATUS_H, w, h - RM_STATUS_H);
}

/* ---- 目录子视图渲染 ---- */
static void draw_toc(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    epd_gfx_fill_rect(0, RM_STATUS_H, w, h - RM_STATUS_H, EPD_GFX_WHITE);
    draw_title("[目录]");

    int total = chapter_index_count();
    int vis = RM_VISIBLE;
    if (vis < 1) vis = 1;

    if (total == 0) {
        epd_gfx_draw_text(RM_MARGIN_X + 4, RM_LIST_TOP + 20,
                          "no chapters detected", EPD_GFX_BLACK, 1);
    } else {
        for (int i = 0; i < vis && s_sub_off + i < total; i++) {
            int idx = s_sub_off + i;
            const chapter_entry_t *ch = chapter_index_at(idx);
            if (!ch) break;
            char badge[16];
            snprintf(badge, sizeof(badge), "p%d", ch->start_page + 1);
            draw_list_item(idx, i, ch->title, badge, idx == s_sub_sel);
        }
        /* 滚动条 */
        if (total > vis) {
            int sb_h = (vis * RM_LIST_H) / total;
            if (sb_h < 8) sb_h = 8;
            int sb_y = (s_sub_off * (RM_LIST_H - sb_h)) / (total - vis);
            epd_gfx_fill_rect(w - 3, RM_LIST_TOP + sb_y, 2, sb_h, EPD_GFX_BLACK);
        }
    }
    draw_hint("UP/DN:sel MID:jump RST:back");
    epd_gfx_flush_window(0, RM_STATUS_H, w, h - RM_STATUS_H);
}

/* ---- 书签子视图渲染 ---- */
static void draw_bm(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    epd_gfx_fill_rect(0, RM_STATUS_H, w, h - RM_STATUS_H, EPD_GFX_WHITE);
    draw_title("[书签]");

    int total = bookmark_count();
    int vis = RM_VISIBLE;
    if (vis < 1) vis = 1;

    if (total == 0) {
        epd_gfx_draw_text(RM_MARGIN_X + 4, RM_LIST_TOP + 20,
                          "no bookmarks (SET to add)", EPD_GFX_BLACK, 1);
    } else {
        for (int i = 0; i < vis && s_sub_off + i < total; i++) {
            int idx = s_sub_off + i;
            const bookmark_t *bm = bookmark_at(idx);
            if (!bm) break;
            char label[48];
            if (bm->note[0])
                snprintf(label, sizeof(label), "p%d %s", bm->page + 1, bm->note);
            else
                snprintf(label, sizeof(label), "Page %d", bm->page + 1);
            draw_list_item(idx, i, label, NULL, idx == s_sub_sel);
        }
        if (total > vis) {
            int sb_h = (vis * RM_LIST_H) / total;
            if (sb_h < 8) sb_h = 8;
            int sb_y = (s_sub_off * (RM_LIST_H - sb_h)) / (total - vis);
            epd_gfx_fill_rect(w - 3, RM_LIST_TOP + sb_y, 2, sb_h, EPD_GFX_BLACK);
        }
    }
    draw_hint("UP/DN:sel MID:jump RST:back");
    epd_gfx_flush_window(0, RM_STATUS_H, w, h - RM_STATUS_H);
}

/* ---- 生词子视图渲染 ---- */
static void draw_word(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    epd_gfx_fill_rect(0, RM_STATUS_H, w, h - RM_STATUS_H, EPD_GFX_WHITE);
    draw_title("[查看生词]");

    int vis = RM_VISIBLE;
    if (vis < 1) vis = 1;

    if (s_word_count == 0) {
        epd_gfx_draw_text(RM_MARGIN_X + 4, RM_LIST_TOP + 20,
                          "no words on this page", EPD_GFX_BLACK, 1);
    } else {
        for (int i = 0; i < vis && s_sub_off + i < s_word_count; i++) {
            int idx = s_sub_off + i;
            const word_link_t *wl = &s_words[idx];
            /* 标签：单词 */
            char label[64];
            snprintf(label, sizeof(label), "%s", wl->text);
            /* 徽标：释义截断 */
            char badge[32];
            snprintf(badge, sizeof(badge), "%.28s", wl->meaning);
            draw_list_item(idx, i, label, badge, idx == s_sub_sel);
        }
        if (s_word_count > vis) {
            int sb_h = (vis * RM_LIST_H) / s_word_count;
            if (sb_h < 8) sb_h = 8;
            int sb_y = (s_sub_off * (RM_LIST_H - sb_h)) / (s_word_count - vis);
            epd_gfx_fill_rect(w - 3, RM_LIST_TOP + sb_y, 2, sb_h, EPD_GFX_BLACK);
        }
    }
    draw_hint("UP/DN:sel MID:jump RST:back");
    epd_gfx_flush_window(0, RM_STATUS_H, w, h - RM_STATUS_H);
}

/* ---- 阅读统计渲染 ---- */
static void draw_stats(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    epd_gfx_fill_rect(0, RM_STATUS_H, w, h - RM_STATUS_H, EPD_GFX_WHITE);
    draw_title("[阅读统计]");

    int y = RM_LIST_TOP + 8;
    int pages = reader_page_count();
    int cur = study_mode_seq_pos();
    if (cur < 0) cur = 0;
    int pct = pages > 0 ? ((cur + 1) * 100) / pages : 0;

    char buf[64];
    snprintf(buf, sizeof(buf), "Pages: %d / %d (%d%%)", cur + 1, pages, pct);
    epd_gfx_draw_text(RM_MARGIN_X + 4, y, buf, EPD_GFX_BLACK, 1);
    y += 24;

    snprintf(buf, sizeof(buf), "Font level: %d (%dpx)",
             reader_font_level(),
             cjk_glyph_cell_size(reader_font_level()));
    epd_gfx_draw_text(RM_MARGIN_X + 4, y, buf, EPD_GFX_BLACK, 1);
    y += 24;

    snprintf(buf, sizeof(buf), "Chapters: %d", chapter_index_count());
    epd_gfx_draw_text(RM_MARGIN_X + 4, y, buf, EPD_GFX_BLACK, 1);
    y += 24;

    snprintf(buf, sizeof(buf), "Bookmarks: %d", bookmark_count());
    epd_gfx_draw_text(RM_MARGIN_X + 4, y, buf, EPD_GFX_BLACK, 1);

    draw_hint("RST:back");
    epd_gfx_flush_window(0, RM_STATUS_H, w, h - RM_STATUS_H);
}

/* ---- 统一渲染 ---- */
static void draw_current(void)
{
    switch (s_view) {
    case VIEW_MAIN: draw_main(); break;
    case VIEW_TOC:  draw_toc();  break;
    case VIEW_BM:   draw_bm();   break;
    case VIEW_WORD: draw_word(); break;
    case VIEW_STATS: draw_stats(); break;
    }
}

/* ---- 子视图滚动辅助 ---- */
static int sub_total(void)
{
    switch (s_view) {
    case VIEW_TOC:  return chapter_index_count();
    case VIEW_BM:   return bookmark_count();
    case VIEW_WORD: return s_word_count;
    default:        return 0;
    }
}

static void sub_scroll_up(void)
{
    int total = sub_total();
    if (total <= 0) return;
    if (s_sub_sel > 0) {
        s_sub_sel--;
        int vis = RM_VISIBLE;
        if (vis < 1) vis = 1;
        if (s_sub_sel < s_sub_off) s_sub_off = s_sub_sel;
        draw_current();
    }
}

static void sub_scroll_down(void)
{
    int total = sub_total();
    if (total <= 0) return;
    if (s_sub_sel < total - 1) {
        s_sub_sel++;
        int vis = RM_VISIBLE;
        if (vis < 1) vis = 1;
        if (s_sub_sel >= s_sub_off + vis) s_sub_off = s_sub_sel - vis + 1;
        draw_current();
    }
}

/* ---- 页面协议回调 ---- */

static void rm_enter(void)
{
    s_view = VIEW_MAIN;
    s_main_sel = 0;
    s_sub_sel = 0;
    s_sub_off = 0;

    /* 预扫描当前页生词（供"查看生词"项使用） */
    int cur_page = study_mode_seq_pos();
    if (cur_page < 0) cur_page = 0;
    s_word_count = reader_word_link_scan(cur_page, s_words, WORD_LINK_MAX);

    draw_current();
    refresh_notify_full_done();
    LOG_I("reader menu entered");
}

static void rm_render(void)
{
    draw_current();
}

static bool rm_on_button(nav_key_t id, button_event_t event)
{
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;

    int vis = RM_VISIBLE;
    if (vis < 1) vis = 1;

    switch (s_view) {
    case VIEW_MAIN: {
        switch (id) {
        case NAV_UP:
            if (s_main_sel > 0) { s_main_sel--; draw_main(); }
            return true;
        case NAV_DOWN:
            if (s_main_sel < MAIN_ITEMS - 1) { s_main_sel++; draw_main(); }
            return true;
        case NAV_CENTER: {
            haptic_event(HAPTIC_MODE);
            switch (s_main_sel) {
            case 0: /* 我的书架 → 退出菜单，push book_shelf */
                return false;  /* pop reader_menu */
            case 1: /* 目录 */
                s_view = VIEW_TOC;
                s_sub_sel = chapter_index_find_by_page(study_mode_seq_pos());
                s_sub_off = 0;
                draw_toc();
                return true;
            case 2: /* 书签 */
                s_view = VIEW_BM;
                s_sub_sel = 0; s_sub_off = 0;
                draw_bm();
                return true;
            case 3: /* 搜索（简化：暂提示需从主菜单进入） */
                /* 搜索需要软键盘输入，当前设备输入受限，暂不实现内嵌搜索 */
                return true;
            case 4: /* 查看生词 */
                s_view = VIEW_WORD;
                s_sub_sel = 0; s_sub_off = 0;
                draw_word();
                return true;
            case 5: /* 阅读设置：字号放大 */
                study_mode_reader_font_step(+1);
                return false;  /* 退出菜单，base_render 接管 */
            case 6: /* 阅读统计 */
                s_view = VIEW_STATS;
                draw_stats();
                return true;
            default:
                return true;
            }
        }
        case NAV_SET:
            /* SET 短按 = 当前页书签切换（便捷操作） */
            {
                int cur_page = study_mode_seq_pos();
                if (cur_page < 0) cur_page = 0;
                if (bookmark_exists(cur_page)) {
                    bookmark_remove(cur_page);
                    LOG_I("bookmark removed page %d", cur_page);
                } else {
                    bookmark_add(cur_page, NULL);
                    LOG_I("bookmark added page %d", cur_page);
                }
                haptic_event(HAPTIC_REVIEW);
                draw_main();  /* 重绘主菜单（书签徽标更新） */
            }
            return true;
        case NAV_RST:
            return false;  /* 退出菜单 */
        default:
            return true;
        }
    }

    case VIEW_TOC: {
        switch (id) {
        case NAV_UP:   sub_scroll_up(); return true;
        case NAV_DOWN: sub_scroll_down(); return true;
        case NAV_CENTER: {
            /* 跳转到选中章节 */
            int page = chapter_index_jump_to(s_sub_sel);
            LOG_I("chapter jump to page %d", page);
            study_mode_reader_goto_page(page);
            return false;  /* 退出菜单 */
        }
        case NAV_RST:
            s_view = VIEW_MAIN;
            draw_main();
            return true;
        default:
            return true;
        }
    }

    case VIEW_BM: {
        switch (id) {
        case NAV_UP:   sub_scroll_up(); return true;
        case NAV_DOWN: sub_scroll_down(); return true;
        case NAV_CENTER: {
            /* 跳转到选中书签 */
            int page = bookmark_jump(s_sub_sel);
            LOG_I("bookmark jump to page %d", page);
            study_mode_reader_goto_page(page);
            return false;  /* 退出菜单 */
        }
        case NAV_RST:
            s_view = VIEW_MAIN;
            draw_main();
            return true;
        default:
            return true;
        }
    }

    case VIEW_WORD: {
        switch (id) {
        case NAV_UP:   sub_scroll_up(); return true;
        case NAV_DOWN: sub_scroll_down(); return true;
        case NAV_CENTER: {
            /* 跳转到选中生词闪卡 */
            if (s_sub_sel < s_word_count) {
                reader_word_link_jump(s_words[s_sub_sel].word_index);
                return false;  /* 退出菜单 */
            }
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

    case VIEW_STATS: {
        /* 统计页：任意键回主菜单 */
        if (id == NAV_RST) {
            s_view = VIEW_MAIN;
            draw_main();
        }
        return true;
    }

    default:
        return true;
    }
}

static void rm_exit(void)
{
    LOG_I("reader menu exited");
    /* 如果主菜单选了"我的书架"，退出后 push book_shelf */
    if (s_view == VIEW_MAIN && s_main_sel == 0) {
        page_router_push(&g_book_shelf_page);
    }
}

/* ---- 页面协议实例 ---- */

const page_t g_reader_menu_page = {
    "reader_menu",
    rm_render,
    rm_on_button,
    rm_enter,
    rm_exit,
    true    /* owns_display */
};
