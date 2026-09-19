/**
 * @file schedule.h
 * @brief 课程表桩（大屏核心闭环阶段）
 *
 * 小屏 v1.6 课程表（周计划自动切组，NVS sched_cfg/sched_last 两键）
 * 不在大屏核心闭环范围。learning_state.c 评分路径调用
 * schedule_try_activate()（跨日自动激活），桩返回 1 = 无需激活：
 * 行为等价于课程表未启用，评分/切组主路径零影响。
 *
 * 替换点：后续阶段迁移 schedule.c 时以完整版覆盖本头 +
 * app_stubs.c 删除对应实现。
 */
#ifndef INKWORD_SCHEDULE_H
#define INKWORD_SCHEDULE_H

#ifdef __cplusplus
extern "C" {
#endif

/** 桩：空操作（配置装载由完整版提供） */
void schedule_init(void);

/**
 * 桩：恒返回 1（无需激活）。
 * 语义见小屏版：0=已激活 1=无需激活（同日/已手动/未启用）-1=无有效槽位。
 */
int schedule_try_activate(void);

/** 桩：空操作（手动切组通知） */
void schedule_on_manual_switch(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SCHEDULE_H */
