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
#include "epd_driver.h"

#include <string.h>

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

/* 字形水平墨迹范围 [l, l+w)（与 reader_engine ink_span 同源） */
static void ink_span(const uint8_t *bits, int cell, int stride,
                     int *out_l, int *out_w)
{
    int l = -1, r = -1;
    for (int x = 0; x < cell && r < 0; x++)
        for (int y = 0; y < cell; y++)
            if (bits[y * stride + x / 8] & (0x80 >> (x % 8))) { l = x; break; }
    if (l < 0) { *out_l = 0; *out_w = 0; return; }   /* 全白字形 */
    for (int x = cell - 1; x >= l; x--)
        for (int y = 0; y < cell; y++)
            if (bits[y * stride + x / 8] & (0x80 >> (x % 8))) { r = x; break; }
    *out_l = l;
    *out_w = r - l + 1;
}

/* 墨迹盒裁剪 blit（与 reader_engine blit_trimmed 同源） */
static void blit_trimmed(int x, int y, const uint8_t *bits,
                         int cell, int stride, int l, int w, uint16_t color)
{
    uint8_t tmp[24 * 3];                /* 最大 24px 级 3 字节/行 */
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

int cjk_text_draw_wrap(int x, int y_top, int max_w, int level,
                       int line_h, int max_lines,
                       const char *s, uint16_t color)
{
    if (!s || max_lines <= 0 || max_w <= 0) return 0;

    int line = 0, cur_x = x, y = y_top;
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
            if (line >= max_lines) return line;   /* 截断丢弃剩余 */
            y += line_h;
            cur_x = x;
            line_empty = true;
        }

        draw_run(cur_x, y, unit, ulen, level, color);
        cur_x += uw;
        line_empty = false;
        p += ulen; avail -= ulen;
    }
    return line_empty && line == 0 ? 0 : line + 1;
}

bool cjk_text_has_wide(const char *s)
{
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0x80) return true;
    return false;
}
