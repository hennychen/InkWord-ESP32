/**
 * @file lan_portal.h
 * @brief LAN 图片门户页（产品化合并，2026-09-16）
 *
 * 从 bigscreen-lan 实验台（src/lan/lan_image.c，run87-110 实证链）抽取
 * 传图核心，以 page_router 页面协议接入学习闭环固件（BIGSCREEN_APP）：
 * 菜单 [系统]→发送图片 → SoftAP + captive portal → 手机浏览器选图
 * （浏览器侧 canvas 变换/FS 抖动，lan_page.h run99 页面零改动复用）→
 * 流式解包进 epd_gfx 画布 → 高优先级任务 GC16 全驱。
 *
 * ADC2/WiFi 硬互斥（GPIO19=ADC2_CH8）的会话级解法：enter 暂停按键
 * 扫描（button_scan_pause），exit 恢复（button_scan_resume + 通道
 * 重配）。LAN 会话激活期间 app_main 待机被抑制（屏电不能断）。
 */
#ifndef BIGSCREEN_LAN_PORTAL_H
#define BIGSCREEN_LAN_PORTAL_H

#include <stdbool.h>

#include "page_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** LAN 会话激活（页面在覆盖栈上：WiFi/httpd 在跑）——app_main 待机
 *  抑制用（会话期屏电不能断，传图无按键交互不能靠 idle 计时判活跃） */
bool lan_portal_active(void);

/** LAN 门户页（menu_ui act 挂载：page_router_push(&g_lan_portal_page)） */
extern const page_t g_lan_portal_page;

#ifdef __cplusplus
}
#endif
#endif /* BIGSCREEN_LAN_PORTAL_H */
