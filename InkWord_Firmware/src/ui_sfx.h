/**
 * @file ui_sfx.h
 * @brief UI 提示音 (v1.1 T1.6，2026-08-24)
 *
 * 5 个短提示音样本（16kHz/mono/PCM16/<0.3s，tools/gen_ui_sounds.py
 * 生成后拷贝到 SD /sdcard/audio/ui/）：
 *   key = 按键按下确认「滴」 / rate = 自评提交「滴答」 /
 *   mode = 模式切换「滴--」 / err = 边界拒绝「嘟-」 /
 *   stamp = 墨封落印「咚」（低频短促，2026-09-04 墨封功能）。
 *
 * 薄封装 audio_play_file 异步播放（投递即返，按键上下文安全，
 * 重按打断重播）；init 只探测样本存在性，缺失静默降级
 * （无 SD / 未拷样本不响不出错）。v1.2 起受 settings_audio
 * 开关门控（埋点处包 enabled 检查，本模块不读 NVS）。
 */
#ifndef INKWORD_UI_SFX_H
#define INKWORD_UI_SFX_H

#ifdef __cplusplus
extern "C" {
#endif

/** 提示音事件（PRD 5.4 触觉事件表的音频侧子集，语义一一对应） */
typedef enum {
    UI_SFX_KEY = 0,   /**< 按键按下确认：滴（HAPTIC_KEYPRESS 档） */
    UI_SFX_RATE,      /**< 自评提交：滴答（HAPTIC_REVIEW 档） */
    UI_SFX_MODE,      /**< 模式切换：滴--（HAPTIC_MODE 档） */
    UI_SFX_ERR,       /**< 错误边界拒绝：嘟-（HAPTIC_ERROR 档） */
    UI_SFX_STAMP,     /**< 墨封落印：咚（低频短促，缺样本静默降级） */
} ui_sfx_t;

/**
 * @brief setup 中探测 4 样本存在性（audio_init 且 SD 就绪后调用一次）。
 *        缺样本置不可用（播放空操作），LOG_W 提示缺失路径。
 */
void ui_sfx_init(void);

/**
 * @brief 异步播放提示音（audio_play_file 直接转发）。
 *        未 init / 缺样本为空操作，任何上下文可安全调用。
 */
void ui_sfx_play(ui_sfx_t ev);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_UI_SFX_H */
