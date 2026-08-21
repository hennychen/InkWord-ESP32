/**
 * @file cjk_font.h
 * @brief 中文点阵字库接口（三级 16/20/24px + 引文表；生成文件勿手改）
 *
 * 字形数据 cjk_font_data.bin（EMBED_FILES 编入固件），码点升序二分查找。
 * 位图行主序 MSB-first，bit=1 着色，可直接 blit 到 epd_gfx_draw_bitmap。
 * level 档位：0=16px / 1=20px / 2=24px；阅读器按级取形并做墨迹盒变宽
 * 渲染（reader_engine），待机页沿用 24px 兼容 API。
 * 由 tools/gen_cjk_font.swift 生成。
 */
#ifndef INKWORD_CJK_FONT_H
#define INKWORD_CJK_FONT_H

#include <stdint.h>

#define CJK_FONT_LEVELS    3                 /**< 字号级数 */
#define CJK_GLYPH_W       24                 /**< 兼容宏：默认级(24px) 字形宽 */
#define CJK_GLYPH_H       24                 /**< 兼容宏：默认级(24px) 字形高 */
#define CJK_GLYPH_STRIDE  3                  /**< 兼容宏：默认级每行字节数 */
#define CJK_GLYPH_N       3892                 /**< 字形总数（三级共用码点表） */

/** UTF-32 码点 -> 指定级字形位图；未收录返回 NULL（调用方画占位框） */
const uint8_t *cjk_glyph_lookup_level(uint32_t cp, int level);

/** 兼容 API（待机页）：UTF-32 码点 -> 24px 级字形位图；未收录返回 NULL */
const uint8_t *cjk_glyph_lookup(uint32_t cp);

/** 指定级字形边长（px）：16/20/24；level 越界返回 0 */
int cjk_glyph_cell_size(int level);

/** 指定级每行字节数：2/3/3；level 越界返回 0 */
int cjk_glyph_stride_size(int level);

#define CHUANXILU_QUOTE_N 24                /**< 引文条数（=小时数） */
/** 待机页逐时轮换引文（UTF-8，\n 分行，每行 <=8字） */
extern const char *const k_chuanxilu_quotes[CHUANXILU_QUOTE_N];

/** 引文出处（右下角署名，UTF-8 单行） */
extern const char k_chuanxilu_attrib[];

#endif /* INKWORD_CJK_FONT_H */