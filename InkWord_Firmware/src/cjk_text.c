/**
 * @file cjk_text.c
 * @brief CJK 点阵混排文本绘制实现
 *
 * 排版原语（utf8_next / ink_span / blit_trimmed / advance）与
 * reader_engine.c 同源副本——reader 尚未上机验证，保持其独立稳定；
 * 两处 advance 语义（空白半角/缺字画框/2px 字距）必须严格一致，
 * 上机验证后合并到本模块单点维护。
 */
#include "cjk_text.h"
#include "cjk_font.h"
#include "cjk_font_sd.h"   /* v1.4 T4.5：SD 卡组子集级联（主集 miss → 子集） */
#include "epd_driver.h"

#include <string.h>
#include <limits.h>

#define CT_SPACING  2   /* 字符间字距（与 reader_engine R_SPACING 一致） */

/* ---- UTF-8 解码：返回消费字节数；坏序列跳 1 字节，cp 落 0xFFFD ---- */
static int utf8_next(const char *p, uint32_t avail, uint32_t *cp)
{
    const uint8_t *b = (const uint8_t *)p;
    if (avail == 0) { *cp = 0; return 0; }
    uint8_t c = b[0];
    if (c < 0x80) { *cp = c; return 1; }
    int n; uint32_t v;
    if ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }   /* 孤立续字节/非法首字节 */
    if ((uint32_t)n > avail) { *cp = 0xFFFD; return 1; }
    for (int i = 1; i < n; i++) {
        if ((b[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (b[i] & 0x3F);
    }
    *cp = v;
    return n;
}

/* 字形水平墨迹范围 [l, l+w)（与 reader_engine ink_span 同源）。
 * 2026-08-23 修复：两循环哨兵均误抄（首循环 r<0 恒真跑满全列、
 * 尾循环 x>=l 不会因 r 赋值而止），l/r 双双被覆盖成墨迹区间反侧端点
 * → w 恒 1 → advance 恒 3px，全部文本挤成一列、分页失效；真机
 * 日志 pages=1 + host 真实字库复现定位；哨兵对齐 reader_engine 正确版 */
static void ink_span(const uint8_t *bits, int cell, int stride,
                     int *out_l, int *out_w)
{
    int l = -1, r = -1;
    for (int x = 0; x < cell && l < 0; x++)
        for (int y = 0; y < cell; y++)
            if (bits[y * stride + x / 8] & (0x80 >> (x % 8))) { l = x; break; }
    if (l < 0) { *out_l = 0; *out_w = 0; return; }   /* 全白字形 */
    for (int x = cell - 1; x > r; x--)
        for (int y = 0; y < cell; y++)
            if (bits[y * stride + x / 8] & (0x80 >> (x % 8))) { r = x; break; }
    *out_l = l;
    *out_w = r - l + 1;
}

/* 墨迹盒裁剪 blit（与 reader_engine blit_trimmed 同源） */
static void blit_trimmed(int x, int y, const uint8_t *bits,
                         int cell, int stride, int l, int w, uint16_t color)
{
    uint8_t tmp[32 * 4];                /* 最大 32px 级 4 字节/行（四级化
                                         * 后 32px 级 stride=4，原 24*3 仅
                                         * 72B 会 memset 溢出栈） */
    int ts = (w + 7) / 8;
    memset(tmp, 0, (size_t)ts * cell);
    for (int gy = 0; gy < cell; gy++) {
        for (int gx = 0; gx < w; gx++) {
            int sx = l + gx;
            if (bits[gy * stride + sx / 8] & (0x80 >> (sx % 8)))
                tmp[gy * ts + gx / 8] |= (0x80 >> (gx % 8));
        }
    }
    epd_gfx_draw_bitmap(x, y, w, cell, tmp, color);
}

/* ---- 排版 advance：单字符步进宽（与 reader_engine glyph_advance 同源；
 * 卡片文本无换行符语义，控制字符按空格处理） ---- */
static int adv_one(uint32_t cp, int level,
                   const uint8_t **bits_out, int *ink_l, int *ink_w)
{
    int cell = cjk_glyph_cell_size(level);
    if (cp < 0x20 || cp == 0x7F) cp = ' ';   /* 控制字符 → 空格 */
    const uint8_t *bits = cjk_glyph_lookup_level(cp, level);
    if (!bits)
        bits = cjk_font_sd_lookup_level(cp, level);  /* T4.5：子集几何
            与主集同构（装载时校验），ink_span/blit 参数直接沿用 */
    if (!bits) {                        /* 未收录：全宽占位（渲染画框） */
        *bits_out = NULL; *ink_l = 0; *ink_w = cell;
        return cell + CT_SPACING;
    }
    int l, w;
    ink_span(bits, cell, cjk_glyph_stride_size(level), &l, &w);
    *bits_out = bits; *ink_l = l; *ink_w = w;
    if (w == 0)                         /* 空白字形：半/全角空格 */
        return cp == 0x3000 ? cell : cell / 2;
    return w + CT_SPACING;
}

/* 画一段字节串（单行片段），返回渲染宽 */
static int draw_run(int x, int y, const char *s, size_t len,
                    int level, uint16_t color)
{
    int cx = x;
    uint32_t avail = (uint32_t)len;
    while (avail > 0 && *s) {
        uint32_t cp;
        int n = utf8_next(s, avail, &cp);
        if (n == 0) break;
        const uint8_t *bits; int l, w;
        int adv = adv_one(cp, level, &bits, &l, &w);
        if (!bits)
            epd_gfx_draw_rect(cx, y, cjk_glyph_cell_size(level),
                              cjk_glyph_cell_size(level), color);
        else if (w > 0)
            blit_trimmed(cx, y, bits, cjk_glyph_cell_size(level),
                         cjk_glyph_stride_size(level), l, w, color);
        cx += adv;
        s += n; avail -= n;
    }
    return cx - x;
}

/* 一段字节串的渲染宽（不画；与 draw_run 同一 advance，保证断行一致） */
static int run_width(const char *s, size_t len, int level)
{
    int w = 0;
    uint32_t avail = (uint32_t)len;
    while (avail > 0 && *s) {
        uint32_t cp;
        int n = utf8_next(s, avail, &cp);
        if (n == 0) break;
        const uint8_t *bits; int l, iw;
        w += adv_one(cp, level, &bits, &l, &iw);
        s += n; avail -= n;
    }
    return w;
}

int cjk_text_width(int level, const char *s)
{
    if (!s) return 0;
    return run_width(s, strlen(s), level);
}

int cjk_text_draw(int x, int y, int level, const char *s, uint16_t color)
{
    if (!s) return 0;
    return draw_run(x, y, s, strlen(s), level, color);
}

static bool is_blank_cp(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == 0x3000;
}

/* ---- 断行核心：完整走完文本并返回总行数；draw 时仅绘制行号落在
 * [skip, skip+cap) 窗口内的内容（量测 pass=false / 绘制 pass=true
 * 共用同一断行逻辑，保证分页页数与渲染行严格一致）。
 * 断行单元：CJK/全角字符逐字可断；ASCII 连续串按词断（词内不拆）；
 * ' '/'\t'/U+3000 行首吞掉、行中放不下时断在其后。 ---- */
static int wrap_walk(int x, int y_top, int max_w, int level,
                     int line_h, const char *s, uint16_t color,
                     bool draw, int skip, int cap)
{
    int line = 0, cur_x = x;
    bool line_empty = true;
    const char *p = s;
    uint32_t avail = (uint32_t)strlen(s);

    while (avail > 0 && *p) {
        uint32_t cp;
        int n = utf8_next(p, avail, &cp);
        if (n == 0) break;

        if (is_blank_cp(cp)) {          /* 空白：行首吞掉 */
            if (!line_empty) {
                const uint8_t *b; int l, w;
                cur_x += adv_one(cp, level, &b, &l, &w);
            }
            p += n; avail -= n;
            continue;
        }

        /* 取断行单元：ASCII 连续串成词（词内不拆），其余单字符 */
        const char *unit = p;
        size_t ulen;
        if (cp < 0x80) {
            ulen = 0;
            while (ulen < avail && (uint8_t)p[ulen] >= 0x21 &&
                   (uint8_t)p[ulen] <= 0x7E)
                ulen++;
        } else {
            ulen = (size_t)n;
        }
        int uw = run_width(unit, ulen, level);

        /* 放不下且行内已有内容 → 换行（行首空白已在上面吞掉） */
        if (!line_empty && cur_x - x + uw > max_w) {
            line++;
            cur_x = x;
            line_empty = true;
        }

        if (draw && line >= skip && line < skip + cap)
            /* 页内相对 y（2026-08-23 修复：此前用绝对行 y，第 2 页起绘制
             * 越过页高压到屏底标签区，真机「上留白+末行重叠」定位） */
            draw_run(cur_x, y_top + (line - skip) * line_h,
                     unit, ulen, level, color);
        cur_x += uw;
        line_empty = false;
        p += ulen; avail -= ulen;
    }
    return line_empty && line == 0 ? 0 : line + 1;
}

int cjk_text_draw_wrap(int x, int y_top, int max_w, int level,
                       int line_h, int max_lines,
                       const char *s, uint16_t color)
{
    if (!s || max_lines <= 0 || max_w <= 0) return 0;
    int total = wrap_walk(x, y_top, max_w, level, line_h, s, color,
                          true, 0, max_lines);
    return total > max_lines ? max_lines : total;   /* 截断语义保持 */
}

int cjk_text_wrap_lines(int max_w, int level, const char *s)
{
    if (!s || max_w <= 0) return 0;
    return wrap_walk(0, 0, max_w, level, 0, s, 0, false, 0, INT_MAX);
}

int cjk_text_draw_wrap_page(int x, int y_top, int max_w, int level,
                            int line_h, int lines_per_page, int page,
                            const char *s, uint16_t color)
{
    if (!s || lines_per_page <= 0 || max_w <= 0 || page < 0) return 0;
    int first = page * lines_per_page;
    int total = wrap_walk(x, y_top, max_w, level, line_h, s, color,
                          true, first, lines_per_page);
    return total > first
        ? (total - first < lines_per_page ? total - first : lines_per_page)
        : 0;
}

bool cjk_text_has_wide(const char *s)
{
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0x80) return true;
    return false;
}
