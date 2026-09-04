/**
 * @file bookmark_mgr.c
 * @brief 书签管理实现（2026-09-05 阅读器增强阶段三）
 *
 * 书签表静态分配（BOOKMARK_MAX=32 枚/书），按页码升序维护。
 * NVS blob 键 "bm_" + signature 前 4 位 hex（≤15 字符 NVS 上限）。
 * 每次增删后立即 save（NVS 写入频率极低，磨损可忽略）。
 */
#include "bookmark_mgr.h"
#include "reader_engine.h"
#include "debug_log.h"

#include "nvs.h"
#include "settings_keys.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "BOOKMARK";

static bookmark_t s_marks[BOOKMARK_MAX];
static int s_count = 0;
static uint32_t s_sig = 0;

/* NVS 键构造：bm_ + signature 前 4 位 hex = 7 字符（≤15 上限） */
static void bm_key(char *buf, size_t sz, uint32_t sig)
{
    snprintf(buf, sz, "bm_%04x", (unsigned)(sig & 0xFFFF));
}

/* 按页码升序插入（保持有序） */
static int insert_sorted(const bookmark_t *bm)
{
    if (s_count >= BOOKMARK_MAX) return -1;
    /* 查找插入位置 */
    int pos = s_count;
    for (int i = 0; i < s_count; i++) {
        if (s_marks[i].page == bm->page) return -2;   /* 已存在 */
        if (s_marks[i].page > bm->page) { pos = i; break; }
    }
    /* 后移 */
    for (int i = s_count; i > pos; i--)
        s_marks[i] = s_marks[i - 1];
    s_marks[pos] = *bm;
    s_count++;
    return 0;
}

void bookmark_mgr_load(uint32_t signature)
{
    s_count = 0;
    s_sig = signature;
    if (signature == 0) return;

    char key[16];
    bm_key(key, sizeof(key), signature);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(bookmark_t) * BOOKMARK_MAX;
    esp_err_t err = nvs_get_blob(h, key, s_marks, &len);
    nvs_close(h);
    if (err == ESP_OK) {
        s_count = (int)(len / sizeof(bookmark_t));
        if (s_count > BOOKMARK_MAX) s_count = BOOKMARK_MAX;
    }
    LOG_I("bookmarks loaded: %d (sig=%08x)", s_count, (unsigned)signature);
}

void bookmark_mgr_save(void)
{
    if (s_sig == 0) return;
    char key[16];
    bm_key(key, sizeof(key), s_sig);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_count > 0)
        nvs_set_blob(h, key, s_marks, sizeof(bookmark_t) * s_count);
    else
        nvs_erase_key(h, key);   /* 空书签表删键 */
    nvs_commit(h);
    nvs_close(h);
}

int bookmark_add(int page, const char *note)
{
    bookmark_t bm;
    memset(&bm, 0, sizeof(bm));
    bm.page = page;
    bm.byte_offset = reader_engine_page_offset(page);
    if (note) snprintf(bm.note, sizeof(bm.note), "%s", note);
    int r = insert_sorted(&bm);
    if (r == 0) bookmark_mgr_save();
    return r;
}

int bookmark_remove(int page)
{
    for (int i = 0; i < s_count; i++) {
        if (s_marks[i].page == page) {
            /* 前移 */
            for (int j = i; j < s_count - 1; j++)
                s_marks[j] = s_marks[j + 1];
            s_count--;
            bookmark_mgr_save();
            return 0;
        }
    }
    return -1;
}

bool bookmark_exists(int page)
{
    for (int i = 0; i < s_count; i++)
        if (s_marks[i].page == page) return true;
    return false;
}

int bookmark_count(void) { return s_count; }

const bookmark_t *bookmark_at(int idx)
{
    return (idx >= 0 && idx < s_count) ? &s_marks[idx] : NULL;
}

int bookmark_jump(int idx)
{
    if (idx < 0 || idx >= s_count) return 0;
    return s_marks[idx].page;
}

void bookmark_mgr_clear(void)
{
    s_count = 0;
    s_sig = 0;
}
