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
 * 旧版/异源 bin 后位图按错误几何 blit 花屏。
 * 级数动态兼容（2026-09-03 四级化）：主集升 16/20/24/32px 四级，
 * SD 上的旧三级子集（levels=3）继续可用——白名单含四级全量几何，
 * 文件头 levels ∈ {3,4} 且逐级匹配即收，位图定位按文件自描述级数 */
#define SD_FONT_LEVELS_MAX 4
#define SD_FONT_LEVELS_MIN 3
static const uint16_t kCell[SD_FONT_LEVELS_MAX]   = { 16, 20, 24, 32 };
static const uint16_t kStride[SD_FONT_LEVELS_MAX] = { 2, 3, 3, 4 };
static const uint32_t kGlyphBytes[SD_FONT_LEVELS_MAX] =
    { 16 * 2, 20 * 3, 24 * 3, 32 * 4 };

static uint8_t *s_bin = NULL;      /* PSRAM 子集缓冲（NULL=未装载） */
static uint32_t s_n = 0;           /* 字形数 */
static uint16_t s_levels = 0;      /* 文件自描述级数（装载时校验入白名单） */
static uint16_t s_cp_off = 0;      /* cp 表起点 = 12 + levels*4（动态） */

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
    s_levels = 0;
    s_cp_off = 0;
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
    if (sz <= 28 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    /* 头 + 几何校验（最大 28B 头（四级）之外逐项验，防旧版/异源文件；
     * 三级旧子集头 24B 有效，多读 4B 位图首字节无害（只取字段） */
    uint8_t head[28];
    if (fread(head, 1, sizeof(head), f) != sizeof(head)) { fclose(f); return -1; }
    if (memcmp(head, "CKF1", 4) != 0) {
        LOG_E("%s: bad magic", path);
        fclose(f);
        return -1;
    }
    const uint16_t levels = rd_le16(head + 6);
    if (levels < SD_FONT_LEVELS_MIN || levels > SD_FONT_LEVELS_MAX) {
        LOG_E("%s: levels=%u not in [%d,%d]", path, levels,
              SD_FONT_LEVELS_MIN, SD_FONT_LEVELS_MAX);
        fclose(f);
        return -1;
    }
    for (int i = 0; i < levels; i++) {
        if (rd_le16(head + 12 + i * 2) != kCell[i] ||
            rd_le16(head + 12 + levels * 2 + i * 2) != kStride[i]) {
            LOG_E("%s: geometry mismatch (cell/stride)", path);
            fclose(f);
            return -1;
        }
    }
    uint32_t n = rd_le32(head + 8);
    if (n == 0) { fclose(f); return -1; }   /* 空子集不该有产物文件 */

    /* 尺寸整账：头(12+levels*4) + 2n 码点（4 对齐）+ 按文件级数位图 */
    const uint16_t cp_off = (uint16_t)(12 + levels * 4);
    uint32_t expect = cp_off + 2 * n;
    expect = (expect + 3) & ~3u;
    for (int i = 0; i < levels; i++) expect += n * kGlyphBytes[i];
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
    /* 头已读走 28B：回填后整读余量（三级旧文件回填尾 4B 为位图首字节，
     * fread 余量从文件当前位置续读，不丢失） */
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
    s_levels = levels;
    s_cp_off = cp_off;
    LOG_I("deck font loaded: %s (%ldB, %u glyphs, %u levels)",
          path, sz, n, levels);
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
    if (!s_bin || level < 0 || level >= s_levels) return NULL;
    if (cp > 0xFFFF) return NULL;           /* CKF1 码点表 u16 */

    const uint16_t *tab = (const uint16_t *)(s_bin + s_cp_off);
    int lo = 0, hi = (int)s_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tab[mid] == (uint16_t)cp) {
            uint32_t off = s_cp_off + 2 * s_n;
            off = (off + 3) & ~3u;
            for (int i = 0; i < level; i++) off += s_n * kGlyphBytes[i];
            return s_bin + off + (uint32_t)mid * kGlyphBytes[level];
        }
        if (tab[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
