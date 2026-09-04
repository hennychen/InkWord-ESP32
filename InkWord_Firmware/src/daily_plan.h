/**
 * @file daily_plan.h
 * @brief 每日学习计划与学习策略（v1.2 T2.4 起，v1.5 T5.5 按组扩展）
 *
 * 薄模块：不新增持久化表——目标量 NVS u8 键按卡组分派（v1.2 预留
 * 「goal 键加卡组后缀」兑现：默认组 "set_daily" 兼容既有键位，其余
 * "sd_<id>"，lr_st_<id> 同先例），完成度走 learning_state：全局
 * lr_stats（ST01）保留今日/连续口径，按组分子读 SD01 表
 * （learning_state_deck_today_new）；跨日结算沿用 stats_roll，
 * 本模块零状态零 init、无 RTOS 依赖。
 *
 * 考试倒计时（T5.5）：NVS u32 "set_exam" = 目标日 yyyymmdd（0=未设）。
 * 设置以「N 天后」表达（免三段日期编辑），存换算后的绝对 ymd——
 * 自治钟重启不失真；days_left = civil 差（已过/未设/未同步归 0），
 * urgent（≤7 天）触发到期视图 horizon 放宽（main.cpp 注入，
 * 日期反推优先清账）。
 */
#ifndef INKWORD_DAILY_PLAN_H
#define INKWORD_DAILY_PLAN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 指定卡组的每日新词目标量（NVS 键按组分派，默认 20）。
 * @param deck_id 卡组短 id（NULL/""=默认组）。
 * @return 5~100 之间 5 的倍数（库外值/无键均回落默认）。
 */
int daily_plan_goal_deck(const char *deck_id);

/**
 * @brief 活跃卡组每日目标量（goal_deck(deck_manager_active_id()) 薄壳，
 *        设置页/概况页既有调用点签名不变）。
 */
int daily_plan_goal(void);

/**
 * @brief 设置活跃卡组目标量（钳位 5~100、步进 5 归整；NVS 即时持久化）。
 */
void daily_plan_set_goal(int n);

/**
 * @brief 今日任务完成判据（T5.5 按组口径 + 2026-09-04 墨封边界）：
 *        当前组新学达标且无到期词。
 * @return due_count == 0 且（deck_today_new(活跃组) >= goal()，或
 *         active_new_count == 0——剩余可学新词全墨封，goal 永不可达，
 *         可学新词耗尽即达标兜底）。
 */
bool daily_plan_done(void);

/* ---- 考试倒计时（v1.5 T5.5，科目级学习策略） ---- */

/**
 * @brief 距考试剩余天数。
 * @return 0 = 未设置/时钟未同步/已过期；否则 1~99。
 */
int exam_days_left(void);

/**
 * @brief 设置考试日期：n 天后（n=0 清除；钳位 1~99）。
 *        时钟未同步时拒绝写入（自治钟无基准，倒计时无意义）。
 */
void exam_set_days(int n);

/**
 * @brief 冲刺窗口判据（1 ≤ days_left ≤ 7）：到期视图 horizon 放宽
 *        触发点（learning_state_set_due_horizon，main.cpp 消费）。
 */
bool exam_urgent(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_DAILY_PLAN_H */
