/**
 * @file audio_player.h
 * @brief I2S 音频驱动 (Task F-10；P0A 异步化 2026-08-24)
 *
 * 通过 MAX98357A 功放播放音频。标准飞利浦 I2S，16bit。
 * 支持 WAV(PCM) 直接播放；MP3 经 libhelix（src/mp3/）解码后送 I2S。
 *
 * 异步模型（音频播放异步化规范）：audio_play_file() 投递路径队列后
 * 立即返回，专用音频任务消费播放——按键扫描/长按判定零阻塞；
 * 重按打断重播由代际计数实现，audio_deinit() 等待任务退出后再卸载
 * I2S（power_manager 深睡收口依赖此语义）。
 */
#ifndef INKWORD_AUDIO_PLAYER_H
#define INKWORD_AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2S 总线、播放队列与音频任务。
 * @return 0 成功，非 0 失败。
 */
int audio_init(void);

/**
 * @brief 停播、回收音频任务并释放 I2S 资源（阻塞至任务退出，≤2s）。
 */
void audio_deinit(void);

/**
 * @brief 设置输出采样率（部分 MP3 文件需要切换）。
 */
int audio_set_sample_rate(uint32_t sample_rate);

/**
 * @brief 异步播放：路径入队立即返回（按键回调上下文安全）。
 *        正在播放时自动打断当前曲目改播新路径（重按打断重播）。
 * @param path 绝对路径，如 "/sdcard/audio/xxx.mp3"（.wav/.mp3）
 * @return 0 已入队；非 0 参数错误/未初始化/录音让渡期被拒。
 */
int audio_play_file(const char *path);

/**
 * @brief 同步播放（阻塞至播完或被打断）。测试/诊断场景用，
 *        勿在按键回调上下文调用（会阻塞按键扫描）。
 */
int audio_play_file_sync(const char *path);

/**
 * @brief 停止当前播放并清空待播队列（供按键打断/深睡收口）。
 */
void audio_stop(void);

/**
 * @brief 是否正在播放。
 */
bool audio_is_playing(void);

/**
 * @brief I2S 总线让渡（M5.2 跟读录音 / P2B 对话录音复用 I2S0）：
 *        on=true 打断在播曲目并等待退出，此后播放请求被拒；
 *        on=false 恢复接收。录音结束后须调 audio_bus_reconfigure()。
 */
void audio_suspend(bool on);

/**
 * @brief 录音占用 I2S0 后恢复播放侧配置（44.1k/16bit mono 重装）。
 */
int audio_bus_reconfigure(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_AUDIO_PLAYER_H */
