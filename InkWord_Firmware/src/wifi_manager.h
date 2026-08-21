/**
 * @file wifi_manager.h
 * @brief WiFi 联网与配置保存 (Task F-17)
 *
 * 从 NVS 读取 SSID/密码自动连接；断线自动重连。
 */
#ifndef INKWORD_WIFI_MANAGER_H
#define INKWORD_WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
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
 * @brief 停射频入深睡（P5）：esp_wifi_stop 停 STA/AP 与射频
 *        （disconnect 只断连不停射频，睡眠前须彻底断电域）。
 *        不重入：下次使用前需 esp_wifi_start 或重新 wifi_manager_init
 *        （深睡唤醒 = 重启，正常路径不受影响）。
 */
void wifi_radio_off(void);

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

/* ============================================================
 * SoftAP 配网门户（captive portal）与异步连接
 * ============================================================ */

/** 异步连接状态 */
typedef enum {
    WCONN_IDLE = 0,     /**< 空闲 */
    WCONN_CONNECTING,   /**< 连接中 */
    WCONN_OK,           /**< 已连接 */
    WCONN_FAIL          /**< 连接失败 */
} wconn_state_t;

/**
 * @brief 开启 SoftAP 热点 InkWord-Setup（开放，用于配网门户）。
 *        内部切 APSTA 模式；STA 凭据不受影响。
 * @return 0 成功或已开启。
 */
int wifi_start_softap(void);

/**
 * @brief 关闭 SoftAP 回纯 STA 模式（重新 start 后事件回调自动重连已保存网络）。
 */
void wifi_stop_softap(void);

/**
 * @brief SoftAP 是否开启。
 */
bool wifi_softap_active(void);

/**
 * @brief 异步连接指定网络（独立任务执行，不阻塞 HTTP 服务），
 *        结果经 wifi_connect_state() 轮询。凭据同时写入 NVS。
 * @return 0 已受理；<0 参数非法或任务创建失败。
 */
int wifi_connect_async(const char *ssid, const char *password);

/**
 * @brief 查询异步连接状态。
 */
wconn_state_t wifi_connect_state(void);

/**
 * @brief 获取 STA 接口 IP 点分字符串（未连接返回 false）。
 */
bool wifi_get_sta_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WIFI_MANAGER_H */
