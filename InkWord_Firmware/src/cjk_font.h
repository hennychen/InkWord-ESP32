/**
 * @file cjk_font.h
 * @brief 中文点阵字库接口（四级 16/20/24/32px + 引文表；生成文件勿手改）
 *
 * 字形数据 cjk_font_data.bin（EMBED_FILES 编入固件），码点升序二分查找。
 * 位图行主序 MSB-first，bit=1 着色，可直接 blit 到 epd_gfx_draw_bitmap。
 * level 档位：0=16px / 1=20px / 2=24px / 3=32px（32px 级 LARGE 档大屏，
 * 2026-09-03）；阅读器按级取形并做墨迹盒变宽渲染（reader_engine）。
 * 由 tools/gen_cjk_font.swift 生成。
 */
#ifndef INKWORD_CJK_FONT_H
#define INKWORD_CJK_FONT_H

#include <stdint.h>

#define CJK_FONT_LEVELS    4                 /**< 字号级数 */
#define CJK_GLYPH_W       32                 /**< 兼容宏：最大级字形宽（零消费方，随级数自适） */
#define CJK_GLYPH_H       32                 /**< 兼容宏：最大级字形高 */
#define CJK_GLYPH_STRIDE  4       /**< 兼容宏：最大级每行字节数 */
#define CJK_GLYPH_N       3935                 /**< 字形总数（各级共用码点表） */

/** UTF-32 码点 -> 指定级字形位图；未收录返回 NULL（调用方画占位框） */
const uint8_t *cjk_glyph_lookup_level(uint32_t cp, int level);

/** 兼容 API：UTF-32 码点 -> 最大级字形位图（零外部消费方）；未收录返回 NULL */
const uint8_t *cjk_glyph_lookup(uint32_t cp);

/** 指定级字形边长（px）：16/20/24/32；level 越界返回 0 */
int cjk_glyph_cell_size(int level);

/** 指定级每行字节数：2/3/3/4；level 越界返回 0 */
int cjk_glyph_stride_size(int level);

#define CHUANXILU_QUOTE_N 24                /**< 引文条数（=小时数） */
/** 待机页逐时轮换引文（UTF-8，\n 分行，每行 <=8字） */
extern const char *const k_chuanxilu_quotes[CHUANXILU_QUOTE_N];

/** 引文出处（右下角署名，UTF-8 单行） */
extern const char k_chuanxilu_attrib[];

#endif /* INKWORD_CJK_FONT_H */