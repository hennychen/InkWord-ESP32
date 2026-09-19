// ESP32-S3 内部温度监控实现
// 规格书 §10：工作温度 0~50°C，超温停机保护
#include "driver/temp_monitor.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "temp";

static temperature_sensor_handle_t s_temp_sensor = NULL;
static TaskHandle_t s_monitor_task = NULL;
static bool s_shutdown_triggered = false;

static void monitor_task(void* pv) {
    const int interval_ms = *(int*)pv;
    free(pv);

    float last_logged_temp = -999;  // 强制首次日志
    int warn_count = 0;

    while (1) {
        float temp_c = temp_monitor_read();
        if (temp_c < 0) {
            ESP_LOGW(TAG, "温度读取失败");
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
            continue;
        }

        // 温度变化 >0.5°C 或跨阈值时日志（避免刷屏）
        bool cross_warn = (last_logged_temp < TEMP_WARN_THRESHOLD_C &&
                          temp_c >= TEMP_WARN_THRESHOLD_C);
        bool cross_shutdown = (last_logged_temp < TEMP_SHUTDOWN_THRESHOLD_C &&
                              temp_c >= TEMP_SHUTDOWN_THRESHOLD_C);
        bool significant_change = (temp_c - last_logged_temp > 0.5 ||
                                  last_logged_temp - temp_c > 0.5);

        if (cross_warn || cross_shutdown || significant_change) {
            ESP_LOGI(TAG, "芯片温度：%.1f°C", (double)temp_c);
            last_logged_temp = temp_c;
        }

        // 告警阈值
        if (temp_c >= TEMP_WARN_THRESHOLD_C && temp_c < TEMP_SHUTDOWN_THRESHOLD_C) {
            warn_count++;
            if (warn_count == 1 || warn_count % 10 == 0) {
                ESP_LOGW(TAG, "⚠ 温度告警：%.1f°C（阈值 %d°C，第 %d 次）",
                        (double)temp_c, TEMP_WARN_THRESHOLD_C, warn_count);
            }
        } else {
            warn_count = 0;  // 降温后重置计数
        }

        // 停机保护阈值
        if (temp_c >= TEMP_SHUTDOWN_THRESHOLD_C) {
            ESP_LOGE(TAG, "🛑 超温停机：%.1f°C（阈值 %d°C）",
                    (double)temp_c, TEMP_SHUTDOWN_THRESHOLD_C);
            s_shutdown_triggered = true;
            // 停机后不再循环，任务挂起等待外部复位
            vTaskSuspend(NULL);
        }

        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

void temp_monitor_start(int check_interval_ms) {
    if (s_monitor_task != NULL) {
        ESP_LOGW(TAG, "温度监控已在运行");
        return;
    }

    // 初始化温度传感器（ESP32-S3 支持 0~80°C 范围）
    temperature_sensor_config_t config = {
        .range_min = 0,
        .range_max = 80,
    };
    esp_err_t err = temperature_sensor_install(&config, &s_temp_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "温度传感器安装失败：%s", esp_err_to_name(err));
        return;
    }

    err = temperature_sensor_enable(s_temp_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "温度传感器使能失败：%s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_temp_sensor);
        s_temp_sensor = NULL;
        return;
    }

    ESP_LOGI(TAG, "温度监控启动（间隔 %dms，告警 %d°C，停机 %d°C）",
             check_interval_ms, TEMP_WARN_THRESHOLD_C, TEMP_SHUTDOWN_THRESHOLD_C);

    int* interval = malloc(sizeof(int));
    *interval = check_interval_ms;

    if (xTaskCreate(monitor_task, "temp_mon", 2048, interval, 5,
                    &s_monitor_task) != pdPASS) {
        ESP_LOGE(TAG, "温度监控任务创建失败");
        free(interval);
        temperature_sensor_disable(s_temp_sensor);
        temperature_sensor_uninstall(s_temp_sensor);
        s_temp_sensor = NULL;
    }
}

void temp_monitor_stop(void) {
    if (s_monitor_task != NULL) {
        vTaskDelete(s_monitor_task);
        s_monitor_task = NULL;
    }
    if (s_temp_sensor != NULL) {
        temperature_sensor_disable(s_temp_sensor);
        temperature_sensor_uninstall(s_temp_sensor);
        s_temp_sensor = NULL;
    }
    ESP_LOGI(TAG, "温度监控已停止");
}

float temp_monitor_read(void) {
    if (s_temp_sensor == NULL) {
        return -1;
    }
    float temp_c = 0;
    esp_err_t err = temperature_sensor_get_celsius(s_temp_sensor, &temp_c);
    if (err != ESP_OK) {
        return -1;
    }
    return temp_c;
}

bool temp_monitor_is_shutdown(void) {
    return s_shutdown_triggered;
}
