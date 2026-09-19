/**
 * @file menu_ui.h
 * @brief 大屏精简版菜单 UI（Phase A+B 后续）
 *
 * 长按中键进入，菜单项：
 *   [学习] 模式切换 / 收藏 / 墨封 / 错词本
 *   [系统] 设备信息
 *
 * 栈串联制：菜单入栈，子功能页入栈其上，退出=pop 回菜单。
 * 终结型动作（模式确认）直接 exit 回 base。
 */
#ifndef INKWORD_MENU_UI_H
#define INKWORD_MENU_UI_H

#include <stdbool.h>
#include "button_handler.h"
#include "page_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 进入功能菜单（长按中键入口）。同步绘制主菜单并全刷；已激活时幂等忽略。 */
void menu_ui_enter(void);

/** 按键转发接口。菜单激活时由 main 按键回调调用（同步处理）。 */
void menu_ui_on_button(nav_key_t id, button_event_t event);

/** T1.4 页面协议实例（enter=menu_ui_enter；经 page_router_push 入栈）。 */
extern const page_t g_menu_ui_page;

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_MENU_UI_H */
