/**
 * @file debug_log.h
 * @brief 日志与调试模块封装 (Task F-04)
 *
 * 基于 ESP-IDF 的 esp_log.h，封装为 LOG_I / LOG_W / LOG_E / LOG_D 宏，
 * 统一 TAG 管理并支持运行时分级过滤。串口输出自带时间戳（esp_log 默认行为）。
 */
#ifndef INKWORD_DEBUG_LOG_H
#define INKWORD_DEBUG_LOG_H

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化日志系统：设置默认全局日志级别。
 * @note  可通过 menuconfig -> Component config -> Log output 调整。
 *        也支持运行时调用 esp_log_level_set("*", level) 动态调整。
 */
void log_init(void);

/**
 * @brief 便捷的运行时分级切换（可选）。
 * @param level ESP_LOG_NONE/ERROR/WARN/INFO/DEBUG/VERBOSE
 */
void log_set_global_level(esp_log_level_t level);

/* ---- 统一封装宏：每个文件顶部需定义 static const char *TAG = "xxx"; ----
 * 输出格式形如：I (12345) TAG : message
 * 内置时间戳与等级着色（受 monitor_filters=colorize 影响）。
 */
#define LOG_I(fmt, ...)   ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...)   ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...)   ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define LOG_D(fmt, ...)   ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#define LOG_V(fmt, ...)   ESP_LOGV(TAG, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_DEBUG_LOG_H */
