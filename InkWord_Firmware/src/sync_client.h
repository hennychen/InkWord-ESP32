/**
 * @file sync_client.h
 * @brief HTTP 同步客户端 (Task F-18)
 *
 * 与后端交互：拉取增量词库、回传学习记录、心跳。
 */
#ifndef INKWORD_SYNC_CLIENT_H
#define INKWORD_SYNC_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 学习记录条目（回传给后端） */
typedef struct {
    uint32_t word_id;
    uint8_t  quality;       /* 0~5 */
    int64_t  timestamp;     /* Unix 秒 */
} ProgressItem;

/**
 * @brief 设置后端 Base URL，如 "https://api.inkword.example.com"
 */
void sync_set_base_url(const char *url);

/**
 * @brief 设置设备认证 Key（写入 X-Device-Key 头）。
 */
void sync_set_device_key(const char *key);

/**
 * @brief 拉取自 localVersion 以来的增量词库 JSON。(F-18)
 * @param local_version 设备本地词库版本号。
 * @param out_buf       输出缓冲。
 * @param buf_size      缓冲大小。
 * @return 服务端新版本号；<0 失败。
 */
int sync_pull_words(int local_version, char *out_buf, int buf_size);

/**
 * @brief 批量回传学习记录。
 * @param items  记录数组。
 * @param count  记录数。
 * @return 0 成功。
 */
int sync_push_progress(const ProgressItem *items, int count);

/**
 * @brief 发送心跳。
 * @param battery 电量百分比 0~100。
 * @param fw_ver  固件版本字符串。
 */
int sync_heartbeat(int battery, const char *fw_ver);

/** 天气信息（后端转发聚合，附带服务器时间用于校时兜底） */
typedef struct {
    int8_t  temp_c;        /**< 摄氏温度 */
    uint8_t icon;          /**< 天气图标 0~7（后端已完成 WMO 映射，对应 weather_icons.h） */
    char    desc[24];      /**< ASCII 短描述，如 "Partly Cloudy" */
    int64_t server_time;   /**< 服务器 Unix 秒（设备校时兜底；0=未提供） */
    int16_t tz_offset_min; /**< 时区偏移分钟（东八区=480；v1 仅存储不应用） */
} weather_info_t;

/**
 * @brief 拉取天气（含服务器时间）。
 *        GET /api/device/weather，后端聚合上游天气并缓存；
 *        响应信封 { code, message, data:{ icon, tempC, desc, serverTime, tzOffsetMin } }。
 * @param out 输出结构体。
 * @return 0 成功；<0 失败（必填字段缺失/解析失败）。
 */
int sync_fetch_weather(weather_info_t *out);

/**
 * @brief 经 HTTP Date 响应头校时（设备主时间源）。
 *        GET generate_204 探测页（无 body 流量最小），解析 Date 头返回 Unix 秒；
 *        失败/字段异常返回 0，不修改任何时钟状态（如何采纳由调用方决定）。
 *        背景：运营商 UDP 123 劫持使 SNTP 不可用；系统 settimeofday 路径
 *        在本机亦损坏（2026-08 实测），调用方以应用层基准对方式采纳。
 */
int64_t sync_fetch_http_time(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SYNC_CLIENT_H */
