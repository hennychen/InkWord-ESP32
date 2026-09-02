/**
 * @file button_handler.h
 * @brief 五向导航按键扫描与去抖 (Task F-11)
 *
 * 50ms 定时轮询去抖，区分短按/长按(1.5s)。
 * 按键：上 / 下 / 左 / 右 / 中 + SET / RST 两个侧键
 * （五向导航开关模块，2026-08 取代 6 独立按键；七键共用 COM）。
 *
 * 事件交付（T0.2，修 C2）：扫描任务只入队（FreeRTOS 队列，深度 16，
 * 满则丢最旧），消费方在主任务 loop 经 button_wait 取出后处理——
 * 回调不再由扫描任务直接触发（原模式下 on_button 在扫描任务上下文
 * 执行渲染+刷新，阻塞期间进按丢失；队列化后刷新阻塞期间事件排队，
 * 扫描永不间断）。button_register_callback 保留为遗留声明（不再触发）。
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
 * @deprecated T0.2 队列化：回调不再由扫描任务触发（遗留声明兼容
 *             保留），事件统一经 button_wait 由主任务消费。
 */
typedef void (*button_callback_t)(nav_key_t id, button_event_t event);

/**
 * @brief 初始化按键 GPIO 与后台扫描任务。
 * @return 0 成功。
 */
int button_handler_init(void);

/**
 * @brief 注册按键事件回调（遗留兼容，T0.2 后不再触发，见文件头）。
 */
void button_register_callback(button_callback_t cb);

/**
 * @brief 等待一个按键事件（主任务消费侧，T0.2）。
 * @param id 输出：按键 ID；ev 输出：事件类型。
 * @param timeout_ms 等待超时（0 = 立即返回；建议主循环 100ms 节拍）。
 * @return true 取到一个事件；false 超时（输出参数未写）。
 *
 * 事件内含扫描任务侧的先后顺序（FIFO）；连续快速按压在刷新阻塞
 * 期间排队不丢（深度 16，满丢最旧并告警）。
 */
bool button_wait(nav_key_t *id, button_event_t *ev, uint32_t timeout_ms);

/**
 * @brief 判断某个五向键当前是否被按下（去抖后电平）。
 */
bool button_is_pressed(nav_key_t id);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BUTTON_HANDLER_H */
