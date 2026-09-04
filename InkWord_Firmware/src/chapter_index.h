/**
 * @file chapter_index.h
 * @brief 章节检测与导航（2026-09-05 阅读器增强阶段二）
 *
 * 自动识别书籍章节结构，支持章节跳转。检测策略按格式分流：
 *   TXT  —— 空行后短行且以"第"开头或数字开头；
 *   MD   —— `# ` / `## ` 等标题行；
 *   HTML —— `<h1>`~`<h6>` 标签内容。
 * 兜底：未检测到任何章节标记时按约 2000 字自动分章。
 *
 * 章节表存 PSRAM（每章 ~56B，100 章 ≈ 5.6KB），reader_engine 建页表
 * 后调 chapter_index_map_pages 填充 start_page 字段。
 *
 * 与 reader_engine 协作：reader_engine_load_book 成功后调
 * chapter_index_build 构建章节索引；章节跳转经 chapter_jump_to
 * 返回页码，由 study_mode_machine 设置游标。
 */
#ifndef INKWORD_CHAPTER_INDEX_H
#define INKWORD_CHAPTER_INDEX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 格式类型（与 book_format 共用枚举值，此处独立定义避免循环依赖） */
typedef enum {
    CH_FMT_TXT = 0,
    CH_FMT_MD,
    CH_FMT_HTML,
} chapter_format_t;

/** 单章条目 */
typedef struct {
    char     title[48];      /**< 章节标题（截断到 47 字符） */
    uint32_t byte_offset;    /**< 章首字节偏移（书全文内） */
    int      start_page;     /**< 起始页码（map_pages 后填；之前 = -1） */
} chapter_entry_t;

/** 章节表最大容量（PSRAM 动态分配，此为上限保护） */
#define CHAPTER_MAX 256

/**
 * @brief 根据书全文构建章节索引。
 * @param text   书全文（UTF-8，NUL 结尾）。
 * @param len    字节长度。
 * @param fmt    格式类型（影响章节标记检测策略）。
 * @return 检测到的章节数量（>=1，兜底自动分章保证至少 1 章）。
 */
int chapter_index_build(const char *text, uint32_t len, chapter_format_t fmt);

/** 当前章节数（build 后有效；未 build 返回 0） */
int chapter_index_count(void);

/** 第 idx 章的条目指针（build 后有效；越界返回 NULL） */
const chapter_entry_t *chapter_index_at(int idx);

/**
 * @brief 建页表后调用：按 byte_offset 就近匹配页码填入 start_page。
 *        二分查找 reader_engine 页偏移表。
 * @param page_offsets 页首字节偏移数组（reader_engine 导出）。
 * @param page_count   页数。
 */
void chapter_index_map_pages(const uint32_t *page_offsets, int page_count);

/**
 * @brief 当前页所在章节索引（二分查找 start_page）。
 * @return 章节索引（0 基）；无章节返回 0。
 */
int chapter_index_find_by_page(int page);

/**
 * @brief 跳到第 ch_idx 章的首页码（map_pages 后有效）。
 * @return 页码；越界/未映射返回 0。
 */
int chapter_index_jump_to(int ch_idx);

/**
 * @brief 按文件扩展名推断格式类型。
 */
chapter_format_t chapter_format_detect(const char *filename);

/** 释放章节表（切书/退出时调用） */
void chapter_index_free(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CHAPTER_INDEX_H */
