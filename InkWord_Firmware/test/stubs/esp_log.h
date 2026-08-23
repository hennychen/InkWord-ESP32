/* native-test stub：ESP-IDF esp_log.h → stderr（真实 debug_log.h 的
 * 唯一外部依赖；storage_manager.h 为纯标准 C 头，直接复用真实头）。 */
#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H

#include <stdio.h>

typedef enum {
    ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO,
    ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
} esp_log_level_t;

/* debug_log.h 以 ESP_LOG_LEVEL_LOCAL 为底层原语重定义 ESP_LOGx */
#define ESP_LOG_LEVEL_LOCAL(level, tag, format, ...) \
    fprintf(stderr, "[%s] " format "\n", tag, ##__VA_ARGS__)

/* Arduino 侧 esp32-hal-log 劫持路径不在 native 构建，兜底定义防裸用 */
#define ESP_LOGE(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN,  tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO,  tag, format, ##__VA_ARGS__)

#endif
