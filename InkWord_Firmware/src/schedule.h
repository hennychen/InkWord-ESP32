/**
 * @file schedule.h
 * @brief 课程表——周计划编排与自动激活（v1.6 新增）
 *
 * 多科目并行学习调度：用户按星期几配置今日卡组槽位（7×4），每日首次
 * 评分或深睡唤醒跨日时自动切到首个未达标卡组并注入目标量。
 *
 * 核心原则（见 docs/COURSE_SCHEDULE_DESIGN.md）：
 *   - 推荐/自动激活，非强制锁定：用户随时手动切组，当日不再自动干预
 *   - 仅跨日首次触发：同日重复评分不重复激活
 *   - 槽位配额覆盖卡组全局 goal（0=使用卡组默认）
 *   - 槽位考试日期覆盖全局 exam（0=使用全局/未设）
 *
 * NVS 存储：单 blob 一键存完整周配置（~421B），sched_cfg/sched_last
 * 两键（settings_keys.h 登记）。首次启动无键 → 默认关闭，开箱不变。
 *
 * 接线点：
 *   - main.cpp setup：schedule_init() 装载配置
 *   - main.cpp deck_flow_switch：schedule_on_manual_switch() 手动通知
 *   - learning_state.c 评分路径：schedule_try_activate() 跨日触发
 *   - menu_ui.c：课程表设置页（一级总览/二级编辑天/三级编辑槽）
 */
#ifndef INKWORD_SCHEDULE_H
#define INKWORD_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_DAYS   7      /**< 周一~周日 */
#define SCHED_SLOTS  4      /**< 每日最多 4 个时段 */

/* ---- 显示课表（LAN Web 编辑器写入，屏幕渲染读取） ---- */
#define SCHED_DISP_MAX_ROWS  8   /**< 最多 8 节课 */
#define SCHED_DISP_MAX_COLS  5   /**< 最多 5 天（周一至周五） */
#define SCHED_DISP_CELL_MAX  8   /**< 单元格内容最长 7 字符 + NUL */

/** 显示课表数据（NVS sched_disp 单 blob 存，~600B） */
typedef struct {
    bool enabled;                                        /**< 是否显示 */
    char title[16];                                      /**< 标题（如"课程表"） */
    char days[SCHED_DISP_MAX_COLS][8];                   /**< 星期标签 */
    char slots[SCHED_DISP_MAX_ROWS][8];                  /**< 节次标签 */
    char grid[SCHED_DISP_MAX_ROWS][SCHED_DISP_MAX_COLS][SCHED_DISP_CELL_MAX]; /**< 内容 */
    int  rows;                                           /**< 实际行数 */
    int  cols;                                           /**< 实际列数 */
} schedule_display_t;

/** 课程表槽位（11B） */
typedef struct {
    char     deck_id[8];    /**< 卡组短 id（""=空槽不启用） */
    uint8_t  goal;          /**< 该槽每日目标量（0=使用卡组全局 goal） */
    uint16_t exam_ymd;      /**< 考试日期 yyyymmdd（0=使用全局/未设） */
} schedule_slot_t;

/** 课程表完整配置（~421B，NVS 单 blob 存） */
typedef struct {
    bool            enabled;                            /**< 总开关 */
    schedule_slot_t day[SCHED_DAYS][SCHED_SLOTS];       /**< 7×4 槽位表 */
    char            label[SCHED_DAYS][16];              /**< 每天显示标签 */
} schedule_cfg_t;

/* ---- 生命周期 ---- */

/** 加载配置（setup 调用一次，NVS 无键则默认关闭） */
void schedule_init(void);

/* ---- 配置读写 ---- */

/** 获取配置指针（只读） */
const schedule_cfg_t *schedule_cfg(void);

/** 获取可写配置指针（修改后需调 schedule_save） */
schedule_cfg_t *schedule_cfg_mut(void);

/** 持久化配置到 NVS */
void schedule_save(void);

/* ---- 自动激活 ---- */

/**
 * @brief 尝试自动激活课程表推荐卡组。
 *
 * 仅跨日首次调用生效（同日重复调用返回 1 无动作）。手动切组后当日
 * 不再自动干预（manual_override 标记，次日重置）。
 *
 * @return 0=已激活（切组完成）；1=无需激活（同日/已手动/未启用）；
 *         -1=无有效槽位（全达标/全空/卡组缺失）
 */
int schedule_try_activate(void);

/** 手动切组通知（deck_flow_switch 编排中调用） */
void schedule_on_manual_switch(void);

/* ---- 查询 ---- */

/** 获取今日指定槽位（NULL=空槽/越界/未启用） */
const schedule_slot_t *schedule_today_slot(int slot_idx);

/** 今日有效槽位数（0~4） */
int schedule_today_slot_count(void);

/** 课程表是否启用 */
bool schedule_is_enabled(void);

/** 获取今天星期几（0=周一~6=周日；时钟未同步返回 -1） */
int schedule_today_wday(void);

/**
 * @brief 渲染课程表到屏幕（从 NVS 读取显示数据，全屏绘制后 flush）。
 *        4.2 寸三色屏 400x300 网格布局，利用红/黑/白三色分区。
 *        无数据时显示占位提示。
 */
void schedule_draw_display_table(void);

/* ---- 显示课表数据读写（LAN Web 编辑器用） ---- */

/** 获取显示课表指针（只读） */
const schedule_display_t *schedule_display_cfg(void);

/** 获取显示课表可写指针（修改后需调 schedule_display_save） */
schedule_display_t *schedule_display_cfg_mut(void);

/** 持久化显示课表到 NVS */
void schedule_display_save(void);

/** 显示课表是否有数据 */
bool schedule_display_has_data(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SCHEDULE_H */
