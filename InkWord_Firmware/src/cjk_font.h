/**
 * @file cjk_font.h
 * @brief 中文子集点阵字库接口（《传习录》引文显示，生成文件勿手改）
 *
 * 字形 24x24 1bpp，行主序 MSB-first，bit=1 着色，
 * 可直接逐字 blit 到 epd_gfx_draw_bitmap。
 * 由 tools/gen_cjk_font.swift 生成；改引文编辑 tools/chuanxilu_quotes.txt 后重跑。
 */
#ifndef INKWORD_CJK_FONT_H
#define INKWORD_CJK_FONT_H

#include <stdint.h>

#define CJK_GLYPH_W       24
#define CJK_GLYPH_H       24
#define CJK_GLYPH_STRIDE  3               /**< 每行字节数 */
#define CJK_GLYPH_N       163                 /**< 字形总数 */

/** UTF-32 码点 -> 字形位图（60 字节）；未收录返回 NULL */
const uint8_t *cjk_glyph_lookup(uint32_t cp);

#define CHUANXILU_QUOTE_N 24                /**< 引文条数（=小时数） */
/** 待机页逐时轮换引文（UTF-8，\n 分行，每行 <=8字） */
extern const char *const k_chuanxilu_quotes[CHUANXILU_QUOTE_N];

/** 引文出处（右下角署名，UTF-8 单行） */
extern const char k_chuanxilu_attrib[];

#endif /* INKWORD_CJK_FONT_H */