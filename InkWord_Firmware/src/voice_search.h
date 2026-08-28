/**
 * @file voice_search.h
 * @brief AI 语音查词状态机（2026-08-28，教材目录浏览+语音查词设计 §B2）
 *
 * chat_mode 简化版四态：idle(中键录音) → recording(≤3s，VAD 断/
 * 中键提前停) → searching(上传) → result(候选列表，中键确认跳词)。
 * 录音/上传在自建任务跑（mic_recorder_record 阻塞式，按键上下文
 * 只置标志），对齐 chat_mode 任务纪律。
 *
 * 协议：POST /api/device/voice-search?deck={活跃卡组 id}，multipart
 * WAV（同 pronunciation/chat），响应 data.candidates top-5（text/
 * meaning/cloudId/score）。AI 全后端化红线：设备只录音上传+消费候选。
 *
 * 三色屏降级（面板能力位先例）：无局刷面板本模块零渲染（ui_render_chat
 * 同款直接 return），ASR top-1 直接震动+跳转（想换词 RST 重说）；
 * 局刷面板走标准 5 候选列表。
 *
 * 生命周期对齐临时视图第五先例：voice_search_reset 由菜单 act 在
 * study_mode_enter_voice_search 成功后调用（清态+起任务）；
 * on_button 返回 false = 请求退出（main.cpp 编排层执行
 * voice_search_request_exit + study_mode_exit_voice_search）。
 */
#ifndef INKWORD_VOICE_SEARCH_H
#define INKWORD_VOICE_SEARCH_H

#include "button_handler.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 四态 */
typedef enum {
    VOICE_IDLE = 0,     /**< 待录音（中键触发） */
    VOICE_RECORDING,    /**< 录音中（≤3s / VAD 断 / 中键提前停） */
    VOICE_SEARCHING,    /**< 上传识别中 */
    VOICE_RESULT,       /**< 候选列表（上下移动/中确认/RST 重说） */
} voice_state_t;

#define VOICE_CAND_MAX  5   /** 后端 top-5 */

/** 进入视图时清态并启动任务（study_mode_enter_voice_search 成功后调用） */
void voice_search_reset(void);

/** 模式退出：请求任务收尾（main.cpp 编排层在 on_button 返回 false 后调） */
void voice_search_request_exit(void);

/** 任务是否在跑（防重入；调试/状态查询用） */
bool voice_search_is_active(void);

/** 当前状态（渲染/按键语义查询用） */
voice_state_t voice_search_state(void);

/** 当前页重绘（ui_render_current 的 MODE_VOICE 分流入口；三色零渲染） */
void voice_search_render(void);

/**
 * @brief 按键处理（main.cpp on_button 的 MODE_VOICE 转发；长短按区分）。
 * @return false = 请求退出视图（RST 长按 / 长按中）。
 */
bool voice_search_on_button(nav_key_t id, button_event_t event);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_VOICE_SEARCH_H */
