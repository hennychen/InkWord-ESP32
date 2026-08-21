/**
 * @file cjk_font.c
 * @brief 中文点阵字库 lookup + 《传习录》引文表（生成文件，勿手改）
 *
 * 字形数据在 cjk_font_data.bin（CMake EMBED_FILES 编入固件）：
 *   Songti SC Bold，三级 16/20/24px，
 *   3892 字形 x 16px=32B + 20px=60B + 24px=72B
 *   = 638288 字节。
 * 码点升序二分查找；位图行主序 MSB-first，bit=1 着色（epd_gfx_draw_bitmap 格式）。
 * 由 tools/gen_cjk_font.swift 生成；改字表/引文后重跑 swift tools/gen_cjk_font.swift。
 */
#include "cjk_font.h"
#include <stddef.h>

/* 嵌入字库 bin 的链接符号：PlatformIO arduino 框架用 board_build.embed_files
 * （platformio.ini，src/cjk_font_data.bin，objcopy 符号含路径 src_ 前缀）。
 * 若迁移 ESP-IDF CMake 构建：EMBED_FILES "cjk_font_data.bin" 符号无前缀。 */
extern const uint8_t _binary_src_cjk_font_data_bin_start[];

/* bin 头（小端）：0..3 magic, 4..5 ver, 6..7 levels, 8..11 n,
 * 12..17 cell[3], 18..23 stride[3], 24.. cp 表 u16[n]，4 对齐后三级位图 */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define BIN_BASE (_binary_src_cjk_font_data_bin_start)

static uint32_t glyph_n(void)      { return rd_le32(BIN_BASE + 8); }
static uint16_t glyph_cell(int lvl)  { const uint8_t *p = BIN_BASE + 12 + lvl * 2; return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t glyph_stride(int lvl){ const uint8_t *p = BIN_BASE + 18 + lvl * 2; return (uint16_t)(p[0] | (p[1] << 8)); }

static const uint16_t *cp_table(void)
{
    return (const uint16_t *)(BIN_BASE + 24);
}

static const uint8_t *level_base(int lvl)
{
    uint32_t n = glyph_n();
    uint32_t off = 24 + 2 * n;
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
    return cjk_glyph_lookup_level(cp, CJK_FONT_LEVELS - 1);  /* 24px 兼容（待机页） */
}

int cjk_glyph_cell_size(int level)   { return glyph_cell(level); }
int cjk_glyph_stride_size(int level) { return glyph_stride(level); }
const char *const k_chuanxilu_quotes[24] = {
    "知是行之始，\n行是知之成。",
    "无善无恶心之体，\n有善有恶意之动。\n知善知恶是良知，\n为善去恶是格物。",
    "心外无物，\n心外无事，\n心外无理。",
    "人须在事上磨，\n方立得住。",
    "持志如心痛。\n一心在痛上，\n岂有工夫说闲话、\n管闲事。",
    "谦者，众善之基；\n傲者，众恶之魁。",
    "种树者必培其根，\n种德者必养其心。",
    "善念发而知之，\n而充之；恶念发\n而知之，而遏之。",
    "学须反己。\n若徒责人，只见\n得人不是，不见\n自己非。",
    "良知只是\n个是非之心。",
    "千圣皆过影，\n良知乃吾师。",
    "人生大病，\n只是一傲字。",
    "悔悟是去病之药，\n然以改之为贵。",
    "省察是有事时\n存养，存养是\n无事时省察。",
    "只念念要存天理，\n即是立志。",
    "静时念念去人欲，\n存天理；\n动时念念去人欲，\n存天理。",
    "为学须得个头脑，\n工夫方有着落。",
    "你未看此花时，\n此花与汝心同归\n于寂；你来看此\n花时，则此花颜\n色一时明白起来。",
    "处朋友，\n务相下则得益，\n相上则损。",
    "虚灵不昧，众理具\n而万事出，心外\n无理，心外无事。",
    "良知之在人心，无\n间于圣愚，天下古\n今之所同也。",
    "未有知而不行者。\n知而不行，\n只是未知。",
    "此心无私欲之蔽，\n即是天理。",
    "常快活，\n便是功夫。",
};

/* 引文出处（右下角署名，与引文同字库 24px 级） */
const char k_chuanxilu_attrib[] = "——王阳明《传习录》";
