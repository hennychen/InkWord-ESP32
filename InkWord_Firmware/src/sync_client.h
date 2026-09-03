/**
 * @file sync_client.h
 * @brief HTTP 同步客户端 (Task F-18)
 *
 * 与后端交互：注册、拉取增量词库、回传学习记录（评分/收藏）、
 * 心跳、天气与校时。
 * 词身份协议（P2）：评分/收藏上报的 wordId 为云端词条 Guid 字符串
 * （words.json 的 cloudId 字段，后端 /admin/words/export 生成）。
 */
#ifndef INKWORD_SYNC_CLIENT_H
#define INKWORD_SYNC_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_WORD_ID_MAX (40)  /**< Guid 36 字符 + 余量 */

/* v2.0 绑定换发（ADR-001 §五）：App 绑定设备 → 后端换发 ApiKey → 旧钥
 * 即刻失效 401。sync_pull_words/push_progress/push_collect/heartbeat
 * 逢 401 返回 SYNC_ERR_AUTH，调用方应清钥重注册（MAC 幂等取回新钥，
 * sync_session.cpp sync_recover_auth 自愈链路）；与通用失败 -1 区分。 */
#define SYNC_ERR_AUTH  (-2)

/** 学习记录条目（回传给后端） */
typedef struct {
    char     word_id[SYNC_WORD_ID_MAX]; /* 云端词条 Guid；空串条目应跳过 */
    uint8_t  quality;                   /* 0~5 */
    int64_t  timestamp;                 /* Unix 秒；0 = 由服务器落地时间代替
                                            （设备自治钟无绝对时间域） */
} ProgressItem;

/**
 * @brief 设置后端 Base URL，如 "https://api.einkword.com"。
 *        http: 前缀自动降级明文 TCP（本地开发后端）。
 */
void sync_set_base_url(const char *url);

/**
 * @brief 读取当前后端 Base URL（ota_manager 等复用同一配置源）。
 */
const char *sync_get_base_url(void);

/**
 * @brief 设置设备认证 Key（写入 X-Device-Key 头）。
 */
void sync_set_device_key(const char *key);

/**
 * @brief 设备认证 Key 是否已配置（NVS 恢复或注册成功后为真）。
 */
bool sync_has_device_key(void);

/**
 * @brief 读取设备认证 Key（audio_sync 等复用 X-Device-Key 头模式）。
 */
const char *sync_get_device_key(void);

/**
 * @brief 首次注册：POST /api/device/register（后端按 MAC 幂等，重复
 *        注册返回既有 ApiKey）。成功后调用方应持久化到 NVS。
 * @param mac         十六进制 MAC 字符串（如 "AABBCCDDEEFF"）。
 * @param name        设备名（可 NULL，后端默认 InkWord-XXXX）。
 * @param out_api_key 输出 ApiKey 缓冲。
 * @param key_len     缓冲大小。
 * @return 0 成功；<0 失败（网络/解析）。
 */
int sync_register(const char *mac, const char *name,
                  char *out_api_key, size_t key_len);

/**
 * @brief 拉取自 localVersion 以来的增量词库 JSON。(F-18)
 * @param local_version 设备本地词库版本号。
 * @param out_buf       输出缓冲。
 * @param buf_size      缓冲大小。
 * @return 服务端新版本号；<0 失败。
 */
int sync_pull_words(int local_version, char *out_buf, int buf_size);

/**
 * @brief 批量回传学习记录（评分）。
 * @param items  记录数组（word_id 为空串的条目由调用方过滤）。
 * @param count  记录数。
 * @return 0 成功。
 */
int sync_push_progress(const ProgressItem *items, int count);

/**
 * @brief 收藏状态上报（设备端 SET 长按切换后同步）。
 * @param word_id   云端词条 Guid。
 * @param collected 收藏状态。
 * @return 0 成功。
 */
int sync_push_collect(const char *word_id, bool collected);

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

/** A3 对话周报（LLM 复盘五段 JSON 的设备端消费子集，字段截断保屏显安全） */
typedef struct {
    char week_start[16];   /**< "2026-08-25"（ISO 日期前十位） */
    int  turn_count;       /**< 聚合轮数 */
    char summary[96];      /**< 本周对话概况 */
    char suggestion[96];   /**< 建议练习场景/方向 */
    char words[96];        /**< 推荐复习词（reviewWords 以 " · " 拼接） */
} chat_review_t;

/**
 * @brief 拉取 AI 对话周报（A3）。
 *        GET /api/device/chat-review（周报 Job 周日 05:00 写 Redis/表，
 *        双通道下发）；响应 data.review 为 LLM 结构化 JSON，设备端仅
 *        消费 summary/suggestion/reviewWords 三段。
 * @param out 输出结构体。
 * @return 0 成功；1 尚无周报（HTTP 404，周报未生成）；<0 网络/解析失败。
 */
int sync_fetch_chat_review(chat_review_t *out);

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
