/**
 * @file lan_display_server.h
 * @brief 局域网直传显示服务（方案 B 测试）
 *
 * 在设备上启动轻量 HTTP 服务器（esp_http_server）+ mDNS（inkword.local），
 * 手机/电脑连入同一 Wi-Fi 后用浏览器打开设备网页，将文本或图片由浏览器
 * Canvas 渲染为 240x416 竖屏 1bpp 位图后 POST 到 /api/display，
 * 固件整帧全刷直接显示（中文渲染由浏览器字体完成，绕过固件无 CJK 字形限制）。
 *
 * 交互：长按 F 键进入接收页（屏幕显示访问 URL），任意按键退出回到学习界面。
 */
#ifndef INKWORD_LAN_DISPLAY_SERVER_H
#define INKWORD_LAN_DISPLAY_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include "button_handler.h"   /* nav_key_t/button_event_t（page_t 依赖） */
#include "page_router.h"      /* 栈串联重构：g_lan_page/g_portal_page 导出 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 HTTP 服务器与 mDNS（幂等；需 Wi-Fi 已连接）。
 * @return 0 成功或已启动；<0 未连接或启动失败。
 */
int lan_server_start(void);

/**
 * @brief 服务器是否已启动。
 */
bool lan_server_is_running(void);

/**
 * @brief LAN 接收页是否在前台（接管按键、屏蔽学习页渲染）。
 */
bool lan_server_is_active(void);

/**
 * @brief 进入接收页：确保服务已启动并绘制 URL 提示页（整屏全刷）。
 */
void lan_server_enter_receive_page(void);

/**
 * @brief 退出接收页（仅清除前台标志，画面重绘由调用方负责）。
 */
void lan_server_leave_receive_page(void);

/**
 * @brief 进入 AP 配网门户：开启 SoftAP（InkWord-Setup）+ DNS 劫持 +
 *        captive portal 重定向，手机连热点后自动弹出 Wi-Fi 配网页；
 *        无凭据时连上自动关；有凭据时为 AP 直连模式（绕开路由器
 *        隔离直传），仅用户按键退出（幂等，重绘提示页）。
 */
void lan_portal_enter(void);

/**
 * @brief 退出 portal（用户按键触发）：关热点回 STA 自动重连；
 *        非 portal 模式调用仅清除前台标志（幂等）。
 */
void lan_portal_exit(void);

/**
 * @brief T1.4 页面协议实例（栈串联重构 2026-09-08）：LAN 接收页/
 *        AP portal 原为 display_claim 外部独占（不入栈），退出逻辑
 *        埋在 base 按键层（栈非空时不可达）；栈化后任意键退出走
 *        dispatch 统一 pop+render_top 回上级。经 page_router_push
 *        入栈（enter/exit 复用上方幂等生命周期函数）。
 */
extern const page_t g_lan_page;

/** @brief AP portal 页面协议实例（g_lan_page 同构） */
extern const page_t g_portal_page;

/**
 * @brief 取用 portal 自动退出标志（读清）：portal_monitor_task 检测
 *        配网成功自动收尾时置位；主 loop 检查后回收 portal 栈页
 *        （page_router_pop_if + render_top，wifi_config_ui s_active
 *        主 loop 回收同款——页栈单写者纪律，任务上下文不动栈）。
 * @return true = 本次检测到自动退出，调用方应回收栈页
 */
bool lan_portal_take_auto_exit(void);

/**
 * @brief 消费一帧待刷的 LAN 直传帧（T0.3 主任务投递）。
 *
 * httpd 任务收帧后仅置就绪标志即返回；本函数由主任务 loop 每轮
 * 调用，有待刷帧时执行 epd_full_refresh + refresh_notify_full_done
 * + ui_force_full_refresh_next 三联动（EPD 单写者，httpd 上下文
 * 零 EPD 直调）。接收页已退出的迟到帧直接丢弃。
 *
 * @return true 本轮刷了一帧；false 无待刷帧（零开销路径）。
 */
bool lan_display_drain_frame(void);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_LAN_DISPLAY_SERVER_H */
