/**
 * @file quiz_ui.h
 * @brief 快速测验视图（v1.2 T2.2 建块 / T1.2 自 main.cpp 迁出，修 A1）
 *
 * 题池重映射 + 渲染 + 作答编排 + T3 音频辅助（word_audio_path 等
 * 与测验强耦合随迁）；出题核心 quiz_session 纯 C 不动（native-test
 * 验证）。核心域词条索引 [0,n) 经 s_quiz_pool 重映射到真词索引——
 * 泛化约束：核心不碰 learning_state / word_parser。
 *
 * 生命周期对齐临时视图先例（browse_mode 同款）：quiz_ui_start 由
 * 菜单 act_quiz 在 study_mode_enter_quiz 成功后调用；渲染经
 * ui_render_word 的 MODE_QUIZ 分流调用 quiz_ui_render（刷新编排/
 * 局刷窗口策略留 main.cpp）；按键经 main.cpp on_button 转发
 * quiz_ui_on_button（长短按区分）。
 */
#ifndef INKWORD_QUIZ_UI_H
#define INKWORD_QUIZ_UI_H

#include "button_handler.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 单词字号适配注入（setup 一次性注入）
 *
 * main.cpp 的 ui_fit_font / ui_word_start_size 为 static 工具（学习页
 * 词卡等多处共用），经函数指针传入避免 quiz_ui 反向依赖 main 内部
 * 实现细节（T1.2 拆分约束）。
 */
void quiz_ui_set_font_fit(int (*fit_font)(const char *text, int start_size,
                                          int max_w),
                          int (*word_start_size)(void));

/** 进入测验会话（study_mode_enter_quiz 成功后由菜单 act_quiz 调用）：
 *  题池构造 → 核心 start → 首帧渲染 + T3 首题自动播 */
void quiz_ui_start(void);

/** 测验视图绘制（ui_render_word 的 MODE_QUIZ 分流入口；刷新编排
 *  留调用方） */
void quiz_ui_render(void);

/** 按键处理（main.cpp on_button 的 MODE_QUIZ 转发；长短按区分） */
void quiz_ui_on_button(nav_key_t id, button_event_t event);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_QUIZ_UI_H */
