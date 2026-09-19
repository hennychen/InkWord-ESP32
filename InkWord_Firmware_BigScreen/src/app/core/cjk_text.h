/**
 * @file cjk_text.h
 * @brief CJK 点阵混排文本绘制（词卡释义/标签；P3 字库资产复用）
 *
 * 六级点阵字库（cjk_font.h，16/20/24/32/40/48px）之上的通用文本层：
 * UTF-8
 * 混排（ASCII + 中文/全角标点）、墨迹盒变宽渲染、CJK 按字断行 /
 * ASCII 按词断行（词内不拆）。与 reader_engine.c 的整页排版同源
 * （advance 原语一致），面向卡片类小块文本；上机验证后可评估
 * 将 reader_engine 迁移到本模块统一维护。
 *
 * 分页支持（词卡释义超一屏时上下键翻页，2026-08-23）：量测
 * （cjk_text_wrap_lines）与分页绘制（cjk_text_draw_wrap_page）
 * 与 cjk_text_draw_wrap 共用同一断行核心，建页与渲染必然一致。
 *
 * 字符集边界：字库 = GB2312 一级 ∪ 全角标点 ∪ ASCII 0x20-0x7E
 * （含空格）∪ IPA 21 字符 + 音标列收集集（2026-08-23 音标行点阵化，
 * 词卡音标行不再走 FreeSans）；超出字符画 cell 空心框占位。
 *
 * 坐标语义：顶左原点（字形 cell 顶边对齐 y），与 epd_gfx 位图
 * 一致；FreeSans 的基线 y 语义不适用于本模块。
 */
#ifndef INKWORD_CJK_TEXT_H
#define INKWORD_CJK_TEXT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单行渲染宽（px，含 2px 字距；不断行不截断）。
 * @param level 0=16px / 1=20px / 2=24px / 3=32px / 4=40px / 5=48px。
 */
int cjk_text_width(int level, const char *s);

/**
 * @brief 单行绘制（不换行不截断，可能画出屏，调用方保证宽度）。
 * @return 实际绘制宽（px）。
 */
int cjk_text_draw(int x, int y, int level, const char *s, uint16_t color);

/**
 * @brief 断行绘制：超 max_w 换行（line_h 行距，首行顶 = y_top）；
 *        超 max_lines 截断丢弃剩余；返回实际绘制行数（s 空 = 0）。
 *
 * 断行单元：CJK/全角字符逐字可断；ASCII 连续串按词断（词内不拆，
 * 词自身超宽独占一行截断）；' '/'\t'/U+3000 行首吞掉、行中放不下
 * 时断在其后。
 */
int cjk_text_draw_wrap(int x, int y_top, int max_w, int level,
                       int line_h, int max_lines,
                       const char *s, uint16_t color);

/**
 * @brief 断行量测：返回 s 在 max_w 宽度下需要的总行数（不绘制；空串 0）。
 *        断行规则与 cjk_text_draw_wrap 严格同源，分页页数推导用。
 */
int cjk_text_wrap_lines(int max_w, int level, const char *s);

/**
 * @brief 分页断行绘制：从全文第 page*lines_per_page 行起，绘制至多
 *        lines_per_page 行（y_top 为该页首行顶坐标）；页越界不绘制。
 * @return 本页实际绘制行数（0 = 空串或页号越界）。
 */
int cjk_text_draw_wrap_page(int x, int y_top, int max_w, int level,
                            int line_h, int lines_per_page, int page,
                            const char *s, uint16_t color);

/** @brief 是否含多字节字符（非 ASCII；调用方选点阵/FreeSans 路径用）。 */
bool cjk_text_has_wide(const char *s);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CJK_TEXT_H */
