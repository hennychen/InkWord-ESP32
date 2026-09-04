/**
 * @file reader_word_link.h
 * @brief 阅读中生词联动（2026-09-05 阅读器增强阶段四b）
 *
 * 当前页文本按空格/标点分词（中文按字，英文按空格），在词库中查找
 * 匹配项，提供词列表供用户查看和跳转闪卡学习。
 *
 * 跳转后阅读器进度保持（reader_progress_page 已有机制），回来可恢复。
 */
#ifndef INKWORD_READER_WORD_LINK_H
#define INKWORD_READER_WORD_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "word_parser.h"   /* WordEntry */

#ifdef __cplusplus
extern "C" {
#endif

/** 当前页匹配到的生词条目 */
typedef struct {
    char     text[64];          /**< 单词文本 */
    char     meaning[96];       /**< 释义（截断） */
    int      word_index;        /**< 词库索引（word_parser_get 用） */
    int      char_offset;       /**< 页内字符偏移（定位用） */
} word_link_t;

/** 单页最大匹配生词数 */
#define WORD_LINK_MAX 32

/**
 * @brief 扫描当前页文本，在词库中查找匹配的生词。
 * @param page       当前页码。
 * @param out        输出数组（调用方分配，建议 >= WORD_LINK_MAX）。
 * @param max_links  数组容量。
 * @return 匹配到的生词数量（0 = 当前页无生词）。
 */
int reader_word_link_scan(int page, word_link_t *out, int max_links);

/**
 * @brief 跳转到指定生词的闪卡学习页。
 * @param word_index 词库索引（word_link_t.word_index）。
 * @return 0 成功；-1 失败
 */
int reader_word_link_jump(int word_index);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_READER_WORD_LINK_H */
