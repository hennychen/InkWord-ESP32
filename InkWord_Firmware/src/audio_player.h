/**
 * @file audio_player.h
 * @brief I2S 音频驱动 (Task F-10)
 *
 * 通过 MAX98357A 功放播放音频。配置标准飞利浦 I2S，44.1kHz/16bit。
 * 支持 WAV(PCM) 直接播放；MP3 经内置轻量解码后送 I2S。
 */
#ifndef INKWORD_AUDIO_PLAYER_H
#define INKWORD_AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2S 总线与 MAX98357A。
 * @return 0 成功，非 0 失败。
 */
int audio_init(void);

/**
 * @brief 释放 I2S 资源（停播）。
 */
void audio_deinit(void);

/**
 * @brief 设置输出采样率（部分 MP3 文件需要切换）。
 */
int audio_set_sample_rate(uint32_t sample_rate);

/**
 * @brief 播放 SD 卡中的音频文件。(F-10)
 *        支持 .wav 与 .mp3，依据扩展名自动选择解码路径。
 * @param path 绝对路径，如 "/sdcard/audio/hello.mp3"
 * @return 0 成功，非 0 失败（文件不存在/解码错误）。
 */
int audio_play_file(const char *path);

/** 向后兼容别名（任务清单命名） */
#define audio_play_mp3(p) audio_play_file(p)

/**
 * @brief 停止当前播放（供按键打断用）。
 */
void audio_stop(void);

/**
 * @brief 是否正在播放。
 */
bool audio_is_playing(void);

/**
 * @brief TEMP 2026-08-23 测试音（验证后移除）：生成 440Hz/1s 正弦波。
 *        SD 卡未挂载时用于验证功放链路。
 */
void audio_play_test_tone(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_AUDIO_PLAYER_H */
