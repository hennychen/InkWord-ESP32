/**
 * @file reader_search.h
 * @brief 阅读器内文本搜索（2026-09-05 阅读器增强阶段四a）
 *
 * 全文 UTF-8 子串搜索，O(N) 线性扫描 4MB < 100ms（ESP32-S3 240MHz）。
 * 返回命中位置列表（页码 + 上下文片段），供搜索结果列表 UI 消费。
 */
#ifndef INKWORD_READER_SEARCH_H
#define INKWORD_READER_SEARCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单条搜索结果 */
typedef struct {
    int      page;            /**< 命中页码 */
    uint32_t byte_offset;     /**< 命中位置（书全文内字节偏移） */
    char     context[40];     /**< 上下文片段（命中前后各约 16 字） */
} search_hit_t;

/** 最大返回命中数 */
#define SEARCH_MAX_HITS 50

/**
 * @brief 在已加载的书中搜索关键字。
 * @param keyword  搜索关键字（UTF-8）。
 * @param results  输出数组（调用方分配，建议 >= SEARCH_MAX_HITS）。
 * @param max_hits 数组容量。
 * @return 命中数量（0 = 无匹配）。
 */
int reader_search(const char *keyword, search_hit_t *results, int max_hits);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_READER_SEARCH_H */
