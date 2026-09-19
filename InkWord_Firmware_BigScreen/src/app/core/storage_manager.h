/**
 * @file storage_manager.h
 * @brief SPIFFS 书库存储管理（大屏阅读器阶段，替换桩实现）
 *
 * 小屏 storage_manager 承载 SD 卡词库/音频/阅读器资源读写；大屏
 * 无 SD，书库落 storage 分区（partitions_bigscreen.csv 0x9C0000
 * ≈ 9.7MB SPIFFS），挂载点 /storage，书籍目录 /storage/books/
 * （UTF-8 文本 .txt / .md）。
 *
 * storage_read_text 语义与小屏一致（任意 VFS 路径）：词库 SD 回退
 * 路径（word_parser 读 /sdcard/ 下文件）在此恒失败 → 自然回退内嵌库，
 * 行为与桩时代零差异；读 /storage/... 路径则命中书库。
 */
#ifndef INKWORD_STORAGE_MANAGER_H
#define INKWORD_STORAGE_MANAGER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 书籍文件名容量（含 NUL；书架列表/书名显示共用） */
#define STORAGE_BOOK_NAME_MAX 64

/** 书架单次枚举上限（reader_menu 列表容量对齐） */
#define STORAGE_SHELF_MAX 16

/**
 * @brief 挂载 SPIFFS（storage 分区；挂载失败自动格式化重挂，首次
 *        烧录分区未格式化场景）。app_main init 链调用一次。
 *        失败不阻塞应用（书库为空语义，阅读器回退演示书）。
 * @return 0 成功；-1 挂载失败。
 */
int storage_init(void);

/**
 * @brief 读文本文件到 buf（NUL 终止）。
 * @return 读取字节数（不含 NUL）；打开/读取失败 -1。
 *         buf_size 需容纳内容 + 1（NUL）。
 */
int storage_read_text(const char *path, char *out_buf, size_t buf_size);

/**
 * @brief 枚举书库（/storage/books/ 下 .txt/.md，文件名升序稳定排列）。
 * @param names 出参数组 [n][STORAGE_BOOK_NAME_MAX]。
 * @param max   数组第一维容量（钳到 STORAGE_SHELF_MAX）。
 * @return 实际枚举数（未挂载/无目录/无书均返回 0）。
 */
int storage_books_list(char names[][STORAGE_BOOK_NAME_MAX], int max);

/** 书名 → 绝对路径（/storage/books/<name>；name 空串产出目录本身） */
void storage_book_path(char *out, size_t n, const char *name);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_STORAGE_MANAGER_H */
