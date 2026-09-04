/**
 * @file reader_search.c
 * @brief 阅读器内文本搜索实现（2026-09-05 阅读器增强阶段四a）
 *
 * 全文线性扫描 UTF-8 子串匹配（strstr 语义，大小写敏感）。
 * 命中位置反查页码（二分 reader_engine 页偏移表），提取上下文片段。
 */
#include "reader_search.h"
#include "reader_engine.h"
#include "debug_log.h"

#include <string.h>

static const char *TAG = "RSEARCH";

/* 二分查找：byte_offset 所在页码 */
static int offset_to_page(uint32_t offset)
{
    int n = reader_page_count();
    if (n <= 0) return 0;
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (reader_engine_page_offset(mid) <= offset) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* 提取上下文片段（命中位置前后各约 16 字节，截断到 UTF-8 边界） */
static void extract_context(const char *text, uint32_t len,
                            uint32_t hit_pos, uint32_t kw_len,
                            char *ctx, size_t ctx_sz)
{
    /* 命中前 16 字节 */
    uint32_t start = hit_pos > 16 ? hit_pos - 16 : 0;
    /* 命中后 16 字节 */
    uint32_t end = hit_pos + kw_len + 16;
    if (end > len) end = len;
    /* 截断到 UTF-8 字符边界 */
    while (start > 0 && (text[start] & 0xC0) == 0x80) start--;
    while (end < len && (text[end] & 0xC0) == 0x80) end++;
    uint32_t ctx_len = end - start;
    if (ctx_len >= ctx_sz) ctx_len = ctx_sz - 1;
    memcpy(ctx, text + start, ctx_len);
    ctx[ctx_len] = '\0';
    /* 替换换行为空格 */
    for (uint32_t i = 0; i < ctx_len; i++)
        if (ctx[i] == '\n' || ctx[i] == '\r') ctx[i] = ' ';
}

int reader_search(const char *keyword, search_hit_t *results, int max_hits)
{
    if (!keyword || !keyword[0] || !results || max_hits <= 0) return 0;
    const char *text = reader_engine_get_text();
    uint32_t len = reader_engine_get_text_len();
    if (!text || len == 0) return 0;

    uint32_t kw_len = (uint32_t)strlen(keyword);
    int hits = 0;
    const char *p = text;
    const char *end = text + len - kw_len;

    while (p <= end && hits < max_hits) {
        const char *found = strstr(p, keyword);
        if (!found) break;
        uint32_t pos = (uint32_t)(found - text);
        search_hit_t *h = &results[hits];
        h->byte_offset = pos;
        h->page = offset_to_page(pos);
        extract_context(text, len, pos, kw_len, h->context, sizeof(h->context));
        hits++;
        p = found + 1;   /* 继续搜索（允许重叠） */
    }
    LOG_I("search '%s': %d hits", keyword, hits);
    return hits;
}
