/**
 * @file debug_log.c
 * @brief 日志与调试模块实现 (Task F-04)
 */
#include "debug_log.h"

void log_init(void)
{
    /* 默认全局 INFO 级别；各模块可在初始化时单独调高/调低。 */
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI("LOG", "logging system initialized @ INFO");
}

void log_set_global_level(esp_log_level_t level)
{
    esp_log_level_set("*", level);
}
