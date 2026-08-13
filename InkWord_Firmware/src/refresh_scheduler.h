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
 * @param area 目标矩形（全屏请传 {0,0,EPD_WIDTH,EPD_HEIGHT}）。
 * @param data 像素数据；为 NULL 时表示清屏。
 */
void refresh_submit(Rect area, const uint8_t *data);

/**
 * @brief 强制立即执行一次全屏清残影刷新。
 */
void refresh_force_full(void);

/**
 * @brief 当前自上次全刷以来的局刷次数。
 */
uint8_t refresh_partial_count(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_REFRESH_SCHEDULER_H */
