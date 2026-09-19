/**
 * @file cjk_font.c
 * @brief 中文点阵字库 lookup（大屏手维护；数据在生成 bin）
 *
 * 字形数据在 cjk_font_data.bin（CMake EMBED_FILES 编入固件）：
 *   16/20px PingFang SC Bold + 24/32/40/48px Kaiti SC Bold，六级
 *   16/20/24/32/40/48px，3935 字形 x 16px=32B + 20px=60B + 24px=72B
 *   + 32px=128B + 40px=200B + 48px=288B = 3069300 字节
 *   （40/48px 级大屏 UI 重设计 2026-09-17 增）。
 * 码点升序二分查找；位图行主序 MSB-first，bit=1 着色（epd_gfx_draw_bitmap 格式）。
 * 由 tools/gen_cjk_font.swift 生成（大屏版只产 bin，本 .c 手维护）；
 * 改字表后重跑 swift tools/gen_cjk_font.swift。
 */
#include "cjk_font.h"
#include <stddef.h>

/* IDF CMake EMBED_FILES 嵌入符号（src/CMakeLists.txt：
 * EMBED_FILES "app/data/cjk_font_data.bin"，路径斜线→下划线进符号。
 * 2026-11 大屏迁移自小屏 Arduino objcopy 版改写——彼时路径
 * src/cjk_font_data.bin 带 src_ 前缀，文件头注释预留同步点已兑现） */
extern const uint8_t _binary_app_data_cjk_font_data_bin_start[];
#define BIN_BASE (_binary_app_data_cjk_font_data_bin_start)

/* bin 头（小端，自描述）：0..3 magic, 4..5 ver, 6..7 levels, 8..11 n,
 * cell[levels] @12、stride[levels] 紧随，cp 表起点 = 12+levels*4，
 * 4 对齐后按级位图（2026-09-03 四级化，消费端动态计算勿写死偏移） */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t glyph_n(void)      { return rd_le32(BIN_BASE + 8); }
static uint16_t bin_levels(void)   { const uint8_t *p = BIN_BASE + 6; return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t glyph_cell(int lvl)  { const uint8_t *p = BIN_BASE + 12 + lvl * 2; return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t glyph_stride(int lvl){ const uint8_t *p = BIN_BASE + 12 + bin_levels() * 2 + lvl * 2; return (uint16_t)(p[0] | (p[1] << 8)); }
/* cp 表起点 = 12 + levels*4（bin 头自描述，cp_table/level_base 唯一同源。
 * 2026-09-03 四级化漏改事故存档：level_base 曾硬编码旧三级头 24，
 * 四级 bin 下位图基址左移 4B —— 16/32px 级 stride 整除4B 恰整行仅
 * 字形平移（视觉无感），20/24px 级 stride=3 行错乱，真机释义区
 * 每字右侧破碎（2026-09-03 3.7" 真机定位，勿再写死偏移） */
static uint32_t cp_table_off(void) { return 12 + (uint32_t)bin_levels() * 4; }


static const uint16_t *cp_table(void)
{
    return (const uint16_t *)(BIN_BASE + cp_table_off());
}

static const uint8_t *level_base(int lvl)
{
    uint32_t n = glyph_n();
    uint32_t off = cp_table_off() + 2 * n;   /* cp 表终点 = 位图区起点 */
    off = (off + 3) & ~3u;
    for (int i = 0; i < lvl; i++)
        off += n * (uint32_t)glyph_stride(i) * (uint32_t)glyph_cell(i);
    return BIN_BASE + off;
}

const uint8_t *cjk_glyph_lookup_level(uint32_t cp, int level)
{
    if (level < 0 || level >= CJK_FONT_LEVELS) return NULL;
    const uint16_t *tab = cp_table();
    int lo = 0, hi = (int)glyph_n() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tab[mid] == (uint16_t)cp)
            return level_base(level) +
                   (uint32_t)mid * glyph_stride(level) * glyph_cell(level);
        if (tab[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

const uint8_t *cjk_glyph_lookup(uint32_t cp)
{
    return cjk_glyph_lookup_level(cp, CJK_FONT_LEVELS - 1);  /* 最大级兼容（零外部消费方） */
}

int cjk_glyph_cell_size(int level)   { return glyph_cell(level); }
int cjk_glyph_stride_size(int level) { return glyph_stride(level); }
