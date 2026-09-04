/**
 * @file book_format.h
 * @brief 多格式书籍解析（2026-09-05 阅读器增强阶段五）
 *
 * 兼容 TXT / Markdown / HTML 子集，统一输出纯文本流给 reader_engine。
 * 格式清洗在加载时一次性完成（输出纯文本入 PSRAM），清洗后文本
 * <= 原始文件大小（去除标记只减不增），4MB 上限不变。
 *
 * 处理规则：
 *   TXT     —— 直通，仅归一化 CRLF → LF；
 *   Markdown -- 去除 # / * / _ / ` 标记；# 标题保留文本（供章节检测）；
 *   HTML     -- 去除所有标签（保留 br/p/h1-h6 为换行/章节标记）；
 *               amp 等实体解码。
 */
#ifndef INKWORD_BOOK_FORMAT_H
#define INKWORD_BOOK_FORMAT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 格式类型（与 chapter_index chapter_format_t 枚举值对齐） */
typedef enum {
    BOOK_FMT_TXT = 0,
    BOOK_FMT_MD,
    BOOK_FMT_HTML,
} book_format_t;

/**
 * @brief 按文件扩展名检测格式。
 * @param filename 文件名（含扩展名，如 "book.md"）。
 * @return 格式类型（默认 TXT）。
 */
book_format_t book_format_detect(const char *filename);

/**
 * @brief 加载并清洗书籍文件，输出纯文本。
 *        内部 PSRAM 分配输出缓冲，调用方用完后需 free（heap_caps_free）。
 * @param path      文件绝对路径。
 * @param out_text  输出：清洗后纯文本指针（PSRAM，调用方负责释放）。
 * @param out_len   输出：清洗后文本字节长度。
 * @param out_fmt   输出：检测到的格式类型（可 NULL）。
 * @return 0 成功；-1 打开/读取失败；-2 内存不足；-3 文件过大（>4MB）
 */
int book_format_load(const char *path, char **out_text, uint32_t *out_len,
                     book_format_t *out_fmt);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BOOK_FORMAT_H */
