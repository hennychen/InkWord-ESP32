/**
 * @file word_parser.h
 * @brief 文件解析器（JSON 词库）(Task F-14)
 *
 * 解析 /sdcard/words.json，提取单词、音标、释义、例句、音频文件名，
 * 存入内存结构体数组。
 */
#ifndef INKWORD_WORD_PARSER_H
#define INKWORD_WORD_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WORD_TEXT_MAX     (64)
#define WORD_PHONETIC_MAX (64)
#define WORD_MEANING_MAX  (256)
#define WORD_EXAMPLE_MAX  (256)
#define WORD_AUDIO_MAX    (96)
#define WORD_TAG_MAX      (32)

/** 单条词条 */
typedef struct {
    char     text[WORD_TEXT_MAX];        /**< 单词 */
    char     phonetic[WORD_PHONETIC_MAX];/**< 音标 */
    char     meaning[WORD_MEANING_MAX];  /**< 释义 */
    char     example[WORD_EXAMPLE_MAX];  /**< 例句 */
    char     audio[WORD_AUDIO_MAX];      /**< 音频文件名 */
    char     tag[WORD_TAG_MAX];          /**< 标签（年级等） */
    uint8_t  difficulty;                 /**< 难度 1~5 */
    uint32_t id;                         /**< 词库内序号 */
} WordEntry;

/**
 * @brief 从 SD 卡读取并解析 words.json。
 * @param path       JSON 文件路径。
 * @param out_array  输出数组（调用方分配）。
 * @param max_count  数组容量。
 * @return 实际解析到的词条数；<0 失败。
 */
int word_parser_load(const char *path, WordEntry *out_array, int max_count);

/**
 * @brief 加载内嵌演示词库（5 条，无 SD 卡时验证学习页按键用）。
 *        仅测试构建（INKWORD_DEMO_WORDS=1）调用，正式构建不编入调用点。
 * @param out_array  输出数组（调用方分配）。
 * @param max_count  数组容量。
 * @return 实际加载数。
 */
int word_parser_load_demo(WordEntry *out_array, int max_count);

/**
 * @brief 已加载的词条总数。
 */
int word_parser_get_count(void);

/**
 * @brief 获取第 index 条词条指针（越界返回 NULL）。
 */
const WordEntry *word_parser_get(int index);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WORD_PARSER_H */
