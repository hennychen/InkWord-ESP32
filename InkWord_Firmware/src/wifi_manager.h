/**
 * @file wifi_manager.h
 * @brief WiFi 联网与配置保存 (Task F-17)
 *
 * 从 NVS 读取 SSID/密码自动连接；断线自动重连。
 */
#ifndef INKWORD_WIFI_MANAGER_H
#define INKWORD_WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi（NVS + netif + 事件循环），并尝试连接已保存网络。
 * @return 0 成功。
 */
int wifi_manager_init(void);

/**
 * @brief 保存并连接指定 SSID/密码。
 */
int wifi_connect(const char *ssid, const char *password);

/**
 * @brief 是否已获得 IP（已连上）。
 */
bool wifi_is_connected(void);

/**
 * @brief 断开。
 */
void wifi_disconnect(void);

/**
 * @brief 扫描附近 Wi-Fi 网络（阻塞，约 1~2 秒）。
 * @param results  输出缓冲，由调用方分配。
 * @param max      缓冲容量（最大条目数）。
 * @return 实际扫到的 AP 数量；<0 失败。
 */
int wifi_scan(wifi_ap_record_t *results, int max);

/**
 * @brief NVS 中是否已存在已保存的 Wi-Fi 凭据。
 * @return true 存在可用凭据；false 无。
 */
bool wifi_has_saved_credentials(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WIFI_MANAGER_H */
