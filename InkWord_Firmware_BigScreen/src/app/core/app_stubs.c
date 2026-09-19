/**
 * @file app_stubs.c
 * @brief 非核心闭环依赖桩实现（大屏核心闭环阶段）
 *
 * 桩头（schedule.h / cjk_font_sd.h / settings_ui.h）的空实现集中地：
 * 保持被移植源（learning_state / word_parser / cjk_text /
 * study_mode_machine）最小改动，各模块后续阶段迁移时删除对应
 * 函数即可（见各桩头替换点注释）。
 *
 * storage_manager 桩已由 SPIFFS 实现替换（storage_manager.c，
 * 阅读器阶段 2026-09-16）。
 */
#include "cjk_font_sd.h"
#include "schedule.h"

#include <stddef.h>   /* NULL（原 storage_manager.h 传递包含，删除后显式补） */

/* ---- schedule.h ---- */
void schedule_init(void) { /* 桩：无课程表配置 */ }

int schedule_try_activate(void) { return 1; /* 无需激活（未启用语义） */ }

void schedule_on_manual_switch(void) { /* 桩：无自动激活干预 */ }

/* ---- cjk_font_sd.h ---- */
const uint8_t *cjk_font_sd_lookup_level(uint32_t cp, int level)
{
    (void)cp; (void)level;
    return NULL; /* 子集未装载：级联回退主集/占位框 */
}
