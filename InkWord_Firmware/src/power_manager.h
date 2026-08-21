/**
 * @file power_manager.h
 * @brief SoC 深睡与定时唤醒电源管理 (P5，PRD V2.1 §8.1 功耗指标架构)
 *
 * 双模式语义（解决 PRD"待机功耗 <5µA"与"待机页 5 分钟引文轮换"的互斥）：
 *   - 交互模式（现状）：loop 1s tick、引文轮换、后台心跳照常；
 *   - 深睡模式：屏保持最后一帧（墨水屏双稳态驻留零功耗）；时钟/引文
 *     冻结（唤醒后自治钟 RTC 差分恢复 + 联网 HTTP Date 校准兜底）；
 *     仅中键（GPIO21，RTC 域）或 RTC 定时器可唤醒，唤醒 = 重启走
 *     setup 分流（ESP_SLEEP_WAKEUP_TIMER → 静默心跳会话，不碰屏）。
 *
 * 唤醒源：
 *   - 中键 ext1 唤醒（低电平触发，唤醒原因 ESP_SLEEP_WAKEUP_EXT1）：
 *     用户交互恢复。本构建链（Arduino core 2.0.17 / IDF 4.4.7）的
 *     S3 仍支持 ext1；IDF 5.x 起 S3 ext1 被移除，P6 框架迁移时需换
 *     esp_deep_sleep_enable_gpio_wakeup / ESP_SLEEP_WAKEUP_GPIO；
 *   - RTC TIMER（PM_HEARTBEAT_PERIOD_S，默认 2h）：静默心跳会话
 *     （Wi-Fi 快连 → HTTP Date 校时 → 注册/上报 flush/心跳/OTA 检查
 *     → 回睡），云端设备列表保活 + 错词上报低延迟 + OTA 低延迟。
 *
 * 入睡全流程见 power_enter_sleep()（状态落盘 → 时钟交接 → 外设收口
 * → COG 深睡锁电荷 → 射频关断 → 总线引脚锁存 → 挂唤醒源 → 睡）。
 */
#ifndef INKWORD_POWER_MANAGER_H
#define INKWORD_POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
/* CONFIG_IDF_TARGET_* 经 platformio.ini build_flags -D 注入（两环境均定义），
 * 无需包含 sdkconfig.h */
#include "esp_sleep.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 无操作自动入睡阈值（分钟）：最后一次按键后无交互即睡；
 * build_flags 可覆盖（上机验证期可临时改 1 分钟观察） */
#ifndef PM_SLEEP_TIMEOUT_MIN
#define PM_SLEEP_TIMEOUT_MIN  10
#endif

/* TIMER 心跳唤醒周期（秒）：静默会话频率（云端保活 + 数据上报） */
#ifndef PM_HEARTBEAT_PERIOD_S
#define PM_HEARTBEAT_PERIOD_S (2 * 3600)
#endif

/* 心跳会话 Wi-Fi 快连超时（秒）：失败静默回睡（路由器离线场景，
 * 不重试不闪屏） */
#define PM_WAKE_WIFI_TIMEOUT_S  10

/**
 * @brief 电源管理初始化（setup 最早调用，先于一切外设初始化）。
 *        读取/记录唤醒原因；释放上次深睡遗留的 EPD 总线引脚锁存
 *        （gpio_hold 越过深睡存活，不释放则 SPI 无法重新控制 CS）。
 * @return 本次启动的唤醒原因（TIMER = 静默心跳会话入口）。
 */
esp_sleep_wakeup_cause_t power_init(void);

/**
 * @brief 本次启动是否由用户按键唤醒（中键 ext1）。调用方据此吞掉
 *        唤醒键的幻影按键事件并给确认震动。
 */
bool power_woke_by_button(void);

/**
 * @brief 记录用户活动（on_button 顶部调用），刷新无操作计时起点。
 */
void power_note_activity(void);

/**
 * @brief 主循环周期调用：无操作超时且无禁睡条件（配网 UI / LAN 接收页
 *        / AP 门户激活）时入睡；入睡后不返回。
 */
void power_maybe_sleep(void);

/**
 * @brief 入睡全流程（不返回）。
 * @param timer_s RTC 定时唤醒秒数；0 = 仅按键唤醒。
 *        外设关断段仅在本会话外设已初始化时执行（静默心跳会话里
 *        屏/SD/音频从未初始化，跳过并沿用睡前已收口的状态）。
 */
void power_enter_sleep(uint32_t timer_s);

/**
 * @brief 标记本会话外设（屏/SD/音频/学习状态）已初始化，
 *        normal boot 路径 epd_driver_init 之后调用一次。
 */
void power_mark_periph_online(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_POWER_MANAGER_H */
