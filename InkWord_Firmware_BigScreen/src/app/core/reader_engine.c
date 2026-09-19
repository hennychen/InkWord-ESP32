/**
 * @file reader_engine.c
 * @brief 大屏阅读引擎实现（小屏 reader_engine 移植 + 排版适配）
 *
 * 布局（XLARGE 1920x1080）：正文区 x ∈ [RD_MARGIN_X, W-RD_MARGIN_X]，
 * y ∈ [RD_STATUS_H+80, H-RD_BOTTOM_BAR-40]；行高 = cell × 行距档/10
 * （1.5/1.6/1.8 倍）；段距 = 0.8 行高（连续空行折叠为一次）；
 * 变宽排版 = 字形墨迹盒水平裁剪 blit + 2px 字距，空格半角宽
 * （U+3000 全角宽）、缺字画空心占位框（全宽）。
 * 建页表与渲染共用同一 advance/换行折叠逻辑（consume_newlines
 * 单点，同起点确定性一致）。
 *
 * 内存：书整本 + 页偏移数组均在 PSRAM（8MB Octal；词池 3.2MB +
 * fb 1MB + 画布 0.26MB 共存预算下单书上限 2MB）。
 * 进度：NVS "inkword"/{rd_sig(书签名), rd_font, rd_lh, rd_page}，
 * 每页保存由 reader_page 显式调（翻页频率低，NVS 磨损可忽略；
 * 书签名 = 文件长度 ^ 前 16 字节 FNV）。
 */
#include "reader_engine.h"
#include "cjk_font.h"
#include "epd_gfx.h"
#include "layout_profile.h"   /* 档位默认字号级（XLARGE=3） */
#include "chapter_index.h"    /* 章节检测与导航 */
#include "bookmark_mgr.h"     /* 书签管理 */
#include "storage_manager.h"  /* SPIFFS 书库 */
#include "debug_log.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "nvs.h"
#include "settings_keys.h"    /* NVS 键权威表 */

static const char *TAG = "READER";

/* ---- 布局常量（正文区；状态栏/提示栏几何见 reader_engine.h） ---- */
#define RD_TOP_GAP      80     /* 正文上边距（状态栏之下） */
#define RD_BOTTOM_GAP   40     /* 提示栏上留白 */
#define RD_SPACING      2      /* 字距（与小屏一致） */
#define RD_AREA_W       (epd_gfx_width() - 2 * RD_MARGIN_X)
#define RD_AREA_TOP     (RD_STATUS_H + RD_TOP_GAP)
#define RD_AREA_H       (epd_gfx_height() - RD_AREA_TOP - RD_BOTTOM_BAR - RD_BOTTOM_GAP)

#define RD_MAX_BOOK     (2 * 1024 * 1024)   /* 单书上限 2MB（PSRAM 预算） */
#define RD_LEVEL_MIN    1     /* 大屏正文五档 20/24/32/40/48px（2026-09-17
                                * UI 重设计扩级：六级字库落地后 40/48px
                                * 级接入循环；0=16px 大屏不用） */
#define RD_LEVEL_MAX    5
#define RD_LEVELS       (RD_LEVEL_MAX - RD_LEVEL_MIN + 1)
#define RD_LH_MIN       15    /* 行距档 ×0.1：15/16/18 */
#define RD_LH_DEF       16
#define RD_LH_MAX       18
#define RD_LH_STEPS     3

/* ---- 内嵌演示书（embed_data.S .incbin，app/data/demo_book.txt） ---- */
extern const uint8_t _binary_app_data_demo_book_txt_start[];
extern const uint8_t _binary_app_data_demo_book_txt_end[];

/* ---- 运行时状态 ---- */
static char     *s_book = NULL;        /* 书全文（PSRAM，含 NUL 哨兵） */
static uint32_t  s_book_len = 0;
static uint32_t  s_sig = 0;            /* 书签名（进度校验） */
static uint32_t *s_pages = NULL;       /* 页首字节偏移（PSRAM） */
static int       s_page_n = 0;
static int       s_level = 4;          /* 字号级（XLARGE 默认 40px） */
static int       s_lh = RD_LH_DEF;     /* 行距档 */
static char      s_title[STORAGE_BOOK_NAME_MAX] = "";  /* 书名（显示用） */
static chapter_format_t s_fmt = CH_FMT_TXT;           /* 章节检测策略 */

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
    /* 字库六级上限 48px：ts*cell 最大 6*48=288B。旧容量 32*4=128B 为
     * 四级化残留，40/48px 级宽字形 memset 溢出（同 cjk_text.c 修复，
     * 2026-09-17） */
    uint8_t tmp[6 * 48];
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
        return cell + RD_SPACING;
    }
    int l, w;
    ink_span(bits, cell, cjk_glyph_stride_size(level), &l, &w);
    *bits_out = bits; *ink_l = l; *ink_w = w;
    if (w == 0)                         /* 空白字形：半/全角空格 */
        return cp == 0x3000 ? cell : cell / 2;
    return w + RD_SPACING;
}

/* ---- 换行序列消费（建页/渲染同源单点）：首 '\n' 推行距，连续
 *      '\n'（空行）折叠为一次段距；返回消费后的偏移。
 *      pos = 当前 '\n' 已被消费后的位置 ---- */
static uint32_t consume_newlines(uint32_t pos, int y, int line_h,
                                 int para_gap, int *out_y)
{
    int ny = y + line_h;
    bool para = false;
    while (pos < s_book_len) {
        uint32_t cp;
        int n = utf8_next(s_book + pos, s_book_len - pos, &cp);
        if (n <= 0) break;
        if (cp == '\r') { pos += (uint32_t)n; continue; }
        if (cp == '\n') { pos += (uint32_t)n; para = true; continue; }
        break;
    }
    if (para) ny += para_gap;           /* 连续空行折叠：段距只加一次 */
    *out_y = ny;
    return pos;
}

/* 行高派生（字号级 × 行距档） */
static int line_height(int cell)
{
    return cell * s_lh / 10;
}

/* ---- 页表构建（advance/换行折叠与渲染严格一致） ----
 * 坐标系：相对正文区（x/y 从 0 起），渲染时加绝对偏移。
 * 页满判定：下一行顶 y + cell 超出区高。
 * 段首孤行保护（widow）：页满时若本页只放了当前段的第一行，
 * 新页回退到段首（整段挪新页）；段已放 ≥2 行则正常切页。 */
static int build_pages(void)
{
    int cell = cjk_glyph_cell_size(s_level);
    int line_h = line_height(cell);
    int para_gap = line_h * 4 / 5;      /* 段距 = 0.8 倍行高 */
    int area_w = RD_AREA_W, area_h = RD_AREA_H;
    if (area_w <= 0 || area_h < cell) { /* 几何异常（未 init？）拒绝建页 */
        LOG_E("area geometry invalid: %dx%d", area_w, area_h);
        return -2;
    }

    if (s_pages) { heap_caps_free(s_pages); s_pages = NULL; }
    uint32_t cap = 256;
    s_pages = heap_caps_malloc(cap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_pages) return -2;

    s_page_n = 1;
    s_pages[0] = 0;
    uint32_t off = 0;
    uint32_t page_start = 0;   /* 当前页首偏移 */
    uint32_t para_start = 0;   /* 当前段首偏移 */
    int x = 0, y = 0;
    int para_lines = 0;        /* 当前段已放行数（折行/换行均计） */
    uint32_t new_off = 0;      /* 页满时的新页起点（两场景共用出口） */

    while (off < s_book_len) {
        uint32_t ch_start = off;
        uint32_t cp;
        int n = utf8_next(s_book + off, s_book_len - off, &cp);
        if (n <= 0) break;
        off += (uint32_t)n;
        if (cp == '\r') continue;       /* CRLF 归一 */

        if (cp == '\n') {
            uint32_t cur_para_start = para_start;   /* 本段首（更新前留档） */
            off = consume_newlines(off, y, line_h, para_gap, &y);
            x = 0;
            para_lines++;
            if (y + cell > area_h && off < s_book_len) {
                new_off = off;
                /* 本段仅放 1 行且段首不在本页页首 → 整段挪新页 */
                if (cur_para_start > page_start && para_lines <= 1)
                    new_off = cur_para_start;
                goto page_break;
            }
            para_start = off;           /* 下一段起点 */
            continue;
        }

        const uint8_t *b; int il, iw;
        int adv = glyph_advance(cp, s_level, &b, &il, &iw);
        if (x + adv > area_w && x > 0) {             /* 折行 */
            x = 0; y += line_h; para_lines++;
        }
        x += adv;

        if (y + cell > area_h && off < s_book_len) {
            new_off = ch_start;         /* 该字挪新页首 */
            /* 段的第一行尚未放任何完整行 → 整段挪新页 */
            if (para_start > page_start && para_lines == 0)
                new_off = para_start;
            goto page_break;
        }
        continue;

    page_break:
        if ((uint32_t)s_page_n >= cap) {
            cap *= 2;
            uint32_t *np = heap_caps_realloc(s_pages, cap * sizeof(uint32_t),
                                             MALLOC_CAP_SPIRAM);
            if (!np) return -2;
            s_pages = np;
        }
        s_pages[s_page_n++] = new_off;
        page_start = new_off;
        off = new_off;                  /* 回退场景重扫该段（段 < 1 页，
                                         * 重扫有界不死循环） */
        x = 0; y = 0; para_lines = 0;
        para_start = new_off;
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

static int load_demo(void)
{
    uint32_t len = (uint32_t)(_binary_app_data_demo_book_txt_end -
                              _binary_app_data_demo_book_txt_start);
    if (len == 0) return -1;
    s_book = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!s_book) return -2;
    memcpy(s_book, _binary_app_data_demo_book_txt_start, len);
    s_book[len] = '\0';
    s_book_len = len;
    snprintf(s_title, sizeof(s_title), "内置演示书");
    s_fmt = CH_FMT_TXT;
    LOG_I("demo book loaded (%u bytes)", (unsigned)s_book_len);
    return 0;
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_W("open failed: %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > RD_MAX_BOOK) {
        LOG_W("book size %ld out of range (0, %d]", sz, RD_MAX_BOOK);
        fclose(f);
        return -1;
    }
    s_book = heap_caps_malloc((uint32_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!s_book) { fclose(f); return -2; }
    if (fread(s_book, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); heap_caps_free(s_book); s_book = NULL; return -1;
    }
    fclose(f);
    s_book_len = (uint32_t)sz;
    s_book[s_book_len] = '\0';
    /* 跳 UTF-8 BOM */
    if (s_book_len >= 3 && (uint8_t)s_book[0] == 0xEF &&
        (uint8_t)s_book[1] == 0xBB && (uint8_t)s_book[2] == 0xBF) {
        memmove(s_book, s_book + 3, s_book_len - 2);
        s_book_len -= 3;
    }
    /* 书名 = 文件名去扩展名（显示/状态栏用） */
    const char *slash = strrchr(path, '/');
    snprintf(s_title, sizeof(s_title), "%s", slash ? slash + 1 : path);
    char *dot = strrchr(s_title, '.');
    if (dot && dot > s_title) *dot = '\0';
    s_fmt = chapter_format_detect(path);
    LOG_I("book loaded: %s (%u bytes, fmt=%d)", path, (unsigned)s_book_len, s_fmt);
    return 0;
}

/* 释放旧书（load_book/load_demo 前置） */
static void release_old(void)
{
    if (s_book) { heap_caps_free(s_book); s_book = NULL; }
    if (s_pages) { heap_caps_free(s_pages); s_pages = NULL; }
    s_book_len = 0; s_page_n = 0; s_sig = 0;
    chapter_index_free();
}

/* 加载后置：签名 + 偏好恢复 + 建页 + 章节索引 + 书签（init/load 共用） */
static int post_load(void)
{
    s_sig = book_signature(s_book, s_book_len);

    /* 进度键按书隔离（scope = 签名 hex 低 28 位；rd_page_(8)+7=15
     * 恰达 NVS 键上限）——书架换书互不覆盖进度/字号/行距（计划
     * 验收项“书架换书进度隔离”；内容相同的书共享进度，语义自洽） */
    char scope[8];
    snprintf(scope, sizeof(scope), "%07x", (unsigned)(s_sig & 0x0FFFFFFFu));
    reader_set_progress_scope(scope);

    /* 字号/行距偏好恢复（书签名匹配才生效；默认档位值 + 大屏钳位） */
    s_level = layout_profile_get()->reader_level;
    if (s_level < RD_LEVEL_MIN) s_level = RD_LEVEL_MIN;
    if (s_level > RD_LEVEL_MAX) s_level = RD_LEVEL_MAX;
    s_lh = RD_LH_DEF;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t sig = 0;
        if (nvs_get_u32(h, NVS_KEY_RD_SIG, &sig) == ESP_OK && sig == s_sig) {
            uint8_t fnt = 0, lh = 0;
            if (nvs_get_u8(h, NVS_KEY_RD_FONT, &fnt) == ESP_OK &&
                fnt >= RD_LEVEL_MIN && fnt <= RD_LEVEL_MAX)
                s_level = fnt;
            if (nvs_get_u8(h, NVS_KEY_RD_LH, &lh) == ESP_OK &&
                lh >= RD_LH_MIN && lh <= RD_LH_MAX)
                s_lh = lh;
        }
        nvs_close(h);
    }

    if (build_pages() != 0) {
        heap_caps_free(s_book); s_book = NULL;
        s_book_len = 0; s_page_n = 0;
        return -2;
    }
    chapter_index_build(s_book, s_book_len, s_fmt);
    chapter_index_map_pages(s_pages, s_page_n);
    bookmark_mgr_clear();
    bookmark_mgr_load(s_sig);
    LOG_I("reader ready: %u bytes, level=%d (%dpx), lh=%d.%d, %d pages",
          (unsigned)s_book_len, s_level, cjk_glyph_cell_size(s_level),
          s_lh / 10, s_lh % 10, s_page_n);
    return 0;
}

/* path = NULL：自动探测（SPIFFS 首书 → 演示书回退） */
static int load_book(const char *path)
{
    if (path && path[0]) return load_file(path);
    char names[1][STORAGE_BOOK_NAME_MAX];
    if (storage_books_list(names, 1) > 0) {
        char p[STORAGE_BOOK_NAME_MAX + 24];
        storage_book_path(p, sizeof(p), names[0]);
        return load_file(p);
    }
    return load_demo();
}

/* ---- NVS 进度 ---- */
/* 卡组隔离 scope（小屏 v1.3 T3.1 同款；大屏暂用默认组，机制保留）：
 * 非空时键名 = 基名+后缀（rd_page_xxx） */
static char s_scope[8] = "";

static const char *rd_key(const char *base)
{
    static char key[16];   /* NVS 键名上限 15+NUL */
    snprintf(key, sizeof(key), "%s%s", base, s_scope);
    return key;
}

void reader_set_progress_scope(const char *scope)
{
    if (!scope) scope = "";
    snprintf(s_scope, sizeof(s_scope), "%s", scope);
}

void reader_engine_save_progress(int page)
{
    if (!reader_ready()) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, rd_key(NVS_KEY_RD_SIG), s_sig);
    nvs_set_u8(h, rd_key(NVS_KEY_RD_FONT), (uint8_t)s_level);
    nvs_set_u8(h, rd_key(NVS_KEY_RD_LH), (uint8_t)s_lh);
    nvs_set_u32(h, rd_key(NVS_KEY_RD_PAGE), (uint32_t)page);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- 公共 API ---- */
int reader_engine_init(void)
{
    int r = load_book(NULL);
    if (r != 0) return r;
    return post_load();
}

bool reader_ready(void)          { return s_book && s_page_n > 0; }
int reader_page_count(void)      { return reader_ready() ? s_page_n : 0; }
int reader_font_level(void)      { return reader_ready() ? s_level : RD_LEVEL_MAX; }
int reader_line_spacing(void)    { return s_lh; }
const char *reader_engine_book_title(void) { return s_title; }
uint32_t reader_engine_get_signature(void) { return s_sig; }
const char *reader_engine_get_text(void)   { return s_book; }
uint32_t reader_engine_get_text_len(void)  { return s_book_len; }

int reader_progress_page(void)
{
    if (!reader_ready()) return -1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    uint32_t sig = 0, page = 0;
    int r = (nvs_get_u32(h, rd_key(NVS_KEY_RD_SIG), &sig) == ESP_OK && sig == s_sig &&
             nvs_get_u32(h, rd_key(NVS_KEY_RD_PAGE), &page) == ESP_OK &&
             (int32_t)page < s_page_n) ? (int)page : -1;
    nvs_close(h);
    return r;
}

/* 切级共用：按 anchor（页首偏移）在新页表就近定位页码 */
static int relocate_by_anchor(uint32_t anchor)
{
    int lo = 0, hi = s_page_n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s_pages[mid] <= anchor) lo = mid; else hi = mid - 1;
    }
    return lo;
}

int reader_font_step(int dir, int cur_page)
{
    if (!reader_ready()) return cur_page;
    uint32_t anchor = (cur_page >= 0 && cur_page < s_page_n)
                      ? s_pages[cur_page] : 0;
    s_level = (s_level - RD_LEVEL_MIN + dir + RD_LEVELS) % RD_LEVELS + RD_LEVEL_MIN;
    if (build_pages() != 0) return 0;   /* 重建失败回首页（极小概率） */
    int lo = relocate_by_anchor(anchor);
    LOG_I("font level -> %d (%dpx), page %d -> %d",
          s_level, cjk_glyph_cell_size(s_level), cur_page, lo);
    return lo;
}

int reader_spacing_step(int dir, int cur_page)
{
    if (!reader_ready()) return cur_page;
    uint32_t anchor = (cur_page >= 0 && cur_page < s_page_n)
                      ? s_pages[cur_page] : 0;
    static const int lh_steps[RD_LH_STEPS] = { RD_LH_MIN, RD_LH_DEF, RD_LH_MAX };
    int i = 0;
    while (i < RD_LH_STEPS && lh_steps[i] != s_lh) i++;
    if (i >= RD_LH_STEPS) i = 0;                /* 未知档回最小档 */
    i = (i + dir % RD_LH_STEPS + RD_LH_STEPS) % RD_LH_STEPS;
    s_lh = lh_steps[i];
    if (build_pages() != 0) return 0;
    int lo = relocate_by_anchor(anchor);
    LOG_I("line spacing -> %d.%d, page %d -> %d",
          s_lh / 10, s_lh % 10, cur_page, lo);
    return lo;
}

void reader_render_body(int page)
{
    if (!reader_ready()) return;
    if (page < 0) page = 0;
    if (page >= s_page_n) page = s_page_n - 1;

    int cell = cjk_glyph_cell_size(s_level);
    int stride = cjk_glyph_stride_size(s_level);
    int line_h = line_height(cell);
    int para_gap = line_h * 4 / 5;

    uint32_t off = s_pages[page];
    uint32_t end = (page + 1 < s_page_n) ? s_pages[page + 1] : s_book_len;
    int x = 0;                            /* 相对正文区左边距（建页同系） */
    int y = RD_AREA_TOP;                  /* 绝对坐标（渲染侧专属） */

    while (off < end) {
        uint32_t cp;
        int n = utf8_next(s_book + off, end - off, &cp);
        if (n <= 0) break;
        off += (uint32_t)n;
        if (cp == '\r') continue;

        if (cp == '\n') {                 /* 换行/段距折叠（与建页同源） */
            int ry = y - RD_AREA_TOP, ny = ry;
            off = consume_newlines(off, ry, line_h, para_gap, &ny);
            y = RD_AREA_TOP + ny;
            x = 0;
            continue;
        }

        const uint8_t *bits; int il, iw;
        int adv = glyph_advance(cp, s_level, &bits, &il, &iw);
        if (x + adv > RD_AREA_W && x > 0) {   /* 折行（与建页一致） */
            x = 0; y += line_h;
        }
        if (bits && iw > 0) {
            blit_trimmed(RD_MARGIN_X + x, y, bits, cell, stride, il, iw,
                         EPD_GFX_BLACK);
        } else if (!bits) {                   /* 缺字占位框 */
            epd_gfx_draw_rect(RD_MARGIN_X + x + 1, y + 1, cell - 2, cell - 2,
                              EPD_GFX_BLACK);
        }
        x += adv;
    }
}

int reader_engine_load_book(const char *path)
{
    release_old();
    int r = load_book(path);
    if (r != 0) return r;
    return post_load();
}

int reader_engine_load_demo(void)
{
    release_old();
    int r = load_demo();
    if (r != 0) return r;
    return post_load();
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

/* 点阵整字绘制（占位页中文提示：GFX 内置字体无中文；等宽步进，
 * 返回绘制宽度供居中计算） */
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
    /* 无可用书（演示书也加载失败，如内存不足）：档位大字级居中提示 */
    static const char *l1 = "阅读模式";
    static const char *l2 = "书籍加载失败";
    static const char *l3 = "请重启设备重试";
    int level = layout_profile_get()->quote_level;
    const int step = cjk_glyph_cell_size(level) + 2;
    int y = RD_STATUS_H + 120;

    int x = (epd_gfx_width() - str_cells(l1) * step) / 2;
    draw_str_cells(x, y, level, l1);
    x = (epd_gfx_width() - str_cells(l2) * step) / 2;
    draw_str_cells(x, y + 2 * step, level, l2);
    x = (epd_gfx_width() - str_cells(l3) * step) / 2;
    draw_str_cells(x, y + 4 * step, level, l3);
    LOG_I("reader placeholder rendered");
}

uint32_t reader_engine_page_offset(int page)
{
    if (!reader_ready() || page < 0 || page >= s_page_n) return 0;
    return s_pages[page];
}

const uint32_t *reader_engine_page_offsets(void)
{
    return reader_ready() ? s_pages : NULL;
}
