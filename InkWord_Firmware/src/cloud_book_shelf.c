/**
 * @file cloud_book_shelf.c
 * @brief 云书屋管理实现（R3.2 云书屋，2026-09-21）
 *
 * 从云端拉取书目列表（GET /api/device/books），解析 JSON 填充条目表。
 * 用户选择后下载到 /sdcard/books/{book_key}.txt，下载成功自动刷新
 * 本地书架（book_shelf_scan）。
 *
 * 列表 UI 作为覆盖层经 page_router_push 入栈，上/下选择、中键下载、
 * RST 退出。已下载的书籍显示 ✓ 标记。
 *
 * 刷新策略：进入/下载完成全刷；光标移动局刷列表区。
 */
#include "cloud_book_shelf.h"
#include "book_shelf.h"
#include "sync_client.h"
#include "storage_manager.h"
#include "page_router.h"
#include "epd_driver.h"
#include "cjk_text.h"
#include "cjk_font.h"
#include "layout_profile.h"
#include "debug_log.h"
#include "haptic.h"
#include "wifi_manager.h"
#include "cJSON.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "CLOUDSHELF";

/* ---- 容量与常量 ---- */
#define CLOUD_SHELF_MAX   32        /* 云端书目最多 32 本 */
#define BOOKS_DIR         "/sdcard/books"
#define JSON_BUF_SIZE     (8 * 1024)  /* 8KB JSON 缓冲 */

/* ---- 几何派生（book_shelf 同范式） ---- */
#define CS_STATUS_H     (layout_profile_get()->status_h)
#define CS_ITEM_H       (layout_profile_get()->item_h)
#define CS_MARGIN_X     (layout_profile_get()->margin_x)
#define CS_HINT_H       (layout_profile_get()->hint_h)
#define CS_FONT_LVL     (layout_profile_get()->font_lvl_main)
#define CS_LIST_TOP     (CS_STATUS_H + 2)
#define CS_LIST_H       (epd_gfx_height() - CS_STATUS_H - CS_HINT_H)
#define CS_VISIBLE      (CS_LIST_H / CS_ITEM_H)

/* ---- 静态状态 ---- */
static cloud_book_entry_t s_entries[CLOUD_SHELF_MAX];
static int          s_count = 0;
static int          s_sel   = 0;      /* 当前选中（0 基） */
static int          s_off   = 0;      /* 滚动偏移 */

/* ---- 工具函数 ---- */

/* 检查本地是否已下载该书籍 */
static bool check_downloaded(const char *book_key)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.txt", BOOKS_DIR, book_key);
    return storage_file_exists(path);
}

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

/* 绘制页面 */
static void draw_page(bool partial)
{
    int w = epd_gfx_width();
    int h = epd_gfx_height();

    /* 顶部状态栏 */
    epd_gfx_fill_rect(0, 0, w, CS_STATUS_H, EPD_GFX_WHITE);
    epd_gfx_draw_hline(0, CS_STATUS_H - 1, w, EPD_GFX_BLACK);
    int title_y = (CS_STATUS_H - cjk_glyph_cell_size(CS_FONT_LVL)) / 2;
    cjk_text_draw(CS_MARGIN_X, title_y, CS_FONT_LVL, "云书屋", EPD_GFX_BLACK);

    /* 列表区 */
    if (s_count > 0) {
        int vis = CS_VISIBLE;
        if (vis < 1) vis = 1;

        for (int i = 0; i < vis && s_off + i < s_count; i++) {
            int idx = s_off + i;
            int y = CS_LIST_TOP + i * CS_ITEM_H;
            bool sel = (idx == s_sel);

            /* 选中高亮 */
            if (sel)
                epd_gfx_fill_rect(CS_MARGIN_X, y,
                                  w - 2 * CS_MARGIN_X, CS_ITEM_H - 2,
                                  EPD_GFX_BLACK);

            uint16_t fg = sel ? EPD_GFX_WHITE : EPD_GFX_BLACK;

            /* 书名（CJK 点阵） */
            const cloud_book_entry_t *be = &s_entries[idx];
            cjk_text_draw(CS_MARGIN_X + 4,
                          y + (CS_ITEM_H - cjk_glyph_cell_size(CS_FONT_LVL)) / 2 - 2,
                          CS_FONT_LVL, be->title, fg);

            /* 右侧徽标：已下载标记 + 文件大小 */
            char badge[32];
            char szbuf[12];
            format_size(be->file_size, szbuf, sizeof(szbuf));
            if (be->downloaded)
                snprintf(badge, sizeof(badge), "✓ %s", szbuf);
            else
                snprintf(badge, sizeof(badge), "%s", szbuf);
            int bw, bh;
            epd_gfx_text_bounds(badge, 1, &bw, &bh);
            epd_gfx_draw_text(w - CS_MARGIN_X - bw - 4,
                              y + CS_ITEM_H / 2, badge, fg, 1);
        }

        /* 滚动条 */
        if (s_count > vis) {
            int sb_h = (vis * CS_LIST_H) / s_count;
            if (sb_h < 8) sb_h = 8;
            int sb_y = (s_off * (CS_LIST_H - sb_h)) / (s_count - vis);
            epd_gfx_fill_rect(w - 3, CS_LIST_TOP + sb_y, 2, sb_h,
                              EPD_GFX_BLACK);
        }
    } else {
        /* 空态提示 */
        cjk_text_draw(CS_MARGIN_X, CS_LIST_TOP + 40, CS_FONT_LVL,
                      "暂无云端书籍", EPD_GFX_BLACK);
    }

    /* 底部提示栏 */
    if (CS_HINT_H > 0) {
        int hy = h - CS_HINT_H;
        int asc = CS_FONT_LVL >= 3 ? 3 : 1;
        epd_gfx_fill_rect(0, hy, w, CS_HINT_H, EPD_GFX_WHITE);
        epd_gfx_draw_hline(0, hy, w, EPD_GFX_BLACK);
        epd_gfx_draw_text(CS_MARGIN_X, hy + CS_HINT_H - (asc > 1 ? 6 : 4),
                          "UP/DN:sel  MID:download  RST:back",
                          EPD_GFX_BLACK, asc);
    }

    if (partial)
        epd_gfx_flush_window(0, CS_STATUS_H, w, h - CS_STATUS_H);
    else
        epd_gfx_flush();
}

/* ---- 页面协议回调 ---- */

static void cs_enter(void)
{
    s_sel = 0;
    s_off = 0;

    if (!wifi_is_connected()) {
        LOG_W("cloud shelf: wifi not connected");
        s_count = 0;
        draw_page(false);
        return;
    }

    int n = cloud_book_shelf_fetch();
    if (n < 0) {
        LOG_W("cloud shelf: fetch failed");
        s_count = 0;
    }
    draw_page(false);
}

static void cs_render(void)
{
    draw_page(false);
}

static bool cs_on_button(nav_key_t id, button_event_t event)
{
    if (event != BUTTON_EVENT_SHORT_PRESS) return true;

    if (s_count == 0) {
        /* 空态：仅 RST 退出 */
        if (id == NAV_RST) return false;
        haptic_event(HAPTIC_ERROR);
        return true;
    }

    int vis = CS_VISIBLE;
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

    case NAV_CENTER:
        /* 下载 */
        if (!wifi_is_connected()) {
            haptic_event(HAPTIC_ERROR);
            return true;
        }
        {
            cloud_book_entry_t *e = &s_entries[s_sel];
            if (e->downloaded) {
                LOG_I("already downloaded: %s", e->book_key);
                haptic_event(HAPTIC_MODE);
                return true;
            }
            LOG_I("downloading: %s", e->book_key);
            if (cloud_book_shelf_download(s_sel) == 0) {
                e->downloaded = true;
                book_shelf_scan();  /* 刷新本地书架 */
                haptic_event(HAPTIC_MODE);
                draw_page(false);
            } else {
                haptic_event(HAPTIC_ERROR);
            }
        }
        return true;

    case NAV_SET:
        /* SET 无操作 */
        return true;

    case NAV_RST:
        /* RST 退出 */
        return false;

    default:
        return true;
    }
}

/* 页面实例 */
const page_t g_cloud_book_shelf_page = {
    .name = "cloud_book_shelf",
    .render = cs_render,
    .on_button = cs_on_button,
    .enter = cs_enter,
    .exit = NULL,
    .owns_display = true,
};

/* ---- 公共 API ---- */

int cloud_book_shelf_fetch(void)
{
    char *json_buf = (char *)heap_caps_malloc(JSON_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!json_buf) {
        LOG_E("cloud shelf: JSON buffer alloc failed");
        return -1;
    }

    int rc = sync_pull_book_list(json_buf, JSON_BUF_SIZE);
    if (rc != 0) {
        LOG_E("cloud shelf: pull failed (%d)", rc);
        free(json_buf);
        return -1;
    }

    /* 解析 JSON：{ code, data: { books: [{ bookKey, title, author, size }] } } */
    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    if (!root) {
        LOG_E("cloud shelf: JSON parse failed");
        return -1;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *books = data ? cJSON_GetObjectItem(data, "books") : NULL;
    if (!books || !cJSON_IsArray(books)) {
        LOG_W("cloud shelf: no books array");
        cJSON_Delete(root);
        s_count = 0;
        return 0;
    }

    s_count = 0;
    int arr_size = cJSON_GetArraySize(books);
    for (int i = 0; i < arr_size && s_count < CLOUD_SHELF_MAX; i++) {
        cJSON *item = cJSON_GetArrayItem(books, i);
        if (!item) continue;

        cloud_book_entry_t *e = &s_entries[s_count];
        memset(e, 0, sizeof(*e));

        cJSON *v;
        v = cJSON_GetObjectItem(item, "bookKey");
        if (v && cJSON_GetStringValue(v))
            snprintf(e->book_key, sizeof(e->book_key), "%s", cJSON_GetStringValue(v));

        v = cJSON_GetObjectItem(item, "title");
        if (v && cJSON_GetStringValue(v))
            snprintf(e->title, sizeof(e->title), "%s", cJSON_GetStringValue(v));

        v = cJSON_GetObjectItem(item, "author");
        if (v && cJSON_GetStringValue(v))
            snprintf(e->author, sizeof(e->author), "%s", cJSON_GetStringValue(v));

        v = cJSON_GetObjectItem(item, "size");
        if (v) e->file_size = (uint32_t)v->valuedouble;

        /* 检查本地是否已下载 */
        e->downloaded = check_downloaded(e->book_key);

        if (e->book_key[0] && e->title[0])
            s_count++;
    }

    cJSON_Delete(root);
    LOG_I("cloud shelf: fetched %d books", s_count);
    return s_count;
}

int cloud_book_shelf_count(void)
{
    return s_count;
}

const cloud_book_entry_t *cloud_book_shelf_at(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_entries[idx];
}

int cloud_book_shelf_download(int idx)
{
    if (idx < 0 || idx >= s_count) return -1;

    cloud_book_entry_t *e = &s_entries[idx];
    char save_path[128];
    snprintf(save_path, sizeof(save_path), "%s/%s.txt", BOOKS_DIR, e->book_key);

    /* 确保目录存在 */
    storage_mkdir_p(BOOKS_DIR);

    int rc = sync_download_book(e->book_key, save_path);
    if (rc != 0) {
        LOG_E("download failed: %s", e->book_key);
        return -1;
    }

    e->downloaded = true;
    LOG_I("downloaded: %s -> %s", e->book_key, save_path);
    return 0;
}
