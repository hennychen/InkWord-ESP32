/**
 * @file chat_ui.h
 * @brief AI 对话屏显（P2B 建块 / T1.3 自 main.cpp 迁出，修 A1）
 *
 * Siri 球动画 + 五态状态页绘制（chat_mode 任务驱动）。函数名沿用
 * 迁出前（ui_render_chat / ui_chat_anim_tick）——chat_mode.c 与
 * main.cpp 引用面零改动。
 */
#ifndef INKWORD_CHAT_UI_H
#define INKWORD_CHAT_UI_H

#include "chat_mode.h"   /* chat_state_t / CHAT_STATE_* */

#ifdef __cplusplus
extern "C" {
#endif

/** 状态页绘制（chat_mode set_state 回调 + ui_render_current 首帧分流）：
 *  内容区局刷，环路内禁全刷红线 */
void ui_render_chat(chat_state_t st, const char *text);

/** THINKING 涟漪帧推进（chat_task 读流循环周期调用，内部 600ms 节拍） */
void ui_chat_anim_tick(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CHAT_UI_H */
