/**
 * @file debug_log.h
 * @brief 日志封装（小屏 debug_log.h 的 ESP-IDF 直译版）
 *
 * LOG_I/W/E/D/V 统一宏，每文件顶部 static const char *TAG = "xxx";
 * 输出格式形如：I (12345) TAG : message（esp_log 原生时间戳+着色）。
 *
 * 与小屏版差异：Arduino 框架下 CONFIG_ARDUHAL_ESP_LOG 会把
 * ESP_LOGx 劫持为 log_x 需 #undef 还原；IDF 原生无此问题，直接
 * 别名转发。log_init/log_set_global_level 未被移植单元调用，省略
 * （日志级别经 menuconfig / esp_log_level_set 调）。
 */
#ifndef INKWORD_DEBUG_LOG_H
#define INKWORD_DEBUG_LOG_H

#include "esp_log.h"

/* ---- 统一封装宏：每个文件顶部需定义 static const char *TAG = "xxx"; ---- */
#define LOG_I(fmt, ...)   ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...)   ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...)   ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define LOG_D(fmt, ...)   ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#define LOG_V(fmt, ...)   ESP_LOGV(TAG, fmt, ##__VA_ARGS__)

#endif /* INKWORD_DEBUG_LOG_H */
