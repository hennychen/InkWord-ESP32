/**
 * @file word_card_ui.h
 * @brief 单词卡片/学习页渲染族（P2 巨石拆分：自 main.cpp 迁出）
 *
 * 职责：word-card/qa/poem 版式绘制、释义分页、状态栏/底部标签、
 * 屏幕方向生效链、跟读评测屏显——base 页 render 分流的内容层。
 * 版式宏（UI_*）与局刷策略为模块内部实现，不入对外接口；静态
 * 状态（s_last_mode 局刷判定/释义页游标）随之迁入（单一定义点）。
 */
#ifndef INKWORD_WORD_CARD_UI_H
#define INKWORD_WORD_CARD_UI_H

#include <stdbool.h>
#include "study_mode_machine.h"   /* study_mode_t / pron_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 学习页渲染入口（base_render 分流/翻词/翻页/模式切换调） */
void ui_render_word(study_mode_t mode, int index);

/** 状态栏：模式名+序号+分隔线（chat_mode 首帧 chat_page_enter 复用） */
void ui_draw_status(study_mode_t mode);

/** 跟读评测三态屏显（pron_task 驱动；内容区局刷） */
void ui_render_pron(pron_state_t st, int total, const char *engine);

/** LAN 直传整帧后强制下次全刷（GFX previous 失配；主 loop 回收行调） */
void ui_force_full_refresh_next(void);

/** 字号档变更后的排版失效（settings_ui 退出路径调：全刷+页游标归零） */
void ui_force_font_refresh(void);

/** 屏幕方向生效链（setup 恢复 / settings_ui 即改即调；幂等不绘制） */
void ui_apply_rotation(void);

/** 字号自适应：从 start_size 逐级降到能放进 max_w（quiz 注入用） */
int ui_fit_font(const char *text, int start_size, int max_w);

/** 单词字号偏好（set_word）→ fit 起步档（quiz 注入用） */
int ui_word_start_size(void);

/** 上下键词内翻释义页（true=已消费；base 按键编排调） */
bool ui_mean_page_step(int dir);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WORD_CARD_UI_H */
