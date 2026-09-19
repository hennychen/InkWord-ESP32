#pragma once
// ESP32-S3 内部温度监控（规格书 §10 工作温度 0~50°C）
// 功能：周期性读取片上温度传感器，超阈告警并触发保护
#include <stdbool.h>

// 温度阈值（摄氏度）
#define TEMP_WARN_THRESHOLD_C   45   // 告警阈值
#define TEMP_SHUTDOWN_THRESHOLD_C 50 // 停机保护阈值（规格书上限）

// 初始化温度监控（启动周期性采样任务）
// check_interval_ms: 采样间隔（默认 30000ms = 30 秒）
void temp_monitor_start(int check_interval_ms);

// 停止温度监控任务
void temp_monitor_stop(void);

// 读取当前温度（摄氏度），失败返回 -1
float temp_monitor_read(void);

// 获取是否已触发停机保护
bool temp_monitor_is_shutdown(void);
