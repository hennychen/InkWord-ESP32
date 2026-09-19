/**
 * @file console_cmd.h
 * @brief 串口命令模拟按键 —— 大屏开发期主验证通道
 *
 * 无按键硬件阶段（BS_NAV_* 宏 -1），串口单字符命令经 button_inject
 * 注入按键事件队列，与真实按键同路径消费（主循环/编排层零差别）。
 */
#ifndef INKWORD_CONSOLE_CMD_H
#define INKWORD_CONSOLE_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

/** 启动串口命令任务（打印一次帮助；幂等）。 */
void console_cmd_init(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CONSOLE_CMD_H */
