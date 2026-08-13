/**
 * @file storage_manager.h
 * @brief SD 卡读写管理 (Task F-12)
 *
 * SPI 模式挂载 SD 卡到 /sdcard，提供文本读取、文件存在判断、目录列举。
 */
#ifndef INKWORD_STORAGE_MANAGER_H
#define INKWORD_STORAGE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并挂载 SD 卡。
 * @return 0 成功，非 0 失败。
 */
int storage_init(void);

/**
 * @brief 读取文本文件全部内容到调用方缓冲。
 * @param path 文件绝对路径。
 * @param out_buf 输出缓冲。
 * @param buf_size 缓冲大小（含 '\\0'）。
 * @return 实际读取字节数，<0 表示失败。
 */
int storage_read_text(const char *path, char *out_buf, size_t buf_size);

/**
 * @brief 判断文件是否存在。
 */
bool storage_file_exists(const char *path);

/**
 * @brief 列出目录下的文件名（简单版，打印到日志）。
 * @param dir 目录绝对路径，如 "/sdcard"。
 */
void storage_list_dir(const char *dir);

/**
 * @brief 获取挂载点根路径。
 */
const char *storage_mount_point(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_STORAGE_MANAGER_H */
