/**
 * @file app_main.h
 * @brief 大屏学习闭环应用主任务（init 链 + 事件循环）
 */
#ifndef INKWORD_APP_MAIN_H
#define INKWORD_APP_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建应用主任务（main.c BIGSCREEN_APP 分支调用；幂等）。
 * @return 0 成功；-1 任务创建失败。
 */
int app_main_start(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_APP_MAIN_H */
