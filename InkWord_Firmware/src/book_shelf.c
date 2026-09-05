/**
 * @file book_shelf.c
 * @brief 书架管理实现（2026-09-05 阅读器增强阶段一）
 *
 * 扫描 /sdcard/books/ 下 .txt/.md/.html 文件，提取书名（首行非空
 * 文本或文件名去扩展名），计算文件大小与阅读进度（NVS rd_* 恢复）。
 * 列表 UI 作为覆盖层经 page_router_push 入栈，上/下选择、中键加载、
 * RST 退出。几何按 layout_profile 档位派生（与 menu_ui 同范式）。
 *
 * 刷新策略：进入/加载全刷；光标移动局刷列表区（与 menu_ui 同口径）。
 * 并发模型：按键回调同步处理+绘制（主 loop 上下文，无并发）。
 */
#include "book_shelf.h"
#include "reader_engine.h"
#include "study_mode_machine.h"
#include "page_router.h"
#include "epd_driver.h"
#include "cjk_text.h"
#include "cjk_font.h"
#include "layout_profile.h"
#include "refresh_scheduler.h"
#include "debug_log.h"
#include "haptic.h"
#include "storage_manager.h"

#include "esp_heap_caps.h"
#include "nvs.h"
#include "settings_keys.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "BOOKSHELF";

/* ---- 容量与常量 ---- */
#define SHELF_MAX       32        /* 书架最多书籍数 */
#define BOOKS_DIR       "/sdcard/books"
#define TITLE_LINE_MAX  128       /* 首行标题提取缓冲 */

/* ---- 几何派生（menu_ui 同范式） ---- */
#define BS_STATUS_H     (layout_profile_get()->status_h)
#define BS_ITEM_H       (layout_profile_get()->item_h)
#define BS_MARGIN_X     (layout_profile_get()->margin_x)
#define BS_HINT_H       (layout_profile_get()->hint_h)
#define BS_FONT_LVL     (layout_profile_get()->font_lvl_main)
#define BS_LIST_TOP     (BS_STATUS_H + 2)
#define BS_LIST_H       (epd_gfx_height() - BS_STATUS_H - BS_HINT_H)
#define BS_VISIBLE      (BS_LIST_H / BS_ITEM_H)

/* ---- 静态状态 ---- */
static book_entry_t s_entries[SHELF_MAX];
static int          s_count = 0;
static int          s_sel   = 0;      /* 当前选中（0 基） */
static int          s_off   = 0;      /* 滚动偏移 */
static int          s_prev_mode = 0;  /* 进入书架前的模式（退出恢复） */

/* ---- 工具函数 ---- */

/* 文件名去扩展名 → 标题（截断到 title 缓冲） */
static void filename_to_title(const char *fname, char *title, size_t title_sz)
{
    snprintf(title, title_sz, "%s", fname);
    /* 去扩展名 */
    char *dot = strrchr(title, '.');
    if (dot) *dot = '\0';
    /* 截断过长文件名 */
    if (strlen(title) > title_sz - 1)
        title[title_sz - 1] = '\0';
}

/* 从文件首行提取标题（UTF-8；跳过空行，取首个非空行截断） */
static void extract_title(const char *path, char *title, size_t title_sz)
{
    FILE *f = fopen(path, "r");
    if (!f) { title[0] = '\0'; return; }
    char line[TITLE_LINE_MAX];
    title[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        /* 跳过空行 / 纯空白行 */
        const char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') continue;
        /* 跳过 Markdown 标题标记 */
        if (*p == '#') {
            p++;
            while (*p == ' ' || *p == '#') p++;
        }
        /* 去尾换行 */
        char *end = strchr(p, '\n');
        if (end) *end = '\0';
        end = strchr(p, '\r');
        if (end) *end = '\0';
        if (*p) {
            snprintf(title, title_sz, "%s", p);
            break;
        }
    }
    fclose(f);
}

/* 检查文件扩展名是否为支持的书籍格式 */
static bool is_book_file(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 4;
    return (strcasecmp(ext, ".txt") == 0 ||
            strcasecmp(ext, ".md") == 0 ||
            strcasecmp(ext, ".htm") == 0) ||
           (len >= 5 && strcasecmp(name + len - 5, ".html") == 0);
}

/* FNV-1a 书签名（与 reader_engine book_signature 同源） */
static uint32_t calc_signature(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[16];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return 0;
    /* 取文件长度 */
    struct stat st;
    uint32_t len = 0;
    if (stat(path, &st) == 0) len = (uint32_t)st.st_size;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)buf[i]; h *= 16777619u; }
    return h ^ len;
}

/* 查 NVS 恢复阅读进度百分比（按 signature 匹配） */
static void restore_progress(book_entry_t *e)
{
    e->has_progress = false;
    e->progress_pct = 0;
    if (e->signature == 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t sig = 0, page = 0;
    /* 尝试无 scope 默认键（reader_engine rd_page） */
    bool hit = nvs_get_u32(h, NVS_KEY_RD_SIG, &sig) == ESP_OK && sig == e->signature;
    if (hit) {
        /* 粗略进度：page / 估算总页数（文件字节数 / 每页约 600 字节 @20px） */
        if (nvs_get_u32(h, NVS_KEY_RD_PAGE, &page) == ESP_OK && e->file_size > 0) {
            uint32_t est_pages = e->file_size / 600;
            if (est_pages < 1) est_pages = 1;
            e->progress_pct = (uint8_t)((page * 100) / est_pages);
            if (e->progress_pct > 100) e->progress_pct = 100;
            e->has_progress = true;
        }
    }
    nvs_close(h);
}

/* ---- 公共 API ---- */

int book_shelf_scan(void)
{
    s_count = 0;
    s_sel = 0;
    s_off = 0;

    DIR *d = opendir(BOOKS_DIR);
    if (!d) {
        LOG_W("no %s directory", BOOKS_DIR);
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < SHELF_MAX) {
        if (!is_book_file(e->d_name)) continue;

        book_entry_t *be = &s_entries[s_count];
        memset(be, 0, sizeof(*be));
        snprintf(be->filename, sizeof(be->filename), "%s", e->d_name);
        be->file_size = 0;

        /* 构建完整路径 */
        char path[272];
        snprintf(path, sizeof(path), "%s/%s", BOOKS_DIR, e->d_name);

        /* 文件大小 */
        struct stat st;
        if (stat(path, &st) == 0) be->file_size = (uint32_t)st.st_size;

        /* 标题：首行提取，失败回退文件名 */
        extract_title(path, be->title, sizeof(be->title));
        if (!be->title[0])
            filename_to_title(e->d_name, be->title, sizeof(be->title));

        /* 签名 + 进度 */
        be->signature = calc_signature(path);
        restore_progress(be);

        s_count++;
    }
    closedir(d);
    LOG_I("shelf scanned: %d books", s_count);
    return s_count;
}

int book_shelf_count(void) { return s_count; }

const book_entry_t *book_shelf_at(int idx)
{
    return (idx >= 0 && idx < s_count) ? &s_entries[idx] : NULL;
}

int book_shelf_load(int idx)
{
    if (idx < 0 || idx >= s_count) return -1;
    char path[272];
    snprintf(path, sizeof(path), "%s/%s", BOOKS_DIR, s_entries[idx].filename);

    int r = reader_engine_load_book(path);
    if (r != 0) {
        LOG_E("failed to load book: %s (err=%d)", path, r);
        return -1;
    }
    /* 切换到阅读模式（study_mode_set 含游标归零/进度恢复/NVS 持久化） */
    study_mode_set(MODE_READER);
    LOG_I("book loaded: %s", s_entries[idx].title);
    return 0;
}

/* ---- UI 渲染 ---- */

/* 文件大小人类可读格式 */
static void format_size(uint32_t bytes, char *buf, size_t sz)
{
    if (bytes < 1024)
        snprintf(buf, sz, "%dB", (int)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buf, sz, "%dK", (int)(bytes / 1024));
    else
        snprintf(buf, sz, "%.1fM", (double)bytes / (1024 * 1024));
}

static void draw_page(bool partial)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();

    /* 清内容区（状态栏以下） */
    epd_gfx_fill_rect(0, BS_STATUS_H, w, h - BS_STATUS_H, EPD_GFX_WHITE);

    /* 标题栏 */
    epd_gfx_fill_rect(0, 0, w, BS_STATUS_H, EPD_GFX_BLACK);
    epd_gfx_draw_text(BS_MARGIN_X, BS_STATUS_H - 6, "我的书架",
                      EPD_GFX_WHITE, 2);

    if (s_count == 0) {
        /* 空态提示 */
        static const char *msg = "SD卡books目录无书籍";
        int tw, th;
        epd_gfx_text_bounds(msg, 1, &tw, &th);
        epd_gfx_draw_text((w - tw) / 2, BS_STATUS_H + 40, msg,
                          EPD_GFX_BLACK, 1);
        static const char *hint = "请放入.txt/.md/.html文件";
        epd_gfx_text_bounds(hint, 1, &tw, &th);
        epd_gfx_draw_text((w - tw) / 2, BS_STATUS_H + 60, hint,
                          EPD_GFX_BLACK, 1);
    } else {
        /* 列表项 */
        int vis = BS_VISIBLE;
        if (vis < 1) vis = 1;
        for (int i = 0; i < vis && s_off + i < s_count; i++) {
            int idx = s_off + i;
            int y = BS_LIST_TOP + i * BS_ITEM_H;
            bool sel = (idx == s_sel);

            /* 选中高亮 */
            if (sel)
                epd_gfx_fill_rect(BS_MARGIN_X, y,
                                  w - 2 * BS_MARGIN_X, BS_ITEM_H - 2,
                                  EPD_GFX_BLACK);

            uint16_t fg = sel ? EPD_GFX_WHITE : EPD_GFX_BLACK;

            /* 书名（CJK 点阵） */
            const book_entry_t *be = &s_entries[idx];
            cjk_text_draw(BS_MARGIN_X + 4,
                          y + (BS_ITEM_H - cjk_glyph_cell_size(BS_FONT_LVL)) / 2 - 2,
                          BS_FONT_LVL, be->title, fg);

            /* 右侧徽标：文件大小 + 进度 */
            char badge[32];
            char szbuf[12];
            format_size(be->file_size, szbuf, sizeof(szbuf));
            if (be->has_progress && be->progress_pct > 0)
                snprintf(badge, sizeof(badge), "%s %d%%", szbuf, be->progress_pct);
            else
                snprintf(badge, sizeof(badge), "%s", szbuf);
            int bw, bh;
            epd_gfx_text_bounds(badge, 1, &bw, &bh);
            epd_gfx_draw_text(w - BS_MARGIN_X - bw - 4,
                              y + BS_ITEM_H / 2, badge, fg, 1);
        }

        /* 滚动条 */
        if (s_count > vis) {
            int sb_h = (vis * BS_LIST_H) / s_count;
            if (sb_h < 8) sb_h = 8;
            int sb_y = (s_off * (BS_LIST_H - sb_h)) / (s_count - vis);
            epd_gfx_fill_rect(w - 3, BS_LIST_TOP + sb_y, 2, sb_h,
                              EPD_GFX_BLACK);
        }
    }

    /* 底部提示栏 */
    if (BS_HINT_H > 0) {
        int hy = h - BS_HINT_H;
        epd_gfx_fill_rect(0, hy, w, BS_HINT_H, EPD_GFX_WHITE);
        epd_gfx_draw_hline(0, hy, w, EPD_GFX_BLACK);
        epd_gfx_draw_text(BS_MARGIN_X, hy + BS_HINT_H - 4,
                          "UP/DN:sel  MID:load  RST:back",
                          EPD_GFX_BLACK, 1);
    }

    if (partial)
        epd_gfx_flush_window(0, BS_STATUS_H, w, h - BS_STATUS_H);
    else
        epd_gfx_flush();
}

/* ---- 页面协议回调 ---- */

static void bs_enter(void)
{
    s_prev_mode = (int)study_mode_current();
    book_shelf_scan();
    draw_page(false);   /* 进入全刷 */
    refresh_notify_full_done();
}

static void bs_render(void)
{
    draw_page(false);
}

static bool bs_on_button(nav_key_t id, button_event_t event)
{
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;

    if (s_count == 0) {
        /* 空态：任意键退出 */
        return false;   /* false = 请求退出 */
    }

    int vis = BS_VISIBLE;
    if (vis < 1) vis = 1;

    switch (id) {
    case NAV_UP:
        if (s_sel > 0) {
            s_sel--;
            if (s_sel < s_off) s_off = s_sel;
            draw_page(true);
        }
        return true;
    case NAV_DOWN:
        if (s_sel < s_count - 1) {
            s_sel++;
            if (s_sel >= s_off + vis) s_off = s_sel - vis + 1;
            draw_page(true);
        }
        return true;
    case NAV_CENTER: {
        /* 加载选中的书 */
        haptic_event(HAPTIC_MODE);
        if (book_shelf_load(s_sel) == 0) {
            /* 加载成功：退出书架（base_render MODE_READER 分支接管） */
            return false;   /* false = 请求退出 */
        }
        haptic_event(HAPTIC_ERROR);  /* 加载失败 */
        return true;
    }
    case NAV_RST:
        return false;   /* RST = 退出 */
    default:
        return true;
    }
}

static void bs_exit(void)
{
    /* 退出清理（渲染恢复由 page_router_render_top 承担） */
    LOG_I("book shelf exited");
}

/* ---- 页面协议实例 ---- */

const page_t g_book_shelf_page = {
    "bookshelf",
    bs_render,
    bs_on_button,
    bs_enter,
    bs_exit,
    true    /* owns_display：自绘整帧独占 */
};
