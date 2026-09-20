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
#define WORD_ROOT_MAX     (96)   /* 词根词缀（如 "spect=看; vis=看"） */
#define WORD_INFL_MAX     (96)   /* 派生变形（逗号分隔） */
#define WORD_SOURCE_MAX   (64)   /* 教材来源（如 "人教版 九年级 Unit 5"） */
#define WORD_GRADE_MAX    (24)   /* 年级（如 "九年级"） */
#define WORD_CLOUD_ID_MAX (40)   /* Guid 36 字符 + 余量；云端导出词库携带 */

/** 单条词条 */
typedef struct {
    char     text[WORD_TEXT_MAX];        /**< 单词 */
    char     phonetic[WORD_PHONETIC_MAX];/**< 音标 */
    char     meaning[WORD_MEANING_MAX];  /**< 释义 */
    char     example[WORD_EXAMPLE_MAX];  /**< 例句 */
    char     audio[WORD_AUDIO_MAX];      /**< 音频文件名 */
    char     tag[WORD_TAG_MAX];          /**< 标签（年级等） */
    /* 词库扩展四字段（V2.1 §6.2，2026-08-20；旧 JSON 缺键时为空串）：
     * root 词根行（词卡右栏顶部）；source/grade 拼入底部标签行；
     * inflections 派生变形（R3.3 2026-09-20 接入渲染，词卡正文流
     * 词根后显示）。注：四字段使 WordEntry 816→1096B，词池+书最坏
     * 并发 ≈7.9MB < 8MB PSRAM 仍可行 */
    char     root[WORD_ROOT_MAX];        /**< 词根词缀（"spect=看; vis=看"） */
    char     inflections[WORD_INFL_MAX]; /**< 派生变形（逗号分隔，词卡正文流渲染） */
    char     source[WORD_SOURCE_MAX];    /**< 教材来源（"人教版 九年级 Unit 5"） */
    char     grade[WORD_GRADE_MAX];      /**< 年级（"九年级"） */
    char     cloud_id[WORD_CLOUD_ID_MAX];/**< 云端词条 Guid（后端 /words/export
                                              生成；空 = 本地导入词，评分/收藏
                                              不上报，见 learning_state 队列） */
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
 * @brief 从内存解析词库 JSON（words.json 同格式）。
 *        与 word_parser_load 同源填充逻辑；用 cJSON_ParseWithLength
 *        吃精确长度，不要求 NUL 结尾，可直接吃固件内嵌 rodata
 *        （_binary_src_default_words_json_*，无 SD 卡兜底，2026-08-23）。
 * @param json       JSON 缓冲首地址（无需 NUL 结尾）。
 * @param len        有效字节数。
 * @param out_array  输出数组（调用方分配）。
 * @param max_count  数组容量。
 * @return 实际解析到的词条数；<0 失败。
 */
int word_parser_load_mem(const char *json, size_t len,
                         WordEntry *out_array, int max_count);

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
