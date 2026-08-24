/**
 * @file audio_sync.h
 * @brief 词条发音音频同步 (P0C，2026-08-24)
 *
 * 后端 TTS 产物（GET /api/device/audio/{cloud_id}.mp3）按需补齐到
 * /sdcard/audio/。音频为缓存资产：SD 缺失/下载失败均不阻断学习
 * 闭环（speak 路径仅 LOG + 短震，见 study_mode_machine）。
 * 文件名由设备端从 words.json 的 cloudId 推导，不进词库（红线）。
 */
#ifndef INKWORD_AUDIO_SYNC_H
#define INKWORD_AUDIO_SYNC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 词池中 cloud_id 非空的词条数（纯内存遍历，菜单徽标分母）。
 */
int audio_sync_cloud_total(void);

/**
 * @brief 缓存的缺失音频数；-1 = 尚未统计（调 audio_sync_refresh_stats）。
 *        缓存在同步任务结束时自动更新，徽标现取无阻塞。
 */
int audio_sync_missing_cached(void);

/**
 * @brief 现算缺失数并更新缓存（阻塞式：5000 词量级 f_stat 约 1s，
 *        仅菜单「音频同步」确认时调用，勿在按键高频路径使用）。
 * @return 缺失数；<0 SD 目录不可用（无卡）。
 */
int audio_sync_refresh_stats(void);

/**
 * @brief 同步任务是否在运行（徽标「...」与重复启动拒绝）。
 */
bool audio_sync_is_running(void);

/**
 * @brief 本次同步已处理的缺失词数（LAN 页/日志展示用）。
 */
int audio_sync_progress(void);

/**
 * @brief 启动后台同步任务（6KB 栈，串行下载，不阻塞 UI）。
 *        任务自身逐词检查存在性，无需预扫词池。
 * @return 0 已启动；-1 拒绝（无 SD/无 Wi-Fi/未配 Key/任务已在跑）。
 */
int audio_sync_start(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_AUDIO_SYNC_H */
