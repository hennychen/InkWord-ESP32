/**
 * @file study_mode_machine.h
 * @brief 三种学习模式状态机 (Task F-16)
 *
 * 模式：闪卡(FLASH) / 听写(DICTATION) / 复习(REVIEW)。
 * 语义动作由五向导航键映射：上下=翻词，中=发音，SET=揭晓/确认，
 * RST=回第一条；长按下=循环切换模式。
 */
#ifndef INKWORD_STUDY_MODE_MACHINE_H
#define INKWORD_STUDY_MODE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODE_FLASH = 0,      /**< 闪卡：看词猜义 */
    MODE_DICTATION,      /**< 听写：听音拼写 */
    MODE_REVIEW,         /**< 复习：SRS 到期词 */
    MODE_COUNT
} study_mode_t;

/**
 * @brief 初始化状态机，默认进入闪卡模式。
 */
void study_mode_init(void);

/**
 * @brief 获取当前模式。
 */
study_mode_t study_mode_current(void);

/**
 * @brief 循环切换到下一个模式。
 * @return 切换后的模式。
 */
study_mode_t study_mode_switch_next(void);

/**
 * @brief 直接设置模式。
 */
void study_mode_set(study_mode_t mode);

/**
 * @brief 获取当前模式的显示名称（用于 UI）。
 */
const char *study_mode_name(study_mode_t mode);

/**
 * @brief 在当前模式下处理“上一条/下一条/确认/发音”等语义动作。
 *        由按键事件经模式映射后调用。
 * @param action 0=prev 1=next 2=confirm(闪卡翻义/听写提交) 3=speak
 */
void study_mode_handle_action(int action);

/**
 * @brief 释义当前是否显示（默认 true；SET 翻义切换遮蔽/揭晓自测）。
 *        翻页/切模式后自动回到显示状态。
 */
bool study_mode_is_revealed(void);

/**
 * @brief 光标重置到当前模式第一条并重绘（RST 侧键）。
 */
void study_mode_reset_cursor(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_STUDY_MODE_MACHINE_H */
