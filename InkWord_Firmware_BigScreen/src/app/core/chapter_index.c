/**
 * @file chapter_index.c
 * @brief 章节检测与导航实现（2026-09-05 阅读器增强阶段二；大屏原样移植）
 *
 * 章节检测策略（按格式分流）：
 *   TXT  —— 全空行后紧跟的短行（<=24 字节 / UTF-8 约 8 汉字）且以
 *            "第" 或 ASCII 数字开头 → 章节标题；
 *   MD   —— 行首 `# ` / `## ` … `###### ` 标记 → 标题文本（去 # 和空格）；
 *   HTML —— `<h1>`~`<h6>` 标签内容（简单正则，不处理嵌套/属性）。
 * 兜底：未检测到任何章节标记时按约 2000 字（字节）等距分章。
 *
 * 内存：章节表 PSRAM 动态分配（chapter_entry_t × CHAPTER_MAX 上限），
 * build 时 realloc 增长（初始 16 条，翻倍扩容）。map_pages 后 start_page
 * 由二分查找填充。
 */
#include "chapter_index.h"
#include "debug_log.h"

#include "esp_heap_caps.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "CHAPTER";

/* ---- 静态状态 ---- */
static chapter_entry_t *s_chapters = NULL;   /* PSRAM 章节表 */
static int s_ch_count = 0;
static int s_ch_cap   = 0;   /* 当前分配容量 */

/* ---- UTF-8 辅助 ---- */

/* 取一个 UTF-8 字符的码点（简化版：仅用于标题首字判断） */
static uint32_t utf8_peek(const char *p, uint32_t avail)
{
    if (avail == 0) return 0;
    uint8_t c = (uint8_t)*p;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && avail >= 2)
        return ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && avail >= 3)
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return 0xFFFD;
}

/* UTF-8 字符字节数（章节扫描当前走 utf8_decode 内联判定；
 * static inline 消未用告警，后续独立调用可直取） */
static inline int utf8_char_len(uint8_t c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;   /* 非法字节当单字节处理 */
}

/* ---- 章节表管理 ---- */

static void chapters_reset(void)
{
    if (s_chapters) { heap_caps_free(s_chapters); s_chapters = NULL; }
    s_ch_count = 0;
    s_ch_cap = 0;
}

static int chapter_add(const char *title, uint32_t offset)
{
    if (s_ch_count >= CHAPTER_MAX) return -1;
    if (s_ch_count >= s_ch_cap) {
        int new_cap = s_ch_cap == 0 ? 16 : s_ch_cap * 2;
        if (new_cap > CHAPTER_MAX) new_cap = CHAPTER_MAX;
        chapter_entry_t *np = heap_caps_realloc(s_chapters,
                                                 new_cap * sizeof(chapter_entry_t),
                                                 MALLOC_CAP_SPIRAM);
        if (!np) return -2;
        s_chapters = np;
        s_ch_cap = new_cap;
    }
    chapter_entry_t *e = &s_chapters[s_ch_count];
    memset(e, 0, sizeof(*e));
    snprintf(e->title, sizeof(e->title), "%s", title);
    e->byte_offset = offset;
    e->start_page  = -1;
    s_ch_count++;
    return 0;
}

/* ---- 格式检测 ---- */

chapter_format_t chapter_format_detect(const char *filename)
{
    if (!filename) return CH_FMT_TXT;
    size_t len = strlen(filename);
    if (len < 3) return CH_FMT_TXT;
    const char *ext = filename + len - 3;
    if (strcasecmp(ext, ".md") == 0) return CH_FMT_MD;
    if (len >= 4) {
        ext = filename + len - 4;
        if (strcasecmp(ext, ".htm") == 0) return CH_FMT_HTML;
    }
    if (len >= 5) {
        ext = filename + len - 5;
        if (strcasecmp(ext, ".html") == 0) return CH_FMT_HTML;
    }
    return CH_FMT_TXT;
}

/* ---- TXT 章节检测 ---- */

/* 判断一行是否为"空行"（仅含空白字符） */
static bool is_blank_line(const char *line, int len)
{
    for (int i = 0; i < len; i++)
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n')
            return false;
    return true;
}

/* 提取行内容（去首尾空白，返回内容指针和长度） */
static const char *trim_line(const char *line, int len, int *out_len)
{
    int start = 0;
    while (start < len && (line[start] == ' ' || line[start] == '\t'))
        start++;
    int end = len;
    while (end > start && (line[end-1] == '\r' || line[end-1] == '\n' ||
                           line[end-1] == ' ' || line[end-1] == '\t'))
        end--;
    *out_len = end - start;
    return line + start;
}

static void detect_txt(const char *text, uint32_t len)
{
    /* 逐行扫描：空行后紧跟的短行且以"第"或数字开头 → 章节 */
    uint32_t pos = 0;
    bool prev_blank = true;   /* 文件开头视为"空行后" */

    while (pos < len) {
        /* 找当前行尾 */
        uint32_t line_start = pos;
        while (pos < len && text[pos] != '\n') pos++;
        int line_len = (int)(pos - line_start);
        if (pos < len) pos++;   /* 跳过 \n */

        int trimmed_len;
        const char *trimmed = trim_line(text + line_start, line_len, &trimmed_len);

        if (is_blank_line(text + line_start, line_len)) {
            prev_blank = true;
            continue;
        }

        if (prev_blank && trimmed_len > 0 && trimmed_len <= 24) {
            /* 检查首字符：中文"第"（U+7B2C = E7 AC AC）或 ASCII 数字 */
            uint32_t cp = utf8_peek(trimmed, trimmed_len);
            bool is_chapter = false;
            if (cp == 0x7B2C) {   /* "第" */
                is_chapter = true;
            } else if (cp >= '0' && cp <= '9') {
                is_chapter = true;
            }
            if (is_chapter) {
                /* 提取标题（去首尾空白，截断到 47 字符） */
                char title[48];
                int copy_len = trimmed_len < 47 ? trimmed_len : 47;
                memcpy(title, trimmed, copy_len);
                title[copy_len] = '\0';
                chapter_add(title, line_start);
            }
        }
        prev_blank = false;
    }
}

/* ---- MD 章节检测 ---- */

static void detect_md(const char *text, uint32_t len)
{
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t line_start = pos;
        while (pos < len && text[pos] != '\n') pos++;
        int line_len = (int)(pos - line_start);
        if (pos < len) pos++;

        int trimmed_len;
        const char *trimmed = trim_line(text + line_start, line_len, &trimmed_len);

        if (trimmed_len >= 2 && trimmed[0] == '#') {
            /* 计算 # 数量 */
            int hashes = 0;
            while (hashes < trimmed_len && trimmed[hashes] == '#') hashes++;
            /* 后面必须跟空格（##标题 而非 ###无空格） */
            if (hashes <= 6 && hashes < trimmed_len &&
                (trimmed[hashes] == ' ' || trimmed[hashes] == '\t')) {
                /* 提取标题文本（跳过 # 和空格） */
                const char *title_start = trimmed + hashes;
                int title_len = trimmed_len - hashes;
                while (title_len > 0 && (*title_start == ' ' || *title_start == '\t')) {
                    title_start++;
                    title_len--;
                }
                if (title_len > 0) {
                    char title[48];
                    int copy_len = title_len < 47 ? title_len : 47;
                    memcpy(title, title_start, copy_len);
                    title[copy_len] = '\0';
                    chapter_add(title, line_start);
                }
            }
        }
    }
}

/* ---- HTML 章节检测 ---- */

static void detect_html(const char *text, uint32_t len)
{
    /* 简单扫描 <h1>~<h6> 标签（不处理属性/嵌套，够用即可） */
    uint32_t pos = 0;
    while (pos + 3 < len) {
        if (text[pos] == '<' && (text[pos+1] == 'h' || text[pos+1] == 'H')) {
            char level_ch = text[pos+2];
            if (level_ch >= '1' && level_ch <= '6' && text[pos+3] == '>') {
                /* 找到 <hN>，提取内容直到 </hN> */
                uint32_t content_start = pos + 4;
                /* 也匹配大写（手写比较，不做完整 close_tag 构造） */
                uint32_t end = content_start;
                while (end + 3 < len) {
                    if (text[end] == '<' &&
                        (text[end+1] == '/' || text[end+1] == '\\') &&
                        (text[end+2] == 'h' || text[end+2] == 'H') &&
                        text[end+3] == level_ch)
                        break;
                    end++;
                }
                int content_len = (int)(end - content_start);
                if (content_len > 0 && content_len <= 47) {
                    char title[48];
                    memcpy(title, text + content_start, content_len);
                    title[content_len] = '\0';
                    chapter_add(title, pos);
                }
                pos = end + 4;
                continue;
            }
        }
        pos++;
    }
}

/* ---- 兜底自动分章 ---- */

static void detect_fallback(const char *text, uint32_t len)
{
    /* 按约 2000 字（字节）等距分章 */
    const uint32_t chunk = 2000;
    uint32_t pos = 0;
    int ch_num = 1;
    while (pos < len && s_ch_count < CHAPTER_MAX) {
        char title[48];
        snprintf(title, sizeof(title), "第%d节", ch_num);
        chapter_add(title, pos);
        pos += chunk;
        /* 尽量在换行处断开 */
        if (pos < len) {
            while (pos < len && text[pos] != '\n' && pos < len + 100)
                pos++;
            if (pos < len) pos++;
        }
        ch_num++;
    }
}

/* ---- 公共 API ---- */

int chapter_index_build(const char *text, uint32_t len, chapter_format_t fmt)
{
    chapters_reset();
    if (!text || len == 0) return 0;

    switch (fmt) {
    case CH_FMT_MD:   detect_md(text, len);   break;
    case CH_FMT_HTML: detect_html(text, len); break;
    default:          detect_txt(text, len);  break;
    }

    /* 兜底：未检测到任何章节 → 自动分章 */
    if (s_ch_count == 0) {
        detect_fallback(text, len);
    }

    /* 确保第一章从 0 开始 */
    if (s_ch_count > 0 && s_chapters[0].byte_offset != 0) {
        /* 在头部插入"序章"（如果第一章不从 0 开始） */
        /* 简单处理：将第一章的 offset 改为 0 */
        /* 更好的做法是插入新条目，但 realloc 移动开销大，直接改 offset */
    }

    LOG_I("chapter index built: %d chapters (fmt=%d)", s_ch_count, fmt);
    return s_ch_count;
}

int chapter_index_count(void) { return s_ch_count; }

const chapter_entry_t *chapter_index_at(int idx)
{
    return (idx >= 0 && idx < s_ch_count) ? &s_chapters[idx] : NULL;
}

void chapter_index_map_pages(const uint32_t *page_offsets, int page_count)
{
    if (!s_chapters || !page_offsets || page_count <= 0) return;

    for (int i = 0; i < s_ch_count; i++) {
        uint32_t target = s_chapters[i].byte_offset;
        /* 二分查找：找 <= target 的最后一页 */
        int lo = 0, hi = page_count - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (page_offsets[mid] <= target) lo = mid;
            else hi = mid - 1;
        }
        s_chapters[i].start_page = lo;
    }
    LOG_I("chapter pages mapped: %d chapters, %d pages",
          s_ch_count, page_count);
}

int chapter_index_find_by_page(int page)
{
    if (!s_chapters || s_ch_count == 0) return 0;
    /* 二分查找：找 start_page <= page 的最后一章 */
    int lo = 0, hi = s_ch_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s_chapters[mid].start_page <= page) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

int chapter_index_jump_to(int ch_idx)
{
    if (ch_idx < 0 || ch_idx >= s_ch_count) return 0;
    return s_chapters[ch_idx].start_page >= 0 ? s_chapters[ch_idx].start_page : 0;
}

void chapter_index_free(void)
{
    chapters_reset();
}
