/**
 * @file book_shelf.h
 * @brief 书架管理（2026-09-05 阅读器增强阶段一）
 *
 * 扫描 /sdcard/books/ 下所有 .txt/.md/.html 文件，提取书名（首行或
 * 文件名）、文件大小、阅读进度（NVS 恢复），提供列表 UI 供用户选书。
 * 选书后经 reader_engine_load_book 加载，自动切换 MODE_READER。
 *
 * 作为覆盖层经 page_router_push 入栈（g_book_shelf_page 导出），
 * 上/下选择、中键确认加载、RST 退出。
 *
 * 内存：书籍条目表静态分配（最多 32 本），不占 PSRAM。
 */
#ifndef INKWORD_BOOK_SHELF_H
#define INKWORD_BOOK_SHELF_H

#include <stdint.h>
#include <stdbool.h>
#include "page_router.h"   /* page_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 单本书条目（书架列表一项） */
typedef struct {
    char     filename[64];   /**< 文件名（含扩展名） */
    char     title[48];      /**< 书名（首行或文件名截取） */
    uint32_t file_size;      /**< 文件大小（bytes） */
    uint32_t signature;      /**< FNV 书签名（与 reader_engine 同源） */
    uint8_t  progress_pct;   /**< 阅读进度 0-100 */
    bool     has_progress;   /**< 是否有阅读记录 */
} book_entry_t;

/**
 * @brief 扫描 /sdcard/books/ 目录，填充书架条目表。
 *        每次进入书架 UI 时调用（enter 回调）。
 * @return 扫描到的书籍数量（0 = 无书）
 */
int book_shelf_scan(void);

/** 书架书籍数量（scan 后有效） */
int book_shelf_count(void);

/** 第 idx 本书的条目指针（scan 后有效；越界返回 NULL） */
const book_entry_t *book_shelf_at(int idx);

/**
 * @brief 加载第 idx 本书到 reader_engine + 切换 MODE_READER + 恢复进度。
 * @return 0 成功；-1 加载失败
 */
int book_shelf_load(int idx);

/** T1.4 页面协议实例（enter=book_shelf_enter；覆盖层入栈） */
extern const page_t g_book_shelf_page;

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BOOK_SHELF_H */
