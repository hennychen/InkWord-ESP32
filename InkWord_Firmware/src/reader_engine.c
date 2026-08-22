/**
 * @file reader_engine.c
 * @brief 阅读模式引擎实现 (PRD 里程碑 P3)
 *
 * 布局：横屏 416x240，内容区 y=32(状态栏)~240；左右边距 8px；
 * 行高 = cell+4px；变宽排版 = 字形墨迹盒水平裁剪 blit + 2px 字距，
 * 空格半角宽（U+3000 全角宽）、缺字画空心占位框（全宽）。
 * 建页表与渲染共用同一 advance/断行逻辑（同起点确定性一致）。
 *
 * 内存：书整本 + 页偏移数组均在 PSRAM（8MB Octal）；书上限 4MB。
 * 进度：NVS "inkword"/{rd_sig(书签名), rd_font, rd_page}，每页渲染即存
 * （翻页频率低，NVS 磨损可忽略；书签名 = 文件长度 ^ 前 16 字节 FNV）。
 */
#include "reader_engine.h"
#include "cjk_font.h"
#include "epd_driver.h"
#include "layout_profile.h" /* Phase 5：档位→字库级映射（默认档/占位页） */
#include "debug_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "nvs.h"

static const char *TAG = "READER";

/* ---- 布局常量（与 main.cpp UI_STATUS_H / epd_gfx 横屏尺寸对应；
 *      Phase 4 去硬编码：可用宽高运行期派生，416x240 下 R_MAX_W=400/R_MAX_H=204 不变） ---- */
#define R_AREA_Y       32     /* 内容区顶 = 状态栏高 */
#define R_MARGIN_X     8      /* 左右边距 */
#define R_MARGIN_TOP   4      /* 内容区上边距 */
#define R_MAX_W        (epd_gfx_width() - 2 * R_MARGIN_X)    /* 400 */
#define R_MAX_H        (epd_gfx_height() - R_AREA_Y - 4)     /* 204 */
#define R_SPACING      2      /* 字距 */
#define R_MAX_BOOK     (4 * 1024 * 1024)   /* 单书上限 4MB */
#define R_DEF_LEVEL    1      /* 静态兜底（MID 档默认 20px）；运行期默认
                              * 按布局档位（SMALL=0 16px / MID=1 / LARGE=2） */

/* ---- 运行时状态 ---- */
static char     *s_book = NULL;        /* 书全文（PSRAM，含 NUL 哨兵） */
static uint32_t  s_book_len = 0;
static uint32_t  s_sig = 0;            /* 书签名（进度校验） */
static uint32_t *s_pages = NULL;       /* 页首字节偏移（PSRAM） */
static int       s_page_n = 0;
static int       s_level = R_DEF_LEVEL;

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

/* ---- 字形墨迹盒列范围（first..last 含端点）；空字形 w=0 ---- */
static void ink_span(const uint8_t *bits, int cell, int stride, int *l, int *w)
{
    int first = -1, last = -1;
    for (int gx = 0; gx < cell && first < 0; gx++) {
        for (int gy = 0; gy < cell; gy++) {
            if (bits[gy * stride + gx / 8] & (0x80 >> (gx % 8))) { first = gx; break; }
        }
    }
    if (first < 0) { *l = 0; *w = 0; return; }
    for (int gx = cell - 1; gx > last; gx--) {
        for (int gy = 0; gy < cell; gy++) {
            if (bits[gy * stride + gx / 8] & (0x80 >> (gx % 8))) { last = gx; break; }
        }
    }
    *l = first; *w = last - first + 1;
}

/* 墨迹盒裁剪 blit：把字形水平裁到 [l, l+w) 再画（变宽排版的渲染半边） */
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

/* ---- 排版 advance：单字符步进宽（建页与渲染共用，保证断行一致） ----
 * 返回 -1 = 换行符；其余 ≥0 步进 px。ink_l/ink_w 出参供渲染 blit。 */
static int glyph_advance(uint32_t cp, int level, const uint8_t **bits_out,
                         int *ink_l, int *ink_w)
{
    int cell = cjk_glyph_cell_size(level);
    if (cp == '\n') return -1;
    const uint8_t *bits = cjk_glyph_lookup_level(cp, level);
    if (!bits) {                        /* 未收录：全宽占位（渲染画框） */
        *bits_out = NULL; *ink_l = 0; *ink_w = cell;
        return cell + R_SPACING;
    }
    int l, w;
    ink_span(bits, cell, cjk_glyph_stride_size(level), &l, &w);
    *bits_out = bits; *ink_l = l; *ink_w = w;
    if (w == 0)                         /* 空白字形：半/全角空格 */
        return cp == 0x3000 ? cell : cell / 2;
    return w + R_SPACING;
}

/* ---- 页表构建（advance 逻辑与渲染严格一致） ---- */
static int build_pages(void)
{
    int cell = cjk_glyph_cell_size(s_level);
    int line_h = cell + 4;
    int max_lines = R_MAX_H / line_h;
    if (max_lines < 1) max_lines = 1;

    if (s_pages) { heap_caps_free(s_pages); s_pages = NULL; }
    uint32_t cap = 256;
    s_pages = heap_caps_malloc(cap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_pages) return -2;

    s_page_n = 1;
    s_pages[0] = 0;
    uint32_t off = 0;
    int line = 0, x = 0;
    while (off < s_book_len) {
        uint32_t cp;
        int n = utf8_next(s_book + off, s_book_len - off, &cp);
        if (n <= 0) break;
        off += (uint32_t)n;
        if (cp == '\r') continue;       /* CRLF 归一 */

        int adv;
        if (cp == '\n') adv = -1;
        else {
            const uint8_t *b; int il, iw;
            adv = glyph_advance(cp, s_level, &b, &il, &iw);
        }
        if (adv < 0) {                  /* 换行 */
            x = 0; line++;
        } else {
            if (x + adv > R_MAX_W && x > 0) { x = 0; line++; }   /* 折行 */
            x += adv;
        }
        if (line >= max_lines && off < s_book_len) {   /* 页满 */
            if ((uint32_t)s_page_n >= cap) {
                cap *= 2;
                uint32_t *np = heap_caps_realloc(s_pages, cap * sizeof(uint32_t),
                                                 MALLOC_CAP_SPIRAM);
                if (!np) return -2;
                s_pages = np;
            }
            s_pages[s_page_n++] = off;
            line = 0; x = 0;
        }
    }
    return 0;
}

/* ---- 书加载 ---- */
static uint32_t book_signature(const char *buf, uint32_t len)
{
    /* FNV-1a over min(len,16) 字节 ^ len：轻量书变更检测 */
    uint32_t h = 2166136261u;
    uint32_t n = len < 16 ? len : 16;
    for (uint32_t i = 0; i < n; i++) { h ^= (uint8_t)buf[i]; h *= 16777619u; }
    return h ^ len;
}

#ifdef INKWORD_DEMO_BOOK
/* 演示书（demo 构建）：含中文/标点/ASCII/换行段的短文，验证分页排版 */
static const char k_demo_book[] =
    "传习录（选）\n"
    "\n"
    "王阳明先生曰：知是行的主意，行是知的功夫；知是行之始，行是知之成。"
    "只说要知，已不曾行；只说要行，已不曾知。\n"
    "\n"
    "又曰：心即理也。天下又有心外之事、心外之理乎？\n"
    "\n"
    "爱问：至善只求诸心，恐于天下事理有不能尽。"
    "先生曰：心即理也。天下又有心外之事、心外之理乎？\n"
    "\n"
    "又问：静时亦觉意思好，才遇事便不同。如何？"
    "先生曰：是徒知静养，而不用克己工夫也。人须在事上磨，方立得住，"
    "方能静亦定、动亦定。\n"
    "\n"
    "—— The quick brown fox jumps over the lazy dog. 0123456789.\n"
    "—— 知行合一，止于至善。Reading on e-ink is easy for the eyes.\n"
    "\n"
    "本篇为内置演示书，验证墨水屏阅读模式的分页、变宽排版与三级字号。"
    "正式书籍请放至 SD 卡 /sdcard/books/ 目录（UTF-8 文本）。\n"
    "\n"
    "山近月远觉月小，便道此山大于月。\n"
    "若人有眼大如天，还见山小月更阔。\n"
    "（王阳明《蔽月山房》）\n";
#endif

static int load_book(void)
{
#ifdef INKWORD_DEMO_BOOK
    s_book_len = (uint32_t)strlen(k_demo_book);
    s_book = heap_caps_malloc(s_book_len + 1, MALLOC_CAP_SPIRAM);
    if (!s_book) return -2;
    memcpy(s_book, k_demo_book, s_book_len + 1);
    LOG_I("demo book loaded (%u bytes)", s_book_len);
#else
    /* SD 书目录下第一个 .txt */
    DIR *d = opendir("/sdcard/books");
    if (!d) {
        LOG_W("no /sdcard/books directory");
        return -1;
    }
    /* 272 = 前缀 15 + d_name 上限 255 + NUL，最坏情况也不截断 */
    char path[272] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl > 4 && strcasecmp(e->d_name + nl - 4, ".txt") == 0) {
            snprintf(path, sizeof(path), "/sdcard/books/%s", e->d_name);
            break;
        }
    }
    closedir(d);
    if (!path[0]) {
        LOG_W("no .txt book under /sdcard/books");
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > R_MAX_BOOK) {
        LOG_W("book size %ld out of range (0, %d]", sz, R_MAX_BOOK);
        fclose(f);
        return -1;
    }
    s_book_len = (uint32_t)sz;
    s_book = heap_caps_malloc(s_book_len + 1, MALLOC_CAP_SPIRAM);
    if (!s_book) { fclose(f); return -2; }
    if (fread(s_book, 1, s_book_len, f) != s_book_len) {
        fclose(f); heap_caps_free(s_book); s_book = NULL; return -1;
    }
    fclose(f);
    s_book[s_book_len] = '\0';
    /* 跳 UTF-8 BOM */
    if (s_book_len >= 3 && (uint8_t)s_book[0] == 0xEF &&
        (uint8_t)s_book[1] == 0xBB && (uint8_t)s_book[2] == 0xBF) {
        memmove(s_book, s_book + 3, s_book_len - 2);
        s_book_len -= 3;
    }
    LOG_I("book loaded: %s (%u bytes)", path, (unsigned)s_book_len);
#endif
    s_sig = book_signature(s_book, s_book_len);
    return 0;
}

/* ---- NVS 进度 ---- */
static void progress_save(int page)
{
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "rd_sig", s_sig);
    nvs_set_u8(h, "rd_font", (uint8_t)s_level);
    nvs_set_u32(h, "rd_page", (uint32_t)page);
    nvs_commit(h);
    nvs_close(h);
}

static void font_level_restore(void)
{
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t sig = 0;
    uint8_t fnt = (uint8_t)layout_profile_get()->reader_level;  /* 档位默认 */
    bool hit = nvs_get_u32(h, "rd_sig", &sig) == ESP_OK && sig == s_sig;
    if (hit && nvs_get_u8(h, "rd_font", &fnt) == ESP_OK && fnt >= CJK_FONT_LEVELS)
        fnt = R_DEF_LEVEL;
    nvs_close(h);
    if (hit) s_level = fnt;
}

/* ---- 公共 API ---- */
int reader_engine_init(void)
{
    int r = load_book();
    if (r != 0) return r;
    font_level_restore();
    if (build_pages() != 0) {
        heap_caps_free(s_book); s_book = NULL;
        return -2;
    }
    LOG_I("reader ready: %u bytes, level=%d (%dpx), %d pages",
          (unsigned)s_book_len, s_level, cjk_glyph_cell_size(s_level), s_page_n);
    return 0;
}

bool reader_ready(void)          { return s_book && s_page_n > 0; }
int reader_page_count(void)      { return reader_ready() ? s_page_n : 0; }
int reader_font_level(void)      { return s_level; }

int reader_progress_page(void)
{
    nvs_handle_t h;
    if (nvs_open("inkword", NVS_READONLY, &h) != ESP_OK) return -1;
    uint32_t sig = 0, page = 0;
    int r = (nvs_get_u32(h, "rd_sig", &sig) == ESP_OK && sig == s_sig &&
             nvs_get_u32(h, "rd_page", &page) == ESP_OK &&
             (int32_t)page < s_page_n) ? (int)page : -1;
    nvs_close(h);
    return r;
}

int reader_font_step(int dir, int cur_page)
{
    if (!reader_ready()) return cur_page;
    uint32_t anchor = (cur_page >= 0 && cur_page < s_page_n)
                      ? s_pages[cur_page] : 0;
    s_level = (s_level + dir + CJK_FONT_LEVELS) % CJK_FONT_LEVELS;
    if (build_pages() != 0) return 0;   /* 重建失败回首页（极小概率） */
    /* 就近定位：页首偏移 <= anchor 的最后一页 */
    int lo = 0, hi = s_page_n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s_pages[mid] <= anchor) lo = mid; else hi = mid - 1;
    }
    LOG_I("font level -> %d (%dpx), page %d -> %d (anchor %u)",
          s_level, cjk_glyph_cell_size(s_level), cur_page, lo, (unsigned)anchor);
    return lo;
}

void reader_render_page(int page)
{
    if (!reader_ready()) { reader_render_placeholder(); return; }
    if (page < 0) page = 0;
    if (page >= s_page_n) page = s_page_n - 1;

    int cell = cjk_glyph_cell_size(s_level);
    int stride = cjk_glyph_stride_size(s_level);
    int line_h = cell + 4;

    uint32_t off = s_pages[page];
    uint32_t end = (page + 1 < s_page_n) ? s_pages[page + 1] : s_book_len;
    int x = R_MARGIN_X, y = R_AREA_Y + R_MARGIN_TOP;

    while (off < end) {
        uint32_t cp;
        int n = utf8_next(s_book + off, end - off, &cp);
        if (n <= 0) break;
        off += (uint32_t)n;
        if (cp == '\r') continue;

        if (cp == '\n') { x = R_MARGIN_X; y += line_h; continue; }

        const uint8_t *bits; int il, iw;
        int adv = glyph_advance(cp, s_level, &bits, &il, &iw);
        if (x + adv > R_MAX_W && x > R_MARGIN_X) {   /* 折行（与建页一致） */
            x = R_MARGIN_X; y += line_h;
        }
        if (bits && iw > 0) {
            blit_trimmed(x, y, bits, cell, stride, il, iw, EPD_GFX_BLACK);
        } else if (!bits) {           /* 缺字占位框 */
            epd_gfx_draw_rect(x + 1, y + 1, cell - 2, cell - 2, EPD_GFX_BLACK);
        }
        x += adv;
    }

    /* 页脚进度（右下角，小字）：80 = 9pt "999/999" 数字串预估宽含余量
     * （实测右缘留白约 40px；换字号档时按 text_bounds 实测调整） */
    char buf[24];
    snprintf(buf, sizeof(buf), "%d/%d", page + 1, s_page_n);
    epd_gfx_draw_text(epd_gfx_width() - R_MARGIN_X - 80,
                      epd_gfx_height() - 6, buf, EPD_GFX_BLACK, 1);

    progress_save(page);
}

/* 字符数（UTF-8 逐字计数；ASCII 与中文混排时 strlen/3 会算错） */
static int str_cells(const char *s)
{
    int n = 0;
    while (*s) {
        uint32_t cp;
        int k = utf8_next(s, 4, &cp);
        if (k <= 0) break;
        s += k;
        n++;
    }
    return n;
}

/* 点阵整字绘制（占位页中文提示：GFX 内置字体无中文，ASCII 与
 * 中文同在字库内等宽步进；返回绘制宽度供居中计算） */
static int draw_str_cells(int x, int y, int level, const char *s)
{
    const int cell = cjk_glyph_cell_size(level);
    const int step = cell + 2;
    int cx = x;
    while (*s) {
        uint32_t cp;
        int n = utf8_next(s, 4, &cp);
        if (n <= 0) break;
        s += n;
        const uint8_t *bits = cjk_glyph_lookup_level(cp, level);
        if (bits)
            epd_gfx_draw_bitmap(cx, y, cell, cell, bits, EPD_GFX_BLACK);
        cx += step;
    }
    return cx - x;
}

void reader_render_placeholder(void)
{
    /* 内置 GFX 字体仅 ASCII：中文提示用档位大字级点阵等宽绘制（MID 档
     * 24px 与旧硬宏一致，视觉零变化；无书时无 NVS 字号可用，取档位
     * 大字级而非正文默认级——占位提示属大字场景），水平居中 */
    static const char *l1 = "阅读模式";
    static const char *l2 = "未找到书籍";
    static const char *l3 = "请将UTF-8文本放入";
    static const char *l4 = "SD卡books目录后重启";
    int level = layout_profile_get()->quote_level;
    /* 档位大字级按可用高度自适应降级（2026-08-22 SMALL 档配套）：
     * 2.7" 176px 高下 24px 底缘 222px/20px 198px 均溢出，降至 16px
     * （174px）才放得下；MID(240px)/LARGE 验算不降级，视觉零变化 */
    while (level > 0 &&
           R_AREA_Y + 36 + 5 * (cjk_glyph_cell_size(level) + 2) +
               cjk_glyph_cell_size(level) > epd_gfx_height())
        level--;
    const int step = cjk_glyph_cell_size(level) + 2;
    int y = R_AREA_Y + 36;

    int x = (epd_gfx_width() - str_cells(l1) * step) / 2;
    draw_str_cells(x, y, level, l1);
    x = (epd_gfx_width() - str_cells(l2) * step) / 2;
    draw_str_cells(x, y + 2 * step, level, l2);
    x = (epd_gfx_width() - str_cells(l3) * step) / 2;
    draw_str_cells(x, y + 4 * step, level, l3);
    x = (epd_gfx_width() - str_cells(l4) * step) / 2;
    draw_str_cells(x, y + 5 * step, level, l4);
    LOG_I("reader placeholder rendered");
}
