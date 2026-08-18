/**
 * @file button_handler.h
 * @brief 五向导航按键扫描与去抖 (Task F-11)
 *
 * 50ms 定时轮询去抖，区分短按/长按(1.5s)，回调函数注册。
 * 按键：上 / 下 / 左 / 右 / 中 + SET / RST 两个侧键
 * （五向导航开关模块，2026-08 取代 6 独立按键；七键共用 COM）。
 */
#ifndef INKWORD_BUTTON_HANDLER_H
#define INKWORD_BUTTON_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NAV_UP = 0,      /**< 上 */
    NAV_DOWN,        /**< 下 */
    NAV_LEFT,        /**< 左 */
    NAV_RIGHT,       /**< 右 */
    NAV_CENTER,      /**< 中键：确认 / 发音，长按进入 Wi-Fi 配置 */
    NAV_SET,         /**< SET 侧键：确认/揭晓（闪卡翻义），长按预留 SRS「记得」 */
    NAV_RST,         /**< RST 侧键：回到第一条，长按预留 SRS「忘了」 */
    NAV_KEY_COUNT
} nav_key_t;

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SHORT_PRESS,   /**< 短按（释放时触发） */
    BUTTON_EVENT_LONG_PRESS     /**< 长按（按住达到阈值时触发） */
} button_event_t;

/**
 * 按键事件回调：参数为五向键 ID 与事件类型。
 */
typedef void (*button_callback_t)(nav_key_t id, button_event_t event);

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
 * @brief 判断某个五向键当前是否被按下（去抖后电平）。
 */
bool button_is_pressed(nav_key_t id);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BUTTON_HANDLER_H */
