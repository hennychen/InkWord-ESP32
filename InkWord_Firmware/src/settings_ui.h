/**
 * @file settings_ui.h
 * @brief 设置页覆盖层（v1.2 T2.5，MENU_DESIGN 二期位兑现）
 *
 * 镜像 menu_ui 覆盖层范式：enter/is_active/on_button 三件套，无 init
 * （静态零初始化）、无阻塞（按键回调上下文同步绘制）。菜单 [系统] 组
 * 「设置」项进入（先 menu_ui_exit 后 enter，双激活防线同菜单子功能）。
 *
 * 行项：每日新词量（±5 循环，与 daily_plan 共用 set_daily 键）/ 发音
 * [开关] / 震动 [开关] / 字号 [标准|大字] / 测验快答 [开关]（v1.5
 * T5.1：2×2 方向直选，默认关=纵列基线；T3 听音题恒直选不受此键控）。
 *
 * 取值 API 与 UI 分层：settings_audio/haptic_enabled 供 haptic.c /
 * ui_sfx.c / study_mode_machine.c 门控（纯 NVS 惰性缓存，无 UI 依赖，
 * 按键高频路径零 flash 读）；settings_font_mode 供 main.cpp 排版宏与
 * reader_engine 默认字号档修正；settings_quiz_grid 供 main.cpp 测验
 * 按键路由与版式分派。
 *
 * NVS 键（"inkword" 命名空间追加，不动既有键）：
 *   set_daily u8（daily_plan 定义）/ set_audio u8 / set_haptic u8 /
 *   set_font u8 / set_quizgrid u8——均默认开/标准（键缺失=默认，
 *   不写默认值）。
 */
#ifndef INKWORD_SETTINGS_UI_H
#define INKWORD_SETTINGS_UI_H

#include <stdbool.h>
#include "button_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 进入设置页（菜单「设置」项入口）。同步绘制并全刷；已激活幂等忽略。 */
void settings_ui_enter(void);

/** 设置页当前是否激活（接管按键；exit 即清）。 */
bool settings_ui_is_active(void);

/** 按键转发接口（激活期间由 main 按键回调调用，同步处理）。 */
void settings_ui_on_button(nav_key_t id, button_event_t event);

/* ---- 取值 API（门控层，任何模块可调；惰性缓存，无 init）---- */

/** 发音开关（set_audio，默认开）：haptic/发音/提示音门控。 */
bool settings_audio_enabled(void);

/** 震动开关（set_haptic，默认开）：haptic_event 入口门控。 */
bool settings_haptic_enabled(void);

/** 字号档（set_font）：0=标准（档位默认）/ 1=大字（释义+阅读默认 +1 级）。 */
int settings_font_mode(void);

/** 测验快答（set_quizgrid，v1.5 T5.1）：false=纵列（P1 基线） /
 *  true=2×2 方向直选。T3 听音题恒直选（中键留给重播），不受此键控。 */
bool settings_quiz_grid(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SETTINGS_UI_H */
