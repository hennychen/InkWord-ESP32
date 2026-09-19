/**
 * @file standby_page.h
 * @brief 大屏待机页 —— 引文轮换（5 分钟无操作进入，任意键退出）
 *
 * 《传习录》引文逐时轮换（quotes_app 24 条表；RTC 未同步期用
 * 开机小时数取模，语义=每次上电轮换起点不同）；32px 点阵居中，
 * 出处右下署名。
 */
#ifndef INKWORD_STANDBY_PAGE_H
#define INKWORD_STANDBY_PAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 渲染待机页整帧并全刷（进入时调一次；页内无轮换动画）。 */
void standby_page_render(void);

/**
 * 自治钟当前秒（esp_timer 单调相对秒，无 RTC 校时域）。
 * learning_state 今日统计/评分时间戳的公共时基（小屏同导出位）。
 */
int64_t standby_time_now(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_STANDBY_PAGE_H */
