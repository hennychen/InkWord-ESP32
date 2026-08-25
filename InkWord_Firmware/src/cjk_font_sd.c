/**
 * @file cjk_font_sd.c
 * @brief SD 卡组子集字库级联查找实现（v1.4 T4.5）
 *
 * 二进制整体读入 PSRAM（子集量级 ~几十 KB：卡组差集字符 × 164B/字
 * 三级位图），CKF1 头 + 几何 + 尺寸三重校验后常驻，lookup 二分镜像
 * 主集 cjk_font.c 逻辑（基址换 PSRAM 缓冲）。文件 IO 用 stdio 直连
 * VFS（storage_read_text 是文本语义 buf_size-1+'\0'，二进制不适用）。
 */
#include "cjk_font_sd.h"
#include "storage_manager.h"
#include "debug_log.h"

#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "CJKSD";

/* 与主集/生成工具三方同源的几何不变量（gen_cjk_font.swift LEVELS、
 * cjk_font.c 消费几何、cjk_text.c blit 参数）：装载时校验，防手拷
 * 旧版/异源 bin 后位图按错误几何 blit 花屏 */
#define SD_FONT_LEVELS      3
static const uint16_t kCell[SD_FONT_LEVELS]   = { 16, 20, 24 };
static const uint16_t kStride[SD_FONT_LEVELS] = { 2, 3, 3 };
static const uint32_t kGlyphBytes[SD_FONT_LEVELS] = { 16 * 2, 20 * 3, 24 * 3 };

static uint8_t *s_bin = NULL;      /* PSRAM 子集缓冲（NULL=未装载） */
static uint32_t s_n = 0;           /* 字形数 */

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

void cjk_font_sd_unload(void)
{
    if (s_bin) {
        heap_caps_free(s_bin);
        s_bin = NULL;
    }
    s_n = 0;
}

int cjk_font_sd_loaded(void)
{
    return s_bin != NULL;
}

int cjk_font_sd_load_file(const char *path)
{
    cjk_font_sd_unload();               /* 切卡组语义：旧子集先退场 */
    if (!path || !path[0]) return 1;

    FILE *f = fopen(path, "rb");
    if (!f) return 1;                   /* 无子集：主集已覆盖的正常路径 */

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 24 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    /* 头 + 几何校验（最小 24B 头之外逐项验，防旧版/异源文件） */
    uint8_t head[24];
    if (fread(head, 1, sizeof(head), f) != sizeof(head)) { fclose(f); return -1; }
    if (memcmp(head, "CKF1", 4) != 0) {
        LOG_E("%s: bad magic", path);
        fclose(f);
        return -1;
    }
    if (rd_le16(head + 6) != SD_FONT_LEVELS) {
        LOG_E("%s: levels=%u != %d", path, rd_le16(head + 6), SD_FONT_LEVELS);
        fclose(f);
        return -1;
    }
    for (int i = 0; i < SD_FONT_LEVELS; i++) {
        if (rd_le16(head + 12 + i * 2) != kCell[i] ||
            rd_le16(head + 18 + i * 2) != kStride[i]) {
            LOG_E("%s: geometry mismatch (cell/stride)", path);
            fclose(f);
            return -1;
        }
    }
    uint32_t n = rd_le32(head + 8);
    if (n == 0) { fclose(f); return -1; }   /* 空子集不该有产物文件 */

    /* 尺寸整账：24B 头 + 2n 码点（4 对齐）+ 三级位图 */
    uint32_t expect = 24 + 2 * n;
    expect = (expect + 3) & ~3u;
    for (int i = 0; i < SD_FONT_LEVELS; i++) expect += n * kGlyphBytes[i];
    if ((uint32_t)sz != expect) {
        LOG_E("%s: size %ld != expect %u (n=%u)", path, sz, expect, n);
        fclose(f);
        return -1;
    }

    s_bin = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!s_bin) {
        LOG_E("alloc %ldB SPIRAM failed", sz);
        fclose(f);
        return -1;
    }
    /* 头已读走 24B：回填后整读余量 */
    memcpy(s_bin, head, sizeof(head));
    size_t want = (size_t)sz - sizeof(head);
    if (fread(s_bin + sizeof(head), 1, want, f) != want) {
        LOG_E("%s: read body failed", path);
        cjk_font_sd_unload();
        fclose(f);
        return -1;
    }
    fclose(f);
    s_n = n;
    LOG_I("deck font loaded: %s (%ldB, %u glyphs)", path, sz, n);
    return 0;
}

int cjk_font_sd_load(const char *deck_id)
{
    char path[48];
    if (!deck_id || !deck_id[0]) {          /* 默认卡组：无子集语义 */
        cjk_font_sd_unload();
        return 1;
    }
    snprintf(path, sizeof(path), "%s/fonts/deck_%s.bin",
             storage_mount_point(), deck_id);
    return cjk_font_sd_load_file(path);
}

const uint8_t *cjk_font_sd_lookup_level(uint32_t cp, int level)
{
    if (!s_bin || level < 0 || level >= SD_FONT_LEVELS) return NULL;
    if (cp > 0xFFFF) return NULL;           /* CKF1 码点表 u16 */

    const uint16_t *tab = (const uint16_t *)(s_bin + 24);
    int lo = 0, hi = (int)s_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tab[mid] == (uint16_t)cp) {
            uint32_t off = 24 + 2 * s_n;
            off = (off + 3) & ~3u;
            for (int i = 0; i < level; i++) off += s_n * kGlyphBytes[i];
            return s_bin + off + (uint32_t)mid * kGlyphBytes[level];
        }
        if (tab[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
