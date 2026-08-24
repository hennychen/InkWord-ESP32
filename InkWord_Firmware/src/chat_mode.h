/**
 * @file chat_mode.h
 * @brief AI 语音对话模式状态机 (P2B，2026-08-24)
 *
 * AI 后端化红线：设备只录音上传 / 下载播放，ASR+LLM+TTS 全在后端
 * （POST /api/device/chat 单端点闭环，协议冻结见 docs/AI_CHAT_MODE.md）。
 * 五态循环 idle → recording(≤10s，VAD 断或中键说完即发) → uploading
 * → thinking(下载回复 MP3) → playing → idle；语音优先、屏幕克制：
 * 环路内仅状态区局刷（ui_render_chat），三色面板（无局刷能力）
 * 零渲染纯语音+震动。
 *
 * 按键：中=开始/发送/打断重说，长按中或 RST=请求退出（由 main 编排
 * 层执行 study_mode_exit_chat）；haptic：录音起一短震、回复到两短震、
 * 网络失败一长震（回 idle 不退模式）。电源零改动：每次按键
 * power_note_activity 自然续期。
 */
#ifndef INKWORD_CHAT_MODE_H
#define INKWORD_CHAT_MODE_H

#include <stdbool.h>

#include "button_handler.h"   /* nav_key_t / button_event_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 对话状态（ui_render_chat 屏显依据） */
typedef enum {
    CHAT_STATE_IDLE = 0,      /**< 待录音（中键开始；屏驻留末句回复） */
    CHAT_STATE_RECORDING,     /**< 录音中（VAD 自动断 / 中键说完即发） */
    CHAT_STATE_UPLOADING,     /**< 上传 WAV 等后端 ASR+LLM+TTS */
    CHAT_STATE_THINKING,      /**< 下载回复 MP3 */
    CHAT_STATE_PLAYING,       /**< 播放回复（中键打断重说） */
    CHAT_STATE_NETFAIL,       /**< 网络不可用（短暂提示后回 idle） */
} chat_state_t;

/** 末句回复缓冲上限（后端 EnforceLimits ≤260 字符 + 余量） */
#define CHAT_REPLY_MAX (256)

/**
 * @brief 进入对话模式（快捷菜单项触发；study_mode_enter_chat 预检
 *        通过后调用）：复位状态并启动常驻对话任务（6KB 栈，模式
 *        生命周期内轮询触发位驱动轮次，不阻塞按键回调）。
 */
void chat_mode_enter(void);

/**
 * @brief 请求退出：置取消位 + 停播 + 清模式标志；常驻任务在最近的
 *        循环边界静默收尾（HTTP 阻塞期最长 45s 后自然退出，渲染
 *        调用全部跳过；chat_tmp.mp3 由任务退出路径删除，深睡重启
 *        后残留文件由下一轮下载覆盖，无害）。
 */
void chat_mode_request_exit(void);

/**
 * @brief 模式内按键统一入口（main.on_button 在 MODE_CHAT 时全量转发）。
 * @return true 按键已消费；false 请求退出（长按中 / RST，调用方执行
 *         study_mode_exit_chat + 恢复渲染）。
 */
bool chat_mode_on_button(nav_key_t id, button_event_t event);

/** 模式是否激活（任务生命周期域）。 */
bool chat_mode_is_active(void);

/** 当前状态（ui_render_current 进入首帧取用）。 */
chat_state_t chat_mode_state(void);

/** 末句回复文本（空串=尚无回合；屏显驻留用）。 */
const char *chat_mode_reply(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CHAT_MODE_H */
