/**
 * @file refresh_scheduler.h
 * @brief 刷新调度器 (Task F-13)
 *
 * 核心职责：自动计数局部刷新次数，达到阈值后强制一次全刷清除残影，
 * 对上层屏蔽 EPD 全刷/局刷切换细节。
 */
#ifndef INKWORD_REFRESH_SCHEDULER_H
#define INKWORD_REFRESH_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 矩形区域 */
typedef struct {
    int x, y, w, h;
} Rect;

/**
 * @brief 初始化调度器，设置局刷阈值。
 */
void refresh_scheduler_init(uint8_t partial_threshold);

/**
 * @brief 提交一次刷新请求。
 *        调度器内部决定走局刷还是全刷；达到阈值会自动清屏+全刷。
 * @param area 目标矩形（竖屏面板坐标，全屏请传 {0,0,EPD_WIDTH,EPD_HEIGHT}）。
 * @param data 像素数据；为 NULL 时表示清屏。
 */
void refresh_submit(Rect area, const uint8_t *data);

/**
 * @brief 强制立即执行一次全屏清残影刷新。
 */
void refresh_force_full(void);

/**
 * @brief 通知调度器发生了一次外部全刷（如 LAN 直传整帧显示），
 *        局刷计数归零（全刷本身即清残影，无需额外动作）。
 */
void refresh_notify_full_done(void);

/**
 * @brief GFX 差分局刷（epd_gfx_flush_window）前置检查（全局阈值版）。
 *        未达阈值：局刷计数 +1，返回 false，调用方继续局刷；
 *        达到阈值：执行清屏全刷清残影并归零，返回 true ——
 *        此时屏幕已被清白，调用方必须整屏重绘后走全刷。
 */
bool refresh_gfx_before_partial(void);

/**
 * @brief 同 refresh_gfx_before_partial，但使用调用方指定的阈值。
 *        供不同页面采用不同防残影节奏（如待机页大时钟 20 次/学习页 8 次）。
 * @param threshold 本次局刷前允许的最大局刷次数。
 */
bool refresh_gfx_before_partial_n(uint8_t threshold);

/**
 * @brief 当前自上次全刷以来的局刷次数。
 */
uint8_t refresh_partial_count(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_REFRESH_SCHEDULER_H */
