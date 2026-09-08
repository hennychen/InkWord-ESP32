/**
 * @file offline_stub_wifi_lan.c
 * @brief WIFI/LAN 轴桩（INKWORD_FEATURE_WIFI=0 / _LAN=0，开源通用化 Phase 1）
 *
 * 顶替 wifi_manager.c / wifi_config_ui.c / ble_provision.cpp /
 * lan_display_server.cpp / lan_proto.c 五个编译单元。头文件原样保留
 * ——include 既有头保证签名严格一致。
 *
 * 桩语义（返回值走既有降级路径，业务文件零改动）：
 *   - wifi_has_saved_credentials=false → setup 不再自动 push 配网
 *     portal（main.cpp 现有判断直接短路）；
 *   - wifi_is_connected / wifi_softap_active / lan_server_is_active
 *     恒 false → power_manager 禁睡条件全部放行、study_mode 预检拒绝；
 *   - g_lan_page / g_portal_page / g_wifi_ui_page：**on_button 必须
 *     为返回 false 的真实函数**——page_router_dispatch_button 对
 *     NULL on_button 是吞键不弹栈（死页），返回 false 触发统一
 *     pop+render_top（用户经出厂长按 LEFT/RIGHT 误入时任意键退出）。
 *     符号经 lan_display_server.h / wifi_config_ui.h 的 extern "C"
 *     声明获得 C 链接，本 .c 桩直接同名定义即可正确顶替。
 *
 * 纪律：被顶替模块新增公开 API 时必须同步本桩（链接错误兜底提醒）。
 */
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "ble_provision.h"
#include "lan_display_server.h"
#include "page_router.h"

/* ---- wifi_manager.h ---- */

int wifi_manager_init(void)  { return 0; }                        /* STUB：setup 不检查返回值，静默 */
int wifi_connect(const char *ssid, const char *password)          /* STUB */
{
    (void)ssid; (void)password;
    return -1;
}
bool wifi_is_connected(void)            { return false; }         /* STUB：全局无网态 */
void wifi_disconnect(void)              { }                       /* STUB */
void wifi_radio_off(void)               { }                       /* STUB：power 收口空操作 */
int wifi_scan(wifi_ap_record_t *results, int max)                 /* STUB */
{
    (void)results; (void)max;
    return 0;
}
bool wifi_has_saved_credentials(void)   { return false; }         /* STUB：setup 不开配网 portal */
int wifi_start_softap(void)             { return -1; }            /* STUB */
void wifi_stop_softap(void)             { }                       /* STUB */
bool wifi_softap_active(void)           { return false; }         /* STUB */
int wifi_connect_async(const char *ssid, const char *password)    /* STUB */
{
    (void)ssid; (void)password;
    return -1;
}
bool wifi_get_sta_ip(char *buf, size_t len)                       /* STUB */
{
    (void)buf; (void)len;
    return false;
}
wconn_state_t wifi_connect_state(void)   { return WCONN_IDLE; }    /* STUB：无异步连接（wifi_config_ui/lan 被裁，防御面） */

/* ---- wifi_config_ui.h ---- */

void wifi_config_ui_init(void)          { }                       /* STUB */
bool wifi_config_ui_is_active(void)     { return false; }         /* STUB：loop 页回收短路 */
void wifi_config_ui_enter(void)         { }                       /* STUB */
void wifi_config_ui_on_button(nav_key_t id, button_event_t event) /* STUB */
{
    (void)id; (void)event;
}

/* ---- ble_provision.h ----（BLE 配网本就默认关，防御性顶替） */

int ble_provision_init(void)            { return -1; }            /* STUB */
bool ble_provision_ready(void)          { return false; }         /* STUB */
void ble_adv_update_state(void)         { }                       /* STUB */

/* ---- lan_display_server.h ---- */

int lan_server_start(void)              { return -1; }            /* STUB */
bool lan_server_is_running(void)        { return false; }         /* STUB */
bool lan_server_is_active(void)         { return false; }         /* STUB */
void lan_server_enter_receive_page(void){ }                       /* STUB */
void lan_server_leave_receive_page(void){ }                       /* STUB */
void lan_portal_enter(void)             { }                       /* STUB */
void lan_portal_exit(void)              { }                       /* STUB */
bool lan_portal_take_auto_exit(void)    { return false; }         /* STUB：loop 回收短路 */
bool lan_display_drain_frame(void)      { return false; }         /* STUB：无帧可刷 */

/* ---- 桩 page 实例（误入任意键退出；见头注释 on_button 铁律） ---- */

static void stub_render(void) { }   /* 防御性空渲染：render_top 零开销返回 */

static bool stub_anykey_exit(nav_key_t id, button_event_t event)
{
    (void)id; (void)event;
    return false;   /* dispatch 统一 pop+render_top 回上级 */
}

const page_t g_lan_page =    { "lan_rx",  stub_render, stub_anykey_exit, NULL, NULL, true };
const page_t g_portal_page = { "portal",  stub_render, stub_anykey_exit, NULL, NULL, true };

const page_t g_wifi_ui_page = { "wifi_config", stub_render, stub_anykey_exit,
                                NULL, NULL, true };
