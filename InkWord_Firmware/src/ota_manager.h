/**
 * @file ota_manager.h
 * @brief OTA 升级管理器 (Task F-19)
 *
 * 双分区方案：下载固件写入 OTA 分区，校验后切换启动，失败回滚。
 */
#ifndef INKWORD_OTA_MANAGER_H
#define INKWORD_OTA_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检查是否有新固件（向 sync 配置的后端查询）。
 * @param out_url    输出新固件下载 URL。
 * @param url_len    URL 缓冲大小。
 * @param out_md5    输出期望 MD5（十六进制字符串）。
 * @param md5_len    md5 缓冲大小。
 * @param out_size   输出固件字节数。
 * @return true 有更新；false 无更新。
 */
bool ota_check_for_update(char *out_url, int url_len, char *out_md5, int md5_len, int *out_size);

/**
 * @brief 下载并写入新固件，校验后标记下次启动切换分区。(F-19)
 * @param url    固件下载 URL（HTTPS）。
 * @param expect_md5  期望 MD5（用于校验）。
 * @return 0 成功，<0 失败。
 */
int ota_perform_upgrade(const char *url, const char *expect_md5);

/**
 * @brief 标记当前固件为有效（首次正常启动后调用，防止回滚循环）。
 */
int ota_mark_valid(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_OTA_MANAGER_H */
