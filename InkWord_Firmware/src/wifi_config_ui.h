/**
 * @file wifi_config_ui.h
 * @brief Wi-Fi 配置页面 UI（扫描 / 选择 / 密码输入 / 连接）
 *
 * 全屏向导式 UI，通过五向导航按键操作：
 *   上/下 = 上下导航, 左/右 = 左右导航, 中 = 输入/确认,
 *   左短按 = 列表页退出, 中长按 = 返回, 左长按 = 密码快删
 *
 * 交互流程：
 *   列表页(扫描结果) → 选 AP → 密码页(软键盘) → 连接 → 结果 → 自动退出
 *
 * 线程模型：独立 FreeRTOS 任务处理所有 UI 逻辑；
 *           按键事件通过队列非阻塞转发，不影响按键扫描任务。
 *
 * T2.2 页面路由接入（跨任务妥协）：g_wifi_ui_page 仅承担栈顶按键
 * 占位（on_button 非阻塞入队），render/enter=NULL（首帧由任务异步
 * 自绘）；退出在任务上下文置位，页栈归位由主 loop 回收 pop_if。
 */
#ifndef INKWORD_WIFI_CONFIG_UI_H
#define INKWORD_WIFI_CONFIG_UI_H

#include "button_handler.h"
#include "page_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Wi-Fi 配置 UI 模块（创建事件队列与后台 UI 任务）。
 *        在 app_main 中 wifi_manager_init 之后调用一次。
 */
void wifi_config_ui_init(void);

/**
 * @brief 配置页当前是否在前台（接管按键）。
 */
bool wifi_config_ui_is_active(void);

/**
 * @brief 进入 Wi-Fi 配置页（触发首次扫描）。
 *        可在无凭据时自动调用，或长按中键手动调用。
 */
void wifi_config_ui_enter(void);

/**
 * @brief 按键转发接口。配置页激活时，由栈顶路由分发调用。
 *        非阻塞（仅入队），绝不卡住按键扫描任务。
 */
void wifi_config_ui_on_button(nav_key_t id, button_event_t event);

/** 页面路由实例（T2.2）：render/enter=NULL 自管（任务异步自绘），
 *  on_button 入队转发；栈归位由 main loop 回收 */
extern const page_t g_wifi_ui_page;

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_WIFI_CONFIG_UI_H */
