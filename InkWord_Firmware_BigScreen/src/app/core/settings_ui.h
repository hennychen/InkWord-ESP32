/**
 * @file settings_ui.h
 * @brief 设置桩（大屏核心闭环阶段，仅音频门控）
 *
 * 小屏 settings_ui 承载音频/显示/网络等 NVS 设置读写面。
 * 大屏核心闭环无音频，study_mode_machine 的 speak 动作经
 * settings_audio_enabled() 门控 —— 桩恒 false：speak 整段跳过，
 * 学习序列其余动作零影响。
 *
 * 替换点：音频阶段以完整版（或 settings 精简版）覆盖本头 +
 * app_stubs.c 删实现。
 */
#ifndef INKWORD_SETTINGS_UI_H
#define INKWORD_SETTINGS_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 桩：恒返回 false（音频关闭，speak 动作跳过） */
bool settings_audio_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SETTINGS_UI_H */
