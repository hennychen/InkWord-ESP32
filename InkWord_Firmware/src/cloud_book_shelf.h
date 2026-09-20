/**
 * @file cloud_book_shelf.h
 * @brief 云书屋管理（R3.2 云书屋，2026-09-21）
 *
 * 从云端拉取书目列表，显示云端可下载书籍。支持下载到本地 SD 卡，
 * 下载后自动加入本地书架。阅读进度和书签通过 sync_client 自动上云。
 *
 * 作为覆盖层经 page_router_push 入栈（g_cloud_book_shelf_page 导出），
 * 上/下选择、中键下载、RST 退出。
 *
 * 内存：云端书目表静态分配（最多 32 本），不占 PSRAM。
 */
#ifndef INKWORD_CLOUD_BOOK_SHELF_H
#define INKWORD_CLOUD_BOOK_SHELF_H

#include <stdint.h>
#include <stdbool.h>
#include "page_router.h"   /* page_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 云端书籍条目 */
typedef struct {
    char     book_key[40];     /**< 云端书籍唯一标识 */
    char     title[48];        /**< 书名 */
    char     author[32];       /**< 作者 */
    uint32_t file_size;        /**< 文件大小（bytes） */
    bool     downloaded;       /**< 是否已下载到本地 */
} cloud_book_entry_t;

/**
 * @brief 从云端拉取书目列表。
 *        调用 sync_pull_book_list 获取 JSON，解析填充条目表。
 * @return 拉取到的书籍数量（<0 失败）
 */
int cloud_book_shelf_fetch(void);

/** 云端书籍数量（fetch 后有效） */
int cloud_book_shelf_count(void);

/** 第 idx 本书的条目指针（fetch 后有效；越界返回 NULL） */
const cloud_book_entry_t *cloud_book_shelf_at(int idx);

/**
 * @brief 下载第 idx 本书到本地 SD 卡。
 *        下载路径 /sdcard/books/{book_key}.txt，下载成功后标记 downloaded。
 * @return 0 成功；<0 失败
 */
int cloud_book_shelf_download(int idx);

/** T1.4 页面协议实例（覆盖层入栈） */
extern const page_t g_cloud_book_shelf_page;

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CLOUD_BOOK_SHELF_H */
