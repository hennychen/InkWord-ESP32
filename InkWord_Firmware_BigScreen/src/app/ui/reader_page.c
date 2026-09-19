/**
 * @file reader_page.c
 * @brief 大屏阅读页实现（覆盖层页；页游标单点持有）
 *
 * 渲染：状态栏（书名·章节 32px + 页码/书签星标 + 分隔线）+ 正文
 * （engine，默认 40px 五档字号）+ 底部中文键位提示栏（80px，
 * 对齐三区范式）。
 *
 * 刷新分档（2026-09-17 UI 重设计）：进页/换书/字号/间距切换 =
 * GC16 全刷；翻页/书签切换 = 全屏窗口 DU 局刷（~0.5s 级；原
 * 「待 waveform_scanq 标定」注释已过时——词卡 run88 局刷已验证，
 * gfx K=8 自动全屏重置驱白灰染兕底）。
 *
 * 惰性初始化：首次 enter 调 reader_engine_init（建页表耗时与书长
 * 成正比，不进启动链保首帧速度）；失败保持未就绪 → 占位页。
 * 进度：每次翻页/跳转后调 reader_engine_save_progress（NVS rd_*）。
 */
#include "reader_page.h"
#include "reader_menu.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "bookmark_mgr.h"
#include "chapter_index.h"
#include "cjk_font.h"
#include "cjk_text.h"
#include "debug_log.h"
#include "epd_gfx.h"
#include "reader_engine.h"
#include "storage_manager.h"   /* STORAGE_BOOK_NAME_MAX */

static const char *TAG = "RPAGE";

/* ---- 状态栏/提示栏排版参数（正文区几何见 reader_engine.c） ---- */
#define RP_TITLE_LVL    3     /* 书名·章节 cjk 级（32px，2026-09-17 升） */
#define RP_PAGE_FSZ     3     /* 页码 FreeSans 档（18pt ≈ 25px 高） */
#define RP_HINT_LVL     1     /* 中文键位提示 cjk 级（20px） */

/* ---- 模块状态 ---- */
static bool s_inited = false;   /* engine 惰性初始化标记 */
static int  s_page = -1;        /* 页游标（-1 = 未恢复进度） */
static bool s_render_fast = false;  /* 尾部刷新档（翻页/书签置 DU 局刷） */

/* ---- 渲染 ---- */

static void draw_status_bar(void)
{
    int w = epd_gfx_width();

    /* 左：书名 · 当前章节（章节缺失只显书名） */
    char title[STORAGE_BOOK_NAME_MAX + 56];
    const chapter_entry_t *ch =
        chapter_index_at(chapter_index_find_by_page(s_page));
    if (ch && ch->title[0])
        snprintf(title, sizeof(title), "%s · %s",
                 reader_engine_book_title(), ch->title);
    else
        snprintf(title, sizeof(title), "%s", reader_engine_book_title());
    int th = (RD_STATUS_H - cjk_glyph_cell_size(RP_TITLE_LVL)) / 2;
    cjk_text_draw(RD_MARGIN_X, th, RP_TITLE_LVL, title, EPD_GFX_BLACK);

    /* 右：页码 N/M（当前页有书签加 * 前缀；右对齐 + 实测宽） */
    char page[24];
    snprintf(page, sizeof(page), "%s%d/%d",
             bookmark_exists(s_page) ? "*" : "",
             s_page + 1, reader_page_count());
    int pw = 0, ph = 0;
    epd_gfx_text_bounds(page, RP_PAGE_FSZ, &pw, &ph);
    epd_gfx_draw_text(w - RD_MARGIN_X - pw,
                      RD_STATUS_H / 2 + 8, page, EPD_GFX_BLACK, RP_PAGE_FSZ);

    /* 状态栏底部分隔线 */
    epd_gfx_draw_hline(0, RD_STATUS_H - 2, w, EPD_GFX_BLACK);
}

static void draw_hint_bar(void)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();
    int hy = h - RD_BOTTOM_BAR;

    epd_gfx_draw_hline(0, hy, w, EPD_GFX_BLACK);
    static const char hint[] =
        "左右 翻页 · 中键 菜单 · SET 书签 · 长按SET 字号 · 长按RST 退出";
    cjk_text_draw((w - cjk_text_width(RP_HINT_LVL, hint)) / 2,
                  hy + (RD_BOTTOM_BAR - cjk_glyph_cell_size(RP_HINT_LVL)) / 2,
                  RP_HINT_LVL, hint, EPD_GFX_BLACK);
}

static void rp_render(void)
{
    epd_gfx_fill_screen(EPD_GFX_WHITE);

    if (!reader_ready()) {
        reader_render_placeholder();   /* 引擎自绘整页提示 */
        epd_gfx_flush();
        s_render_fast = false;
        return;
    }

    draw_status_bar();
    reader_render_body(s_page);
    draw_hint_bar();
    if (s_render_fast) {
        epd_gfx_flush_window(0, 0, epd_gfx_width(), epd_gfx_height());
    } else {
        epd_gfx_flush();
    }
    s_render_fast = false;   /* 单次有效：进页/字号/跳转回全刷 */

    reader_engine_save_progress(s_page);   /* 每页渲染即存（小屏先例） */
}

/* 翻页/书签路径：置 DU 局刷档后渲染 */
static void rp_render_fast(void)
{
    s_render_fast = true;
    rp_render();
}

/* ---- 页面协议回调 ---- */

static void rp_enter(void)
{
    if (!s_inited) {
        s_inited = true;
        int64_t t0 = esp_timer_get_time();
        int r = reader_engine_init();
        LOG_I("reader engine init: %d (%lld ms)", r,
              (long long)((esp_timer_get_time() - t0) / 1000));
    }
    if (reader_ready() && s_page < 0) {
        int p = reader_progress_page();
        s_page = (p >= 0) ? p : 0;
    }
    if (s_page < 0) s_page = 0;
    rp_render();
}

static bool rp_on_button(nav_key_t id, button_event_t event)
{
    /* ---- 长按 ---- */
    if (event == BUTTON_EVENT_LONG_PRESS) {
        switch (id) {
        case NAV_CENTER:
        case NAV_RST:
            return false;               /* 请求退出（pop 回菜单） */
        case NAV_SET:
            reader_page_font_step(+1);  /* 字号放大步进（循环） */
            rp_render();
            return true;
        default:
            return true;
        }
    }
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;

    /* ---- 短按：占位页仅退出（无内容可操作） ---- */
    if (!reader_ready()) {
        if (id == NAV_RST || id == NAV_CENTER) return false;
        return true;
    }

    switch (id) {
    case NAV_LEFT:
    case NAV_UP:
        if (s_page > 0) { s_page--; rp_render_fast(); }
        return true;
    case NAV_RIGHT:
    case NAV_DOWN:
        if (s_page < reader_page_count() - 1) { s_page++; rp_render_fast(); }
        return true;
    case NAV_CENTER:
        page_router_push(&g_reader_menu_page);   /* 阅读菜单入栈其上 */
        return true;
    case NAV_SET:
        /* 书签添加/移除切换（小屏 SET 语义同） */
        if (bookmark_exists(s_page)) {
            bookmark_remove(s_page);
            LOG_I("bookmark removed page %d", s_page);
        } else {
            bookmark_add(s_page, NULL);
            LOG_I("bookmark added page %d", s_page);
        }
        rp_render_fast();   /* 状态栏星标更新（仅小面积差异，DU 快档） */
        return true;
    case NAV_RST:
        if (s_page != 0) { s_page = 0; rp_render_fast(); }
        return true;
    default:
        return true;
    }
}

const page_t g_reader_page = {
    .name = "reader",
    .render = rp_render,
    .on_button = rp_on_button,
    .enter = rp_enter,
    .exit = NULL,          /* 进度每页已存，退出无清态需求 */
    .owns_display = true,
};

/* ---- reader_menu 协作接口 ---- */

int reader_page_current(void)
{
    return reader_ready() ? s_page : -1;
}

void reader_page_goto(int page)
{
    if (!reader_ready()) return;
    if (page < 0) page = 0;
    if (page > reader_page_count() - 1) page = reader_page_count() - 1;
    s_page = page;
}

void reader_page_font_step(int dir)
{
    s_page = reader_font_step(dir, s_page);
}

void reader_page_spacing_step(int dir)
{
    s_page = reader_spacing_step(dir, s_page);
}

int reader_page_load_book(const char *path)
{
    int r = (path && path[0]) ? reader_engine_load_book(path)
                              : reader_engine_load_demo();
    if (r != 0) {
        LOG_W("load book failed: %s (err=%d)", path ? path : "(demo)", r);
        return -1;  /* 加载失败保持原书（书架不切空书） */
    }
    int p = reader_progress_page();   /* 新书自身进度（无记录回首页） */
    s_page = (p >= 0) ? p : 0;
    return 0;
}
