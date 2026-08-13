/**
 * @file button_handler.h
 * @brief 按键扫描与去抖 (Task F-11)
 *
 * 50ms 定时轮询去抖，区分短按/长按(1.5s)，回调函数注册。
 * 按键：A / B / C / D / E / F（E、F 为方向键，用于 Wi-Fi 配置 UI）。
 */
#ifndef INKWORD_BUTTON_HANDLER_H
#define INKWORD_BUTTON_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_A = 0,
    BUTTON_B,
    BUTTON_C,
    BUTTON_D,
    BUTTON_E,        /**< 左方向键 */
    BUTTON_F,        /**< 右方向键 */
    BUTTON_COUNT
} button_id_t;

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SHORT_PRESS,   /**< 短按（释放时触发） */
    BUTTON_EVENT_LONG_PRESS     /**< 长按（按住达到阈值时触发） */
} button_event_t;

/**
 * 按键事件回调：参数为按键 ID 与事件类型。
 */
typedef void (*button_callback_t)(button_id_t id, button_event_t event);

/**
 * @brief 初始化按键 GPIO 与后台扫描任务。
 * @return 0 成功。
 */
int button_handler_init(void);

/**
 * @brief 注册按键事件回调。
 */
void button_register_callback(button_callback_t cb);

/**
 * @brief 判断某个按键当前是否被按下（去抖后电平）。
 */
bool button_is_pressed(button_id_t id);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BUTTON_HANDLER_H */
