/**
 * @file button_handler.h
 * @brief 导航按键扫描与去抖 —— 自绘板 ADC 分压版（小屏 Task F-11
 *        同名 API）
 *
 * 按键：自绘 PCB 三按键，分压网络汇总 GPIO19（ADC2_CH8）；物理键→
 * 导航键映射与 ADC 窗口阈值均在 board_config.h（阈值待实测修正，
 * 串口 a 命令诊断）。ADC 不可用时降级串口命令输入。
 *
 * 事件交付（与小屏 T0.2 队列化同构）：扫描任务只入队（FreeRTOS
 * 队列深度 16，满丢最旧），消费方在主任务经 button_wait 取出处理；
 * button_inject 供串口命令通道注入同一队列（真实/模拟按键同路径）。
 *
 * 时序：50ms 轮询 + 连续 2 采样一致去抖（100ms）；长按 1.5s 阈值
 * （30 采样）触发时一次性发出，释放时未触发长按则发短按。
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
    NAV_CENTER,      /**< 中键：确认 / 发音 */
    NAV_SET,         /**< SET 侧键：揭晓（闪卡翻义），长按收藏 */
    NAV_RST,         /**< RST 侧键：回到第一条，长按错词本进出 */
    NAV_KEY_COUNT
} nav_key_t;

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SHORT_PRESS,   /**< 短按（释放时触发） */
    BUTTON_EVENT_LONG_PRESS     /**< 长按（按住达到阈值时触发） */
} button_event_t;

/**
 * @brief 初始化 ADC 按键与后台扫描任务（ADC 失败自动降级，不阻塞）。
 * @return 0 成功；-1 队列/任务创建失败。
 */
int button_handler_init(void);

/**
 * @brief 单周期 ADC 采样（4 次均值；与扫描判键同源，供串口诊断）。
 * @return 12bit 原始均值（0~4095）；-1 = ADC 不可用。
 */
int button_adc_sample(void);

/**
 * @brief 等待一个按键事件（主任务消费侧）。
 * @param id 输出：按键 ID；ev 输出：事件类型。
 * @param timeout_ms 等待超时（0 = 立即返回；建议主循环 100ms 节拍）。
 * @return true 取到一个事件；false 超时（输出参数未写）。
 */
bool button_wait(nav_key_t *id, button_event_t *ev, uint32_t timeout_ms);

/**
 * @brief 判断某个导航键当前是否被按下（去抖后电平；无物理键映射
 *        的导航键恒 false）。
 */
bool button_is_pressed(nav_key_t id);

/**
 * @brief 注入一个按键事件（console_cmd 串口命令通道用）——与真实
 *        扫描事件同队列同路径，主循环无差别消费。
 * @note  队列满时丢最旧（与扫描侧溢出策略一致）。
 */
void button_inject(nav_key_t id, button_event_t ev);

/**
 * @brief 挂起后台扫描（ADC2/WiFi 互斥：lan_portal 会话起 WiFi 前调，
 *        扫描任务软门挂起不再采样 GPIO19=ADC2_CH8）。
 */
void button_scan_pause(void);

/**
 * @brief 恢复后台扫描（WiFi stop 后调；含 ADC 通道重配——ADC2 仲裁
 *        恢复的保险，失败降级串口命令输入）。
 */
void button_scan_resume(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_BUTTON_HANDLER_H */
