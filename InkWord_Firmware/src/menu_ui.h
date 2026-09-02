/**
 * @file menu_ui.h
 * @brief 快捷菜单 UI（功能菜单：收藏/模式/配网/门户/LAN/设备信息/按键说明）
 *
 * 顶层覆盖层（与 wifi_config_ui 同级语义）：长按中键（学习页/待机页）
 * 进入，激活期间独占七键。菜单启动子功能（配网/AP/LAN/收藏浏览）时
 * 先自我退出再调既有入口函数（「先 exit 后 enter」纪律，避免双激活）。
 * 设计文档：docs/MENU_DESIGN.md（v1.0 2026-08-23 定稿）。
 *
 * 并发模型：无阻塞操作（与 wifi_config_ui 为 Wi-Fi 扫描建独立任务不同），
 * 按键回调上下文同步处理+绘制（main.cpp 学习页渲染同上下文，无帧缓冲
 * 并发）。不建任务不建队列，无需 init（状态静态零初始化）。
 *
 * 按键（菜单激活时独占，长按全部忽略防误触）：
 *   上/下短按 = 移动选择（循环滚动）   中短按 = 确认/进入
 *   SET 短按 = 返回上级（主菜单层=退出菜单）
 *   RST 短按 = 任意层级直接退出回学习页/待机页
 */
#ifndef INKWORD_MENU_UI_H
#define INKWORD_MENU_UI_H

#include <stdbool.h>
#include "button_handler.h"
#include "page_router.h"  /* T1.4：page_t（g_menu_ui_page 导出） */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 进入功能菜单（长按中键入口）。同步绘制主菜单并全刷；
 *        已激活时幂等忽略。
 */
void menu_ui_enter(void);

/**
 * @brief 菜单当前是否激活（接管按键；exit 即清）。
 */
bool menu_ui_is_active(void);

/**
 * @brief 按键转发接口。菜单激活时由 main 按键回调调用（同步处理）。
 */
void menu_ui_on_button(nav_key_t id, button_event_t event);

/**
 * @brief T1.4 页面协议实例（enter=menu_ui_enter；经 page_router_push
 *        入栈，长按中键入口由 main.cpp 调用）。
 */
extern const page_t g_menu_ui_page;

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_MENU_UI_H */
