/**
 * @file screen_aging_test.h
 * @brief 屏幕老化诊断测试接口
 */
#ifndef SCREEN_AGING_TEST_H
#define SCREEN_AGING_TEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 进入屏幕老化诊断测试模式
 */
void screen_aging_test_enter(void);

/**
 * @brief 退出屏幕老化诊断测试模式
 */
void screen_aging_test_exit(void);

/**
 * @brief 测试模式按键处理
 * @param key_id 按键 ID
 * @param event 事件类型 (1=短按, 2=长按)
 * @return true 如果事件被消费
 */
bool screen_aging_test_on_button(int key_id, int event);

/**
 * @brief 测试是否激活
 */
bool screen_aging_test_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_AGING_TEST_H */
