/**
 * @file haptic.h
 * @brief 震动马达触觉反馈 (PRD 5.4 事件表，2026-08-20)
 *
 * GPIO41 开关量驱动（MOS 管，无调速），事件以时长区分；
 * 全部非阻塞：esp_timer 一次性回调关断，调用方（按键扫描任务 /
 * Arduino loop）零等待。事件表与 PRD_V2.1 §5.4 严格对齐：
 *   按键按下 20ms / 自评提交 30ms / 模式切换 50ms /
 *   错误边界 100ms / 跟读通过双短震（P4 预留）。
 * 音效反馈（扬声器短提示音）待 MAX98357 接线验证后另行落地。
 */
#ifndef INKWORD_HAPTIC_H
#define INKWORD_HAPTIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 触觉事件（语义见 PRD_V2.1 §5.4 表） */
typedef enum {
    HAPTIC_KEYPRESS = 0,  /**< 按键按下瞬间：20ms 短震 */
    HAPTIC_REVIEW,        /**< 自评提交（左/右短按）：30ms 短震 */
    HAPTIC_MODE,          /**< 进入/退出模式（含错词本）：50ms 短震 */
    HAPTIC_ERROR,         /**< 错误操作/边界拒绝：100ms 长震 */
    HAPTIC_PASS,          /**< 跟读评测通过：两下短震（P4 预留） */
    HAPTIC_FAIL,          /**< 跟读评测失败：一下长震（P4 预留） */
} haptic_event_t;

/**
 * @brief 初始化马达 GPIO 与关断定时器（setup 中先于按键扫描任务）。
 * @return 0 成功；<0 定时器创建失败（马达保持关断，不影响主流程）。
 */
int haptic_init(void);

/**
 * @brief 触发语义事件（时长映射见枚举注释）。
 *        重叠事件直接重启当前脉冲（后到优先）。
 */
void haptic_event(haptic_event_t ev);

/**
 * @brief 单脉冲震动 on_ms 毫秒（非阻塞，超时自动关断）。
 */
void haptic_pulse(uint16_t on_ms);

/**
 * @brief 双短震：on_ms 震 -> 停 gap_ms -> 再震 on_ms（评测通过用）。
 */
void haptic_pulse2(uint16_t on_ms, uint16_t gap_ms);

/**
 * @brief 立即关断马达（P5 深睡前收口）：停定时器 + 极性感知关断，
 *        防低有效模块在深睡中常震；未 init 时安全（GPIO 空操作）。
 */
void haptic_off(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_HAPTIC_H */
