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

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SYNC_CLIENT_H */
