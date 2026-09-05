/**
 * @file book_format.c
 * @brief 多格式书籍解析实现（2026-09-05 阅读器增强阶段五）
 *
 * TXT 直通（CRLF 归一）；Markdown 去标记（保留标题文本供章节检测）；
 * HTML 去标签（br/p/h 转换行，实体解码）。清洗后纯文本入 PSRAM，
 * 上限 4MB（与 reader_engine R_MAX_BOOK 一致）。
 *
 * 当前实现为轻量级：HTML 仅处理干净子集（不嵌套/不属性复杂标签），
 * 满足常见 epub 导出 HTML 和网页另存为场景。
 */
#include "book_format.h"
#include "debug_log.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "BOOKFMT";

#define MAX_BOOK_SIZE (4 * 1024 * 1024)   /* 4MB 上限 */

book_format_t book_format_detect(const char *filename)
{
    if (!filename) return BOOK_FMT_TXT;
    size_t len = strlen(filename);
    if (len < 4) return BOOK_FMT_TXT;
    const char *ext = filename + len - 4;
    if (strcasecmp(ext, ".md") == 0) return BOOK_FMT_MD;
    if (strcasecmp(ext, ".htm") == 0) return BOOK_FMT_HTML;
    if (len >= 5 && strcasecmp(filename + len - 5, ".html") == 0)
        return BOOK_FMT_HTML;
    return BOOK_FMT_TXT;
}

/* ---- TXT 清洗：仅 CRLF → LF ---- */
static uint32_t clean_txt(const char *src, uint32_t len, char *dst, uint32_t dst_cap)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < len && out < dst_cap - 1; i++) {
        if (src[i] == '\r') {
            /* CRLF → LF; 孤立 CR → LF */
            if (i + 1 < len && src[i + 1] == '\n') continue;  /* 跳过 CR，下一轮 LF 自然写入 */
            dst[out++] = '\n';
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
    return out;
}

/* ---- Markdown 清洗 ---- */
static uint32_t clean_md(const char *src, uint32_t len, char *dst, uint32_t dst_cap)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < len && out < dst_cap - 1; i++) {
        char c = src[i];
        /* 行首处理 */
        bool line_start = (i == 0 || src[i - 1] == '\n');
        if (line_start) {
            /* 跳过 # 标题标记（保留文本） */
            if (c == '#') {
                while (i < len && (src[i] == '#' || src[i] == ' ')) i++;
                if (i >= len) break;
                c = src[i];
            }
            /* 跳过列表标记 "- " / "* " / "+ " / "1. " */
            if ((c == '-' || c == '*' || c == '+') && i + 1 < len && src[i + 1] == ' ') {
                i++;  /* 跳过标记符 */
                continue;
            }
            if (isdigit((unsigned char)c)) {
                uint32_t j = i;
                while (j < len && isdigit((unsigned char)src[j])) j++;
                if (j < len && src[j] == '.' && j + 1 < len && src[j + 1] == ' ') {
                    i = j + 1;  /* 跳过 "1. " */
                    continue;
                }
            }
        }
        /* 行内去标记 */
        if (c == '*' || c == '_' || c == '`') {
            /* 跳过单个标记字符（粗体/斜体/代码） */
            continue;
        }
        /* CRLF 归一 */
        if (c == '\r') {
            if (i + 1 < len && src[i + 1] == '\n') continue;
            dst[out++] = '\n';
        } else {
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
    return out;
}

/* ---- HTML 实体解码（常见命名实体 + 数字实体） ---- */
static int decode_entity(const char *src, uint32_t len, char *out, size_t out_sz)
{
    /* src 指向 '&' 之后 */
    if (len == 0) return 0;
    /* 数字实体 &#123; 或 &#x1A; */
    if (src[0] == '#') {
        uint32_t cp = 0;
        int i = 1;
        bool hex = false;
        if (i < (int)len && (src[i] == 'x' || src[i] == 'X')) { hex = true; i++; }
        while (i < (int)len && src[i] != ';') {
            if (hex) {
                if (src[i] >= '0' && src[i] <= '9') cp = cp * 16 + (src[i] - '0');
                else if (src[i] >= 'a' && src[i] <= 'f') cp = cp * 16 + (src[i] - 'a' + 10);
                else if (src[i] >= 'A' && src[i] <= 'F') cp = cp * 16 + (src[i] - 'A' + 10);
                else return 0;
            } else {
                if (src[i] >= '0' && src[i] <= '9') cp = cp * 10 + (src[i] - '0');
                else return 0;
            }
            i++;
        }
        if (i >= (int)len || src[i] != ';') return 0;
        int consumed = i + 1;  /* 含 ';' */
        /* UTF-8 编码 */
        if (cp < 0x80 && out_sz >= 1) { out[0] = (char)cp; return consumed; }
        if (cp < 0x800 && out_sz >= 2) {
            out[0] = (char)(0xC0 | (cp >> 6));
            out[1] = (char)(0x80 | (cp & 0x3F));
            return consumed;
        }
        if (cp < 0x10000 && out_sz >= 3) {
            out[0] = (char)(0xE0 | (cp >> 12));
            out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (char)(0x80 | (cp & 0x3F));
            return consumed;
        }
        if (cp < 0x110000 && out_sz >= 4) {
            out[0] = (char)(0xF0 | (cp >> 18));
            out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[3] = (char)(0x80 | (cp & 0x3F));
            return consumed;
        }
        return 0;
    }
    /* 命名实体 */
    struct { const char *name; const char *repl; int repl_len; } entities[] = {
        { "amp;",  "&",  1 },
        { "lt;",   "<",  1 },
        { "gt;",   ">",  1 },
        { "quot;", "\"", 1 },
        { "apos;", "'",  1 },
        { "nbsp;", " ",  1 },
        { "mdash;", "--", 2 },
        { "ndash;", "-",  1 },
        { "hellip;", "...", 3 },
        { NULL, NULL, 0 }
    };
    for (int e = 0; entities[e].name; e++) {
        int nlen = (int)strlen(entities[e].name);
        if ((int)len >= nlen && strncmp(src, entities[e].name, nlen) == 0) {
            int copy = entities[e].repl_len < (int)out_sz ? entities[e].repl_len : (int)out_sz;
            memcpy(out, entities[e].repl, copy);
            return nlen;  /* 含 ';' */
        }
    }
    return 0;
}

/* UTF-8 编码单个码点到 dst，返回写入字节数（实体表当前全 ASCII repl
 * 未启用动态编码；static inline 消未用告警，阅读器后续可调用） */
static inline int encode_utf8(uint32_t cp, char *dst, size_t sz)
{
    if (cp < 0x80 && sz >= 1) { dst[0] = (char)cp; return 1; }
    if (cp < 0x800 && sz >= 2) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000 && sz >= 3) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000 && sz >= 4) {
        dst[0] = (char)(0xF0 | (cp >> 18));
        dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* ---- HTML 清洗 ---- */
static uint32_t clean_html(const char *src, uint32_t len, char *dst, uint32_t dst_cap)
{
    uint32_t out = 0;
    bool in_tag = false;
    bool tag_is_block = false;  /* 块级标签（p/div/br/h1-h6/li）→ 换行 */

    for (uint32_t i = 0; i < len && out < dst_cap - 1; i++) {
        char c = src[i];

        if (in_tag) {
            if (c == '>') {
                in_tag = false;
                if (tag_is_block && out < dst_cap - 1) {
                    /* 避免连续换行 */
                    if (out == 0 || dst[out - 1] != '\n')
                        dst[out++] = '\n';
                }
                tag_is_block = false;
            }
            continue;
        }

        if (c == '<') {
            /* 检查是否块级标签 */
            const char *tag = src + i + 1;
            uint32_t remain = len - i - 1;
            /* br → 换行 */
            if (remain >= 2 && (strncasecmp(tag, "br", 2) == 0) &&
                (tag[2] == '>' || tag[2] == '/' || tag[2] == ' ')) {
                if (out < dst_cap - 1 && (out == 0 || dst[out - 1] != '\n'))
                    dst[out++] = '\n';
                /* 跳过整个标签 */
                while (i < len && src[i] != '>') i++;
                continue;
            }
            /* p / div / h1-h6 / li / tr → 块级 */
            if (remain >= 1) {
                char t0 = (char)tolower((unsigned char)tag[0]);
                bool is_close = (t0 == '/');
                const char *tname = is_close ? tag + 1 : tag;
                if (strncasecmp(tname, "p", 1) == 0 && (tname[1] == '>' || tname[1] == ' ' || tname[1] == '/'))
                    tag_is_block = true;
                else if (strncasecmp(tname, "div", 3) == 0)
                    tag_is_block = true;
                else if (tname[0] == 'h' && tname[1] >= '1' && tname[1] <= '6')
                    tag_is_block = true;
                else if (strncasecmp(tname, "li", 2) == 0)
                    tag_is_block = true;
                else if (strncasecmp(tname, "tr", 2) == 0)
                    tag_is_block = true;
            }
            in_tag = true;
            continue;
        }

        /* 实体解码 */
        if (c == '&') {
            char buf[4];
            int consumed = decode_entity(src + i + 1, len - i - 1, buf, sizeof(buf));
            if (consumed > 0) {
                for (int j = 0; j < consumed && j < 4 && out < dst_cap - 1; j++)
                    dst[out++] = buf[j];
                i += consumed;  /* 跳过实体文本（含 ';'） */
                continue;
            }
        }

        /* CRLF 归一 */
        if (c == '\r') {
            if (i + 1 < len && src[i + 1] == '\n') continue;
            dst[out++] = '\n';
        } else {
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
    return out;
}

/* ---- 公共 API ---- */

int book_format_load(const char *path, char **out_text, uint32_t *out_len,
                     book_format_t *out_fmt)
{
    if (!path || !out_text || !out_len) return -1;

    /* 检测格式 */
    const char *slash = strrchr(path, '/');
    const char *fname = slash ? slash + 1 : path;
    book_format_t fmt = book_format_detect(fname);
    if (out_fmt) *out_fmt = fmt;

    /* 读原始文件 */
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E("cannot open: %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    if (sz > MAX_BOOK_SIZE) { fclose(f); return -3; }

    char *raw = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!raw) { fclose(f); return -2; }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { heap_caps_free(raw); return -1; }

    /* 跳 UTF-8 BOM */
    uint32_t raw_len = (uint32_t)rd;
    char *raw_start = raw;
    if (raw_len >= 3 && (uint8_t)raw_start[0] == 0xEF &&
        (uint8_t)raw_start[1] == 0xBB && (uint8_t)raw_start[2] == 0xBF) {
        raw_start += 3;
        raw_len -= 3;
    }

    /* 分配输出缓冲（清洗后 <= 原始大小） */
    char *cleaned = heap_caps_malloc(raw_len + 1, MALLOC_CAP_SPIRAM);
    if (!cleaned) { heap_caps_free(raw); return -2; }

    uint32_t clean_len;
    switch (fmt) {
    case BOOK_FMT_MD:
        clean_len = clean_md(raw_start, raw_len, cleaned, raw_len + 1);
        break;
    case BOOK_FMT_HTML:
        clean_len = clean_html(raw_start, raw_len, cleaned, raw_len + 1);
        break;
    default:
        clean_len = clean_txt(raw_start, raw_len, cleaned, raw_len + 1);
        break;
    }
    heap_caps_free(raw);   /* 原始缓冲释放 */

    *out_text = cleaned;
    *out_len = clean_len;
    LOG_I("format load: %s fmt=%d raw=%u clean=%u", fname, fmt, (unsigned)raw_len, (unsigned)clean_len);
    return 0;
}
