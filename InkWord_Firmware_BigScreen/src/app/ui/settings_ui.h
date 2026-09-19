/**
 * @file settings_ui.h
 * @brief 大屏精简版设置页（Phase A+B 后续）
 *
 * 菜单 [系统] 组「设置」项进入。栈串联制：设置页入栈菜单之上，
 * 退出=pop 回菜单。
 *
 * 精简版设置项（大屏 Phase A+B）：
 *   发音 [开关] / 字号 [标准|大字|特大] / 音量 [0-100] / 关于
 *
 * 完整设置项（后续阶段）：震动/单词大小/粗细/测验/屏幕方向/面板/快捷键
 */
#ifndef INKWORD_SETTINGS_UI_H
#define INKWORD_SETTINGS_UI_H

#include <stdbool.h>
#include "button_handler.h"
#include "page_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 进入设置页（菜单「设置」项入口）。同步绘制并全刷；已激活幂等忽略。 */
void settings_ui_enter(void);

/** 按键转发接口（激活期间由 main 按键回调调用，同步处理）。 */
void settings_ui_on_button(nav_key_t id, button_event_t event);

/** T1.4 页面协议实例（enter=settings_ui_enter；经 page_router_push 入栈）。 */
extern const page_t g_settings_ui_page;

/* ---- 取值 API（门控层，任何模块可调；惰性缓存，无 init）---- */

/** 发音开关（set_audio，默认开）：haptic/发音/提示音门控。 */
bool settings_audio_enabled(void);

/** 音量（set_vol）：0~100 步进 10，默认 75。 */
int settings_volume(void);

/** 音量 setter：钳 0~100，NVS 持久化。 */
void settings_volume_set(int v);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_SETTINGS_UI_H */
