/**
 * @file lan_display_server.cpp
 * @brief 局域网直传显示服务实现（方案 B 测试）
 *
 * 端点：
 *   GET  /              内嵌上传页：Canvas 渲染 + Floyd-Steinberg 抖动，
 *                       在浏览器端产出面板物理 PWxPH 1bpp 帧（中文经浏览器
 *                       字体渲染，几何按当前面板运行期注入 —— Phase 6
 *                       多面板）；文本/图片均支持旋转（自动/0/90/180/270°），
 *                       可选横屏排布满幅显示
 *   POST /api/display   T2.3 协议 v2 帧头 + v1/v1.5 双长度兼容（分类
 *                       与头解析见 lan_proto.h 谱系）：v2 = 8B 头
 *                       （'I''W' + ver2 + bpp + W/H 大端）+ body，
 *                       错尺寸/坏版本 400 明确文案；v1.5 无头双平面
 *                       epd_fb_total() 字节（[0]=B/W bit=1 白 + [1]
 *                       =accent bit=1 置色，v2.1 2026-08-29 accent 随
 *                       desc.accent_rgb 泛化）与 v1 无头单平面
 *                       epd_fb_size() 字节（余平面补零，旧脚本兼容）
 *                       按长度判别照收；均 epd_full_refresh 整帧直刷。
 *                       BW 面板 v1.5/v2-color 自然退化 v1
 *   GET  /api/device-info  设备能力 JSON（proto_ver:2）：panel/gfx
 *                       几何、fb_size/fb_total、plane_count、
 *                       accent_rgb 三元组；ETag 协商缓存（304），
 *                       外部工具/上传页动态建画布数据源
 *   GET  /wifi          Wi-Fi 配网页：扫描列表选 SSID + 密码输入（两种模式均可用）
 *   GET  /api/wifi/scan|status、POST /api/wifi/connect（异步连接，状态轮询）
 *   GET  /api/decks     词书列表（v1.3 T3.4 App 换书）：id/name/count/active
 *   POST /api/deck/upload?id=&name=&count=&type= 词书 LAN 直传
 *                        （body=words.json 文本，流式落 SD decks/<id>/ +
 *                        manifest 登记重扫；type 可选版式 word-card/
 *                        qa-card/poem-card，v1.5 T5.3 编辑器推送兑现预留）
 *   POST /api/deck/active  切换词书（body {"id":""}；复用菜单切书编排
 *                        deck_flow_switch：NVS+重载+状态作废+进度隔离）
 *   GET  /api/stats     今日统计/连续天数（lr_stats 口径 + 错词/到期/收藏数）
 *                        + mac 字段（v2.0 App 绑定凭据，与注册 MAC 同源）
 *   GET  其他任意 URI   302 重定向（captive portal 探测域名 → 弹出配网页）
 *
 * 两种工作模式：
 *   STA 在线：mDNS http://inkword.local（iOS/macOS 佳，Android 兼容有限），
 *             主页右上角入口进 /wifi 直接换网；
 *   AP 配网（portal）：SoftAP InkWord-Setup + DNS 劫持（53 端口全应答 192.168.4.1）
 *             → 手机连热点后系统探测域名被重定向 → 自动弹出配网页；
 *             连接成功后自动回学习界面并关闭热点。
 *
 * 线程模型（T0.3，修 C1 终态）：handler 在 httpd 任务只收帧入
 *           双缓冲并置就绪标志，立即回 200；直刷由主任务 loop 经
 *           lan_display_drain_frame() 执行（EPD 回归单写者，刷新
 *           14.6s 不阻塞 httpd 服务其它请求）。原「httpd 直刷与
 *           按键任务渲染存在潜在竞争」由 T0.1 互斥锁兜底 + 本任务
 *           单写者化根治；「接收页激活时屏蔽学习页渲染 + 外部直刷
 *           后强制下次全刷」两道缓解保留。
 */
#include "lan_display_server.h"
#include "page_router.h"        /* display_claim/release：前台态同步（P2 注册制） */
#include "debug_log.h"
#include "epd_driver.h"
#include "lan_proto.h"        /* T2.3：v2 帧分类/头解析（纯 C，native-test） */
#include "lan_pages.h"        /* PAGE_HTML/WIFI_HTML/SCHEDULE_HTML 资产 */
#include "esp_mac.h"          /* v2.0：stats 端点 mac 字段（App 绑定凭据） */
#include "layout_profile.h"   /* 2026-08-25：TINY 档紧凑版式分派 */
#include "refresh_scheduler.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "deck_manager.h"
#include "learning_state.h"
#include "word_parser.h"
#include "storage_manager.h"
#include "gpio_config.h"      /* SD_MOUNT_POINT */
#include "schedule.h"         /* 课程表显示数据读写 */

#include "esp_http_server.h"
#include "esp_netif.h"
#include "mdns.h"
#include "esp_err.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>    /* malloc/free：页面组装与帧接收缓冲（Phase 6 运行期化） */

static const char *TAG = "LAN";

#define AP_IFACE_IP     "192.168.4.1"          /* SoftAP 默认网关 IP */
#define PORTAL_SSID     "InkWord-Setup"

static httpd_handle_t s_server = NULL;
static bool s_active = false;                  /* 接收页在前台 */

/* 整帧接收双缓冲（T0.3）：httpd 任务写入侧与主任务消费侧轮换。
 *   s_recv_idx    —— httpd 下一帧写入块（收完即翻转）
 *   s_ready_idx   —— 待主任务直刷的帧号（-1 无；覆盖旧值 = 丢旧保新）
 *   s_draining_idx—— 主任务正在直刷的帧号（-1 无；httpd 写块避开，
 *                    防 14.6s 全刷期间第三帧覆写 SPI 正在读的缓冲）
 *   s_ready_page_active —— 置就绪时接收页是否在前台（消费时页已退出
 *                    则丢弃：防止迟到的帧覆盖退出后刚绘的学习页；
 *                    STA 后台传图（页未激活直传）不受影响）
 * 状态转换均在 portMUX 临界区内（纳秒级）；帧体收写与刷屏在锁外 */
#define LAN_FRAME_NBUF 2
static uint8_t *s_frame_buf[LAN_FRAME_NBUF] = {NULL, NULL}; /* 首用分配：
                                   * epd_fb_total()（Phase 6 多面板）*/
static volatile int s_recv_idx = 0;
static volatile int s_ready_idx = -1;
static volatile int s_draining_idx = -1;
static volatile bool s_ready_page_active = false;
static portMUX_TYPE s_frame_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_portal_mode = false;             /* AP 配网门户激活 */
static bool s_portal_provision = false;        /* 无凭据配网场景（连上即自动关）；
                                                 * 有凭据时为 AP 直连模式（用户按键退出） */
static volatile bool s_dns_run = false;        /* DNS 劫持任务运行标志 */
static TaskHandle_t s_dns_task = NULL;
static TaskHandle_t s_portal_task = NULL;
static volatile bool s_portal_auto_exit = false; /* 栈串联重构：配网成功
                                                  * 自动收尾标志（任务置位，
                                                  * 主 loop 读清回收栈页） */

/* main.cpp 提供：外部直刷后强制下一次学习界面渲染走全刷 */
extern "C" void ui_force_full_refresh_next(void);
/* main.cpp 提供：切书编排（v1.3 T3.1：NVS+词库重载+状态作废+进度隔离） */
extern "C" bool deck_flow_switch(int idx);

/* ============================================================
 * HTTP handlers
 * ============================================================ */

/* 上传页几何占位符替换（Phase 6 多面板）：__PW__/__PH__ 面板物理尺寸
 * （浏览器画布与帧格式），__GW__/__GH__ GFX 几何（设备视角预览），
 * __COLOR__ 色彩能力（fb_total>fb_size 即多平面，彩色 UI 注入），
 * __ACC__ 第三色 RGB 三元组（v2.1 2026-08-29：desc.accent_rgb，
 * "255,0,0" 形式——红屏；量化调色板
 * 与文案同源）。
 * 单遍扫描就地展开；缓冲预留 64B 余量（6 个占位符均短，__ACC__
 * 最长 11B 展开增量） */
static size_t page_subst(const char *tpl, char *out, size_t out_cap,
                         const char *vals[6])
{
    static const char *const tags[6] =
        {"__PW__", "__PH__", "__GW__", "__GH__", "__COLOR__", "__ACC__"};
    size_t o = 0, i = 0;
    while (tpl[i] && o + 1 < out_cap) {
        int sub = -1;
        if (tpl[i] == '_') {
            for (int k = 0; k < 6; k++) {
                if (strncmp(tpl + i, tags[k], strlen(tags[k])) == 0) { sub = k; break; }
            }
        }
        if (sub >= 0) {
            const size_t tl = strlen(tags[sub]), vl = strlen(vals[sub]);
            if (o + vl >= out_cap) return 0;
            memcpy(out + o, vals[sub], vl);
            o += vl; i += tl;
        } else {
            out[o++] = tpl[i++];
        }
    }
    if (tpl[i]) return 0; /* 未扫完即触顶：拒绝发送残页 */
    out[o] = '\0';
    return o;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    /* 按当前面板组装页面（GET / 请求频率低，逐次分配可接受） */
    char pw[8], ph[8], gw[8], gh[8], col[8], acc[16];
    snprintf(pw, sizeof(pw), "%d", epd_panel_width());
    snprintf(ph, sizeof(ph), "%d", epd_panel_height());
    snprintf(gw, sizeof(gw), "%d", epd_gfx_width());
    snprintf(gh, sizeof(gh), "%d", epd_gfx_height());
    snprintf(col, sizeof(col), "%d",
             epd_fb_total() > epd_fb_size() ? 1 : 0); /* 多平面=彩色 */
    {
        const uint32_t rgb = epd_panel_accent_rgb(); /* BW/异常退黑不影响
                                                      * 隐藏 UI */
        snprintf(acc, sizeof(acc), "%u,%u,%u",
                 (unsigned)((rgb >> 16) & 0xFF),
                 (unsigned)((rgb >> 8) & 0xFF),
                 (unsigned)(rgb & 0xFF));
    }
    const char *vals[6] = {pw, ph, gw, gh, col, acc};

    const size_t cap = sizeof(PAGE_HTML) + 64;
    char *page = (char *)malloc(cap);
    if (!page) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    const size_t len = page_subst(PAGE_HTML, page, cap, vals);
    httpd_resp_set_type(req, "text/html");
    /* 发送页随面板能力/版本变化（2026-08-22 彩色升级后旧缓存页致首测
     * 误报无色），禁缓存保证每次拿到当前面板的注入页 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = len > 0 ? httpd_resp_send(req, page, len)
                            : (httpd_resp_set_status(req, "500 Internal Server Error"),
                               httpd_resp_send(req, NULL, 0));
    free(page);
    return err;
}

static esp_err_t wifi_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WIFI_HTML, sizeof(WIFI_HTML) - 1);
}

/* T2.3 设备能力 JSON（proto_ver:2）：面板/GFX 几何、帧缓冲尺寸、
 * 平面数、accent_rgb 三元组。上传页与外部工具动态建画布数据源；
 * ETag = 面板注册名 + 协议版（换面板注册表或升协议 → 值变），
 * If-None-Match 命中回 304 省流量（频拉场景每次仅头部往返） */
static esp_err_t device_info_get_handler(httpd_req_t *req)
{
    char etag[56];
    snprintf(etag, sizeof(etag), "\"%s-p%d\"",
             epd_panel_desc() ? epd_panel_desc()->name : "?", LAN_PROTO_VER);
    char inm[56];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm))
            == ESP_OK && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        return httpd_resp_send(req, NULL, 0);
    }
    const size_t fb = epd_fb_size(), ft = epd_fb_total();
    const uint32_t rgb = epd_panel_accent_rgb();
    char json[192];
    snprintf(json, sizeof(json),
             "{\"panel_w\":%d,\"panel_h\":%d,\"gfx_w\":%d,\"gfx_h\":%d,"
             "\"fb_size\":%u,\"fb_total\":%u,\"plane_count\":%d,"
             "\"accent_rgb\":[%u,%u,%u],\"proto_ver\":%d}",
             epd_panel_width(), epd_panel_height(),
             epd_gfx_width(), epd_gfx_height(),
             (unsigned)fb, (unsigned)ft, ft > fb ? 2 : 1,
             (unsigned)((rgb >> 16) & 0xFF), (unsigned)((rgb >> 8) & 0xFF),
             (unsigned)(rgb & 0xFF), LAN_PROTO_VER);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=0, must-revalidate");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    static wifi_ap_record_t aps[20];
    int n = wifi_scan(aps, 20);   /* 阻塞 1~2 秒 */

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "auth",
                              aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!body) body = strdup("[]");   /* 分配失败兕底 */

    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, body, strlen(body));
    free(body);   /* cJSON_Print 分配在堆上 */
    return e;
}

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    char ip[20];
    wifi_get_sta_ip(ip, sizeof(ip));

    char body[80];
    switch (wifi_connect_state()) {
    case WCONN_CONNECTING:
        strcpy(body, "{\"state\":\"connecting\"}"); break;
    case WCONN_OK:
        snprintf(body, sizeof(body),
                 "{\"state\":\"ok\",\"ip\":\"%s\"}", ip[0] ? ip : "?");
        break;
    case WCONN_FAIL:
        strcpy(body, "{\"state\":\"fail\"}"); break;
    default:
        /* 未在连接流程中：按当前链路状态显示 */
        if (wifi_is_connected())
            snprintf(body, sizeof(body), "{\"state\":\"ok\",\"ip\":\"%s\"}", ip);
        else
            strcpy(body, "{\"state\":\"idle\"}");
        break;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, strlen(body));
}

static esp_err_t wifi_connect_post_handler(httpd_req_t *req)
{
    /* Wi-Fi 配置 UI 激活时拒绝，避免与软键盘配网流程冲突 */
    if (wifi_config_ui_is_active()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "device config ui active");
        return ESP_OK;
    }

    char body[192];
    if (req->content_len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "body too large");
        return ESP_OK;
    }
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "recv failed");
        return ESP_FAIL;
    }
    body[r] = '\0';

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "bad json");
        return ESP_OK;
    }
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));

    esp_err_t resp = ESP_OK;
    if (wifi_connect_async(ssid, pass ? pass : "") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "invalid ssid/pass");
        resp = ESP_OK;
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    cJSON_Delete(j);
    return resp;
}

/* 词书/统计端点（定义见后「词书管理与学习统计」区块；catchall 先行分发） */
static esp_err_t deck_list_get_handler(httpd_req_t *req);
static esp_err_t stats_get_handler(httpd_req_t *req);
static esp_err_t device_info_get_handler(httpd_req_t *req);

static esp_err_t schedule_get_handler(httpd_req_t *req);
static esp_err_t schedule_post_handler(httpd_req_t *req);
static esp_err_t schedule_page_handler(httpd_req_t *req);

/* GET 总入口（路径通配）：路径分发；未知路径 302（captive portal 探测域名重定向） */
static esp_err_t catchall_get_handler(httpd_req_t *req)
{
    char path[64];
    strlcpy(path, req->uri, sizeof(path));
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    if (strcmp(path, "/") == 0)                 return root_get_handler(req);
    if (strcmp(path, "/wifi") == 0)             return wifi_page_get_handler(req);
    if (strcmp(path, "/api/wifi/scan") == 0)    return wifi_scan_get_handler(req);
    if (strcmp(path, "/api/wifi/status") == 0)  return wifi_status_get_handler(req);
    if (strcmp(path, "/api/decks") == 0)        return deck_list_get_handler(req);
    if (strcmp(path, "/api/stats") == 0)        return stats_get_handler(req);
    if (strcmp(path, "/api/device-info") == 0)  return device_info_get_handler(req);
    if (strcmp(path, "/api/schedule") == 0)     return schedule_get_handler(req);
    if (strcmp(path, "/schedule") == 0)         return schedule_page_handler(req);

    /* 其余：captive portal 探测域名（connectivitycheck.gstatic.com 等）
     * 或未知路径 → 302；手机连热点后系统探测被重定向到配网页 → 自动弹出 */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location",
                       s_portal_mode
                           ? "http://" AP_IFACE_IP "/wifi"  /* 重定向到配网页 */
                           : "/");                             /* 普通未知路径回主页 */
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t display_post_handler(httpd_req_t *req)
{
    /* Wi-Fi 配置页期间拒绝，避免与其 UI 任务竞争屏幕 */
    if (wifi_config_ui_is_active()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "wifi config ui active");
        return ESP_OK;
    }
    /* T2.3 协议 v2 帧头 + v1/v1.5 双长度兼容：classify 按
     * content_len 无歧义分流（8B 头恒多 8 字节，与裸长度无碰撞）；
     * v1 单平面（余平面补零）/v1.5 双平面（[0]=B/W bit=1 白 +
     * [1]=accent）行为与历史版一致 */
    const size_t frame_bytes = epd_fb_size();
    const size_t total_bytes = epd_fb_total();
    const int plane_count = total_bytes > frame_bytes ? 2 : 1;
    const lan_frame_kind_t kind =
        lan_frame_classify((size_t)req->content_len, frame_bytes, total_bytes);
    if (kind == LAN_FRAME_REJECT) {
        char msg[144];
        snprintf(msg, sizeof(msg),
                 "body must be %u/%u (v1/v1.5 raw) or %u/%u bytes (v2, 8B header)",
                 (unsigned)frame_bytes, (unsigned)total_bytes,
                 (unsigned)(frame_bytes + 8), (unsigned)(total_bytes + 8));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }
    const bool has_hdr = (kind == LAN_FRAME_V2_BW || kind == LAN_FRAME_V2_COLOR);
    size_t recv_len;
    if (has_hdr) { /* v2：先收 8B 头校验，再按头声明的 body 长收体 */
        uint8_t hdr[8];
        int got = 0;
        while (got < 8) {
            int r = httpd_req_recv(req, (char *)hdr + got, 8 - got);
            if (r <= 0) {
                LOG_E("display upload recv header failed");
                httpd_resp_set_status(req, "500 Internal Server Error");
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "recv failed");
                return ESP_FAIL;
            }
            got += r;
        }
        const char *err = "";
        size_t body_len = 0;
        if (lan_v2_header_parse(hdr, epd_panel_width(), epd_panel_height(),
                                plane_count, &body_len, &err) != 0) {
            const int fw = (hdr[4] << 8) | hdr[5];
            const int fh = (hdr[6] << 8) | hdr[7];
            LOG_E("v2 header rejected: %s (frame %dx%d bpp %d, panel %dx%d planes %d)",
                  err, fw, fh, hdr[3], epd_panel_width(), epd_panel_height(),
                  plane_count);
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "v2 rejected: %s (frame %dx%d bpp %d, panel %dx%d planes %d)",
                     err, fw, fh, hdr[3], epd_panel_width(), epd_panel_height(),
                     plane_count);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, msg);
            return ESP_OK;
        }
        recv_len = body_len;
    } else {
        recv_len = (kind == LAN_FRAME_V15_COLOR) ? total_bytes : frame_bytes;
    }
    const bool color_frame =
        (kind == LAN_FRAME_V15_COLOR || kind == LAN_FRAME_V2_COLOR);

    /* T0.3：选写入块（避开主任务正在直刷的 draining 块；双缓冲下必
     * 有可用块——若候选块恰为未消费的 pending，覆盖 = 丢旧保新） */
    int widx;
    portENTER_CRITICAL(&s_frame_mux);
    widx = s_recv_idx;
    if (widx == s_draining_idx) widx ^= 1;
    portEXIT_CRITICAL(&s_frame_mux);

    if (!s_frame_buf[widx]) { /* 首用分配（epd_driver_init 后几何就绪） */
        s_frame_buf[widx] = (uint8_t *)malloc(epd_fb_total());
        if (!s_frame_buf[widx]) {
            LOG_E("frame buffer alloc failed (%u B)", (unsigned)epd_fb_total());
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "no memory");
            return ESP_FAIL;
        }
    }

    int received = 0;
    while (received < (int)recv_len) {
        int r = httpd_req_recv(req, (char *)s_frame_buf[widx] + received,
                               (int)recv_len - received);
        if (r <= 0) {
            LOG_E("display upload recv failed at %d/%d", received, (int)recv_len);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    /* v1 单平面 → 多平面面板余平面（accent）清零；v2 双平面已填满直通 */
    if (!color_frame && total_bytes > frame_bytes)
        memset(s_frame_buf[widx] + frame_bytes, 0x00, total_bytes - frame_bytes);

    /* T0.3：帧体收完即置就绪返回（直刷交主任务 lan_display_drain_frame，
     * 原三联动 epd_full_refresh + refresh_notify_full_done +
     * ui_force_full_refresh_next 语义原样迁往消费侧） */
    portENTER_CRITICAL(&s_frame_mux);
    if (s_ready_idx >= 0 && s_ready_idx != widx)
        LOG_I("frame %d pending not drained, replaced (drop-old)", s_ready_idx);
    s_ready_idx = widx;
    s_ready_page_active = s_active;
    s_recv_idx = widx ^ 1;
    portEXIT_CRITICAL(&s_frame_mux);

    LOG_I("LAN frame queued (%d bytes%s), main loop will display", received,
          color_frame ? ", color" : "");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* T0.3：主任务消费侧直刷入口（main.cpp loop 每轮调用）——有就绪帧时
 * 在主任务上下文执行原 handler 内的三联动：整帧直刷 + 局刷计数归零
 * + 学习界面下次强制全刷。EPD 单写者化后 httpd 任务不再触碰刷屏路径 */
bool lan_display_drain_frame(void)
{
    int idx;
    bool page_active;
    portENTER_CRITICAL(&s_frame_mux);
    idx = s_ready_idx;
    page_active = s_ready_page_active;
    if (idx >= 0) {
        s_ready_idx = -1;      /* 先取出独占：刷屏 14.6s 窗口内新帧照常 */
        s_draining_idx = idx;  /* 置 draining：httpd 写块避开 */
    }
    portEXIT_CRITICAL(&s_frame_mux);
    if (idx < 0) return false;

    /* 接收页激活期收的帧，消费时页已退出（用户按键退出后画面已恢复
     * 学习页）——丢弃防迟到帧覆盖；STA 后台传图（page_active=false）
     * 不受影响照常显示 */
    if (page_active && !s_active) {
        LOG_I("LAN frame dropped: receive page left before display");
        s_draining_idx = -1;
        return false;
    }

    epd_full_refresh(s_frame_buf[idx]);
    refresh_notify_full_done();   /* 外部全刷等价于残影清理，计数归零 */
    ui_force_full_refresh_next(); /* GFX previous 缓冲已失配，下次强制全刷 */
    s_draining_idx = -1;
    LOG_I("LAN frame displayed (%d bytes)", (int)epd_fb_total());
    return true;
}

/* ============================================================
 * 词书管理与学习统计（v1.3 T3.4，App LAN 直连链路）
 * ============================================================ */

/* query 值 URL 解码（%XX）：name 中文经 URL 编码传输 */
static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) {
            int hi = strchr("0123456789abcdefABCDEF", s[1]) ?
                     (s[1] <= '9' ? s[1] - '0' : (s[1] | 0x20) - 'a' + 10) : -1;
            int lo = strchr("0123456789abcdefABCDEF", s[2]) ?
                     (s[2] <= '9' ? s[2] - '0' : (s[2] | 0x20) - 'a' + 10) : -1;
            if (hi >= 0 && lo >= 0) {
                *o++ = (char)((hi << 4) | lo);
                s += 3;
                continue;
            }
        }
        *o++ = *s++;
    }
    *o = '\0';
}

static bool deck_id_valid(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > DECK_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static esp_err_t deck_list_get_handler(httpd_req_t *req)
{
    deck_manager_scan();                    /* 幂等重扫：上传后清单最新 */

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active", deck_manager_active_id());
    cJSON *arr = cJSON_AddArrayToObject(root, "decks");
    for (int i = 0; i < deck_manager_count(); i++) {
        const deck_info_t *d = deck_manager_at(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", d->id);
        cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddNumberToObject(o, "count", d->count);
        cJSON_AddItemToArray(arr, o);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, body, strlen(body));
    free(body);
    return e;
}

static esp_err_t stats_get_handler(httpd_req_t *req)
{
    /* mac（v2.0 账户绑定凭据，ADR-001 §五）：与 sync_register 同源
     * STA MAC 大写 12 hex，App LAN 发现后携此调 POST /api/me/devices/bind */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char body[288];
    snprintf(body, sizeof(body),
             "{\"mac\":\"%02X%02X%02X%02X%02X%02X\","
             "\"activeDeck\":\"%s\",\"totalWords\":%d,"
             "\"todayNew\":%d,\"todayReviews\":%d,\"streakDays\":%d,"
             "\"wrongCount\":%d,\"dueCount\":%d,\"collectedCount\":%d}",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             deck_manager_active_name(), word_parser_get_count(),
             learning_state_today_new(), learning_state_today_reviews(),
             learning_state_streak_days(), learning_state_wrong_count(),
             learning_state_due_count(), learning_state_collected_count());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, strlen(body));
}

static esp_err_t deck_upload_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 ||
        (size_t)req->content_len > 4 * 1024 * 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "empty or oversized body");
        return ESP_OK;
    }

    char query[192];
    char id[DECK_ID_MAX + 1] = "", name[40] = "", cnt[12] = "";
    char dtype[20] = "";                    /* v1.5 T5.3：版式透传 */
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        /* id 超长静默截断防御（2026-09-09）：截断后长度恰好过
         * deck_id_valid，落盘目录与调用方预期不一致（pron_test →
         * pron_te 实例）——返回 TRUNC 直接拒绝 */
        if (httpd_query_key_value(query, "id", id, sizeof(id)) ==
            ESP_ERR_HTTPD_RESULT_TRUNC) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "id too long (max 7 chars)");
            return ESP_OK;
        }
        httpd_query_key_value(query, "name", name, sizeof(name));
        httpd_query_key_value(query, "count", cnt, sizeof(cnt));
        httpd_query_key_value(query, "type", dtype, sizeof(dtype));
    }
    url_decode(id);
    url_decode(name);
    if (!deck_id_valid(id)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "id required (1-7 chars [0-9a-zA-Z_-])");
        return ESP_OK;
    }
    if (!name[0]) strlcpy(name, id, sizeof(name));
    int count = atoi(cnt);

    /* 版式白名单（T4.3 card_layout 分派键；不传走 word-card 缺省，
     * 向后兼容 T3.4 旧调用；非法值拒绝防拼错静默降级） */
    const char *ptype = NULL;
    if (dtype[0]) {
        if (strcmp(dtype, "word-card") == 0 ||
            strcmp(dtype, "qa-card") == 0 ||
            strcmp(dtype, "poem-card") == 0)
            ptype = dtype;
        else {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "type must be word-card/qa-card/poem-card");
            return ESP_OK;
        }
    }

    char dir[64], tmp[80], final[80];
    snprintf(dir, sizeof(dir), SD_MOUNT_POINT "/decks/%s", id);
    snprintf(tmp, sizeof(tmp), "%s/words.json.tmp", dir);
    snprintf(final, sizeof(final), "%s/words.json", dir);
    if (storage_mkdir_p(dir) != 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "mkdir failed (SD?)");
        return ESP_OK;
    }

    FILE *f = fopen(tmp, "w");
    if (!f) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "open failed");
        return ESP_OK;
    }

    /* 流式落盘（words.json 可达 MB 级，不占整块 RAM）；首块抽验 JSON 头 */
    char buf[1024];
    int received = 0;
    bool head_ok = false;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf, (int)sizeof(buf));
        if (r <= 0) {
            fclose(f);
            remove(tmp);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "recv failed");
            return ESP_FAIL;
        }
        if (!head_ok) {
            head_ok = (buf[0] == '{' || buf[0] == '[');
            if (!head_ok) {
                fclose(f);
                remove(tmp);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "body not json");
                return ESP_OK;
            }
        }
        fwrite(buf, 1, (size_t)r, f);
        received += r;
    }
    fclose(f);
    if (rename(tmp, final) != 0) {
        remove(tmp);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "rename failed");
        return ESP_OK;
    }

    /* manifest 登记 + 重扫（词书内容解析与切换由 App 另发 active；
     * subject 预留 NULL 走 en 缺省；payloadType v1.5 T5.3 兑现透传
     * ——编辑器推 qa/poem 卡组时 manifest 登记版式，T4.3 渲染分派
     * 依此选版式 */
    if (deck_manager_upsert(id, name, count, NULL, ptype) != 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "manifest write failed");
        return ESP_OK;
    }

    LOG_I("deck uploaded: %s (%d B, %d words)", id, received, count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t deck_active_post_handler(httpd_req_t *req)
{
    char body[96];
    if (req->content_len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "body too large");
        return ESP_OK;
    }
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) return ESP_FAIL;
    body[r] = '\0';

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "bad json");
        return ESP_OK;
    }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "id"));
    if (!id) id = "";                        /* 缺省 = 切回默认词库 */

    deck_manager_scan();
    /* 空 id 切默认（[0] 恒在）；非空 id 必须显式命中，未找到报 404——
     * 静默回退默认会擦掉用户已激活卡组（deck_flow_switch(0) 走
     * nvs_erase_key），曾致 LAN 切书后 active 莫名回 default */
    int idx = (id[0] == '\0') ? 0 : deck_manager_find_index(id);
    if (idx < 0) {
        LOG_W("deck active: id '%s' not in manifest", id);
        cJSON_Delete(j);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "deck id not found");
        return ESP_OK;
    }
    cJSON_Delete(j);

    /* 切书编排与菜单路径同源（NVS+词库重载+学习状态作废+进度隔离
     * +归位闪卡）；与按键任务的渲染竞争同 /api display 既有模型 */
    if (!deck_flow_switch(idx)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "switch failed (deck file?)");
        return ESP_OK;
    }

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"active\":\"%s\"}",
             deck_manager_active_id());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

/* ============================================================
 * 课程表 LAN Web 编辑器（/schedule 页面 + /api/schedule JSON）
 * ============================================================ */

/* GET /api/schedule：返回当前显示课表 JSON */
static esp_err_t schedule_get_handler(httpd_req_t *req)
{
    const schedule_display_t *d = schedule_display_cfg();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", d->enabled);
    cJSON_AddStringToObject(root, "title", d->title[0] ? d->title : "课程表");
    cJSON_AddNumberToObject(root, "rows", d->rows);
    cJSON_AddNumberToObject(root, "cols", d->cols);

    cJSON *days = cJSON_AddArrayToObject(root, "days");
    for (int c = 0; c < d->cols; c++)
        cJSON_AddItemToArray(days, cJSON_CreateString(d->days[c]));

    cJSON *slots = cJSON_AddArrayToObject(root, "slots");
    for (int r = 0; r < d->rows; r++)
        cJSON_AddItemToArray(slots, cJSON_CreateString(d->slots[r]));

    cJSON *grid = cJSON_AddArrayToObject(root, "grid");
    for (int r = 0; r < d->rows; r++) {
        cJSON *row = cJSON_CreateArray();
        for (int c = 0; c < d->cols; c++)
            cJSON_AddItemToArray(row, cJSON_CreateString(d->grid[r][c]));
        cJSON_AddItemToArray(grid, row);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, body, strlen(body));
    free(body);
    return e;
}

/* POST /api/schedule：更新显示课表（JSON body）并渲染到屏幕 */
static esp_err_t schedule_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || (size_t)req->content_len > 4096) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "empty or oversized body");
        return ESP_OK;
    }
    char *body = (char *)malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { free(body); return ESP_FAIL; }
    body[r] = '\0';

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "bad json");
        return ESP_OK;
    }

    schedule_display_t *d = schedule_display_cfg_mut();
    memset(d, 0, sizeof(*d));

    /* enabled */
    cJSON *j_enabled = cJSON_GetObjectItem(j, "enabled");
    d->enabled = cJSON_IsTrue(j_enabled);

    /* title */
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(j, "title"));
    if (title) strlcpy(d->title, title, sizeof(d->title));
    else strlcpy(d->title, "课程表", sizeof(d->title));

    /* rows / cols */
    cJSON *j_rows = cJSON_GetObjectItem(j, "rows");
    cJSON *j_cols = cJSON_GetObjectItem(j, "cols");
    d->rows = (j_rows && j_rows->valueint > 0) ? j_rows->valueint : 0;
    d->cols = (j_cols && j_cols->valueint > 0) ? j_cols->valueint : 0;
    if (d->rows > SCHED_DISP_MAX_ROWS) d->rows = SCHED_DISP_MAX_ROWS;
    if (d->cols > SCHED_DISP_MAX_COLS) d->cols = SCHED_DISP_MAX_COLS;

    /* days */
    cJSON *j_days = cJSON_GetObjectItem(j, "days");
    if (cJSON_IsArray(j_days)) {
        for (int c = 0; c < d->cols && c < cJSON_GetArraySize(j_days); c++) {
            const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(j_days, c));
            if (s) strlcpy(d->days[c], s, sizeof(d->days[c]));
        }
    }

    /* slots */
    cJSON *j_slots = cJSON_GetObjectItem(j, "slots");
    if (cJSON_IsArray(j_slots)) {
        for (int r2 = 0; r2 < d->rows && r2 < cJSON_GetArraySize(j_slots); r2++) {
            const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(j_slots, r2));
            if (s) strlcpy(d->slots[r2], s, sizeof(d->slots[r2]));
        }
    }

    /* grid */
    cJSON *j_grid = cJSON_GetObjectItem(j, "grid");
    if (cJSON_IsArray(j_grid)) {
        for (int r2 = 0; r2 < d->rows && r2 < cJSON_GetArraySize(j_grid); r2++) {
            cJSON *row = cJSON_GetArrayItem(j_grid, r2);
            if (!cJSON_IsArray(row)) continue;
            for (int c = 0; c < d->cols && c < cJSON_GetArraySize(row); c++) {
                const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(row, c));
                if (s) strlcpy(d->grid[r2][c], s, sizeof(d->grid[r2][c]));
            }
        }
    }

    cJSON_Delete(j);
    schedule_display_save();

    /* 渲染到屏幕 */
    schedule_draw_display_table();

    LOG_I("schedule updated via LAN: %dx%d", d->rows, d->cols);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* GET /schedule：Web 编辑页面 */
static esp_err_t schedule_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, SCHEDULE_HTML, sizeof(SCHEDULE_HTML) - 1);
}

/* ============================================================
 * 公共接口
 * ============================================================ */
static void register_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set("inkword");
        mdns_instance_name_set("InkWord EPD");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        LOG_W("mdns_init failed: %s (IP access still works)",
              esp_err_to_name(err));
    }
}

int lan_server_start(void)
{
    if (s_server) return 0;
    /* portal 模式走 AP 网络，不要求 STA 已连接 */
    if (!s_portal_mode && !wifi_is_connected()) {
        LOG_W("lan server start deferred: wifi not connected");
        return -1;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 8192;                   /* handler 内执行整帧全刷，留足调用栈 */
    cfg.max_uri_handlers = 10;               /* GET 通配 + POST×6（display/wifi/deck×2/schedule×2） */
    cfg.uri_match_fn = httpd_uri_match_wildcard;  /* 支持路径通配路由 */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        LOG_E("httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return -1;
    }

    /* GET 统一经通配分发（含 captive portal 302） */
    httpd_uri_t uri_get = {};
    uri_get.uri = "/*";
    uri_get.method = HTTP_GET;
    uri_get.handler = catchall_get_handler;
    httpd_register_uri_handler(s_server, &uri_get);

    httpd_uri_t uri_display = {};
    uri_display.uri = "/api/display";
    uri_display.method = HTTP_POST;
    uri_display.handler = display_post_handler;
    httpd_register_uri_handler(s_server, &uri_display);

    httpd_uri_t uri_conn = {};
    uri_conn.uri = "/api/wifi/connect";
    uri_conn.method = HTTP_POST;
    uri_conn.handler = wifi_connect_post_handler;
    httpd_register_uri_handler(s_server, &uri_conn);

    httpd_uri_t uri_deck_up = {};
    uri_deck_up.uri = "/api/deck/upload";
    uri_deck_up.method = HTTP_POST;
    uri_deck_up.handler = deck_upload_post_handler;
    httpd_register_uri_handler(s_server, &uri_deck_up);

    httpd_uri_t uri_deck_act = {};
    uri_deck_act.uri = "/api/deck/active";
    uri_deck_act.method = HTTP_POST;
    uri_deck_act.handler = deck_active_post_handler;
    httpd_register_uri_handler(s_server, &uri_deck_act);

    httpd_uri_t uri_sched_post = {};
    uri_sched_post.uri = "/api/schedule";
    uri_sched_post.method = HTTP_POST;
    uri_sched_post.handler = schedule_post_handler;
    httpd_register_uri_handler(s_server, &uri_sched_post);

    /* mDNS：仅 STA 在线模式注册 inkword.local（失败不影响 IP 直访）。
     * portal 模式跳过，待配网完成回 STA 后由 monitor 补注册 */
    if (!s_portal_mode) {
        register_mdns();
    }

    LOG_I("LAN display server running on port 80 (%s)",
          s_portal_mode ? "AP portal" : "http://inkword.local");
    return 0;
}

bool lan_server_is_running(void)
{
    return s_server != NULL;
}

bool lan_server_is_active(void)
{
    return s_active;
}

static bool get_sta_ip(char *buf, size_t len)
{
    return wifi_get_sta_ip(buf, len);
}

/* TINY 档 ASCII 按宽折行（2026-08-25）：URL/IP 无空格不做词边界
 * 启发，从整串起逐字符回退找可容纳前缀断行；*y 逐行前进（行距
 * 12px）。供配网页长串（LAN 地址/门户提示）在 106/112px 正文宽
 * 可读（size1 仍超宽时兜底拆两行，单字符不拆防死循环） */
static void tiny_draw_wrap_ascii(const char *s, int x, int *y, int max_w,
                                 uint16_t color)
{
    char line[40];
    while (s[0]) {
        int n = (int)strlen(s);
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        int tw, th;
        while (n > 1) {
            memcpy(line, s, (size_t)n);
            line[n] = '\0';
            epd_gfx_text_bounds(line, 1, &tw, &th);
            if (tw <= max_w) break;
            n--;
        }
        memcpy(line, s, (size_t)n);
        line[n] = '\0';
        epd_gfx_draw_text(x, *y, line, color, 1);
        *y += 12;
        s += n;
    }
}

void lan_server_enter_receive_page(void)
{
    /* portal 模式下重绘配网提示页（AP 服务后台保持） */
    if (s_portal_mode) {
        lan_portal_enter();
        return;
    }

    lan_server_start(); /* 幂等；未联网时仅提示 */

    s_active = true;
    page_router_display_claim();   /* 显示占用真相源同步（P2 注册制） */

    /* TINY 竖屏紧凑版式（2026-08-25）：原版式为 416px 宽设计值（URL
     * size3 ≈324px 超 122/128px 屏宽 2.5 倍），改标题栏 24 / size1
     * 短句 / URL 逐行折行（tiny_draw_wrap_ascii）；未联网分支不再提
     * 示键盘路径（wifi_config_ui 不适配 TINY，配网唯一通道=AP 门户） */
    if (layout_profile_get()->kind == LAYOUT_TINY) {
        int w = epd_gfx_width();
        epd_gfx_fill_screen(EPD_GFX_WHITE);
        epd_gfx_fill_rect(0, 0, w, 24, EPD_GFX_BLACK);
        epd_gfx_draw_text(8, 18, "LAN RX", EPD_GFX_WHITE, 1);

        char ip[20];
        bool has_ip = get_sta_ip(ip, sizeof(ip));
        if (lan_server_is_running() && has_ip) {
            char url[40];
            snprintf(url, sizeof(url), "http://%s", ip);
            epd_gfx_draw_text(8, 44, "Open browser:", EPD_GFX_BLACK, 1);
            int y = 64;
            tiny_draw_wrap_ascii(url, 8, &y, w - 16, EPD_GFX_BLACK);
            epd_gfx_draw_text(8, y + 8, "inkword.local", EPD_GFX_BLACK, 1);
        } else {
            epd_gfx_draw_text(8, 44, "WiFi off.", EPD_GFX_BLACK, 1);
            epd_gfx_draw_text(8, 68, "Long-press LEFT", EPD_GFX_BLACK, 1);
            epd_gfx_draw_text(8, 84, "for WiFi portal", EPD_GFX_BLACK, 1);
        }
        epd_gfx_draw_text(8, epd_gfx_height() - 24, "any key exit",
                          EPD_GFX_BLACK, 1);
        epd_gfx_flush();
        LOG_I("LAN receive page shown (tiny, server=%s ip=%s)",
              lan_server_is_running() ? "on" : "off", has_ip ? ip : "none");
        return;
    }

    /* GFX 显示层（epd_gfx_width() x height()，面板无关），ASCII
     * （FreeSans 无 CJK 字形） */
    epd_gfx_fill_screen(EPD_GFX_WHITE);

    epd_gfx_fill_rect(0, 0, epd_gfx_width(), 36, EPD_GFX_BLACK);
    epd_gfx_draw_text(16, 26, "LAN Receive", EPD_GFX_WHITE, 2);

    char ip[20];
    bool has_ip = get_sta_ip(ip, sizeof(ip));

    if (lan_server_is_running() && has_ip) {
        char url[40];
        snprintf(url, sizeof(url), "http://%s/", ip);
        epd_gfx_draw_text(16, 78, "Open in phone browser:", EPD_GFX_BLACK, 1);
        epd_gfx_draw_text(16, 124, url, EPD_GFX_BLACK, 3);
        epd_gfx_draw_text(16, 154, "or http://inkword.local", EPD_GFX_BLACK, 1);
        epd_gfx_draw_text(16, 196, "Send text / image, then any key exit",
                          EPD_GFX_BLACK, 1);
    } else {
        epd_gfx_draw_text(16, 80, "WiFi not connected.", EPD_GFX_BLACK, 2);
        epd_gfx_draw_text(16, 120, "Long press LEFT for WiFi portal,",
                          EPD_GFX_BLACK, 2);
        epd_gfx_draw_text(16, 150, "or long press CENTER (keyboard).",
                          EPD_GFX_BLACK, 2);
    }

    epd_gfx_flush();
    LOG_I("LAN receive page shown (server=%s ip=%s)",
          lan_server_is_running() ? "on" : "off", has_ip ? ip : "none");
}

void lan_server_leave_receive_page(void)
{
    s_active = false;
    page_router_display_release(); /* 与 enter/portal 同步成对清除 */
}

/* ============================================================
 * AP 配网门户（captive portal）：SoftAP + DNS 劫持 + 302 重定向
 * ============================================================ */

/* DNS 劫持：53/UDP 收到 A 查询一律应答 192.168.4.1（AP 网关）。
 * 手机连上热点后，系统 connectivity 探测域名被解析到本机 →
 * 探测请求被 HTTP 302 到 /wifi → 系统自动弹出配网页。 */
static void dns_hijack_task(void *arg)
{
    (void)arg;
    /* 2026-08-22 栈溢出勘误：两缓冲共 1072B 原在栈上，叠加 lwIP
     * socket 调用链（socket/bind/recvfrom ~2KB）超出 3072 栈金丝雀，
     * AP portal 启动即 Stack canary panic 重启循环（真机日志实证，
     * dnshijack 任务名点名）。任务单例无重入 → 挪 static（BSS），
     * 栈同时加大至 4096 双保险 */
    static char qbuf[512];
    static char rbuf[560];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        LOG_E("dns hijack: socket failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_E("dns hijack: bind 53 failed");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LOG_I("DNS hijack running (all A queries -> " AP_IFACE_IP ")");
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, qbuf, sizeof(qbuf), 0,
                         (struct sockaddr *)&from, &flen);
        if (n < 12) continue;                 /* 超时/超短包 */
        if ((uint8_t)qbuf[5] != 1) continue;  /* 仅处理 QDCOUNT=1 */

        uint8_t *b = (uint8_t *)qbuf;
        int p = 12;
        while (p < n && b[p]) p += b[p] + 1; /* 跳过 QNAME */
        p += 5;                               /* NULL + QTYPE + QCLASS */
        if (p > n) continue;
        int qlen = p;
        uint16_t qtype = (uint16_t)((b[p - 4] << 8) | b[p - 3]);

        /* 响应 = 原查询（头 + Question） + 可选应答段 */
        int rl = qlen;
        memcpy(rbuf, qbuf, qlen);
        rbuf[2] = 0x85;                        /* QR=1 AA=1 RD=1 */
        rbuf[3] = 0x80;                        /* RA=1 RCODE=0 */
        rbuf[6] = 0;
        rbuf[7] = (qtype == 1) ? 1 : 0;        /* ANCOUNT */
        if (qtype == 1) {                      /* 仅 A 查询回答 */
            static const uint8_t ans[16] = {
                0xC0, 0x0C,                    /* NAME 指针 → QNAME */
                0x00, 0x01,                    /* TYPE=A */
                0x00, 0x01,                    /* CLASS=IN */
                0x00, 0x00, 0x00, 0x1E,        /* TTL=30s */
                0x00, 0x04,                    /* RDLENGTH=4 */
                192, 168, 4, 1                 /* RDATA */
            };
            memcpy(rbuf + rl, ans, sizeof(ans));
            rl += sizeof(ans);
        }
        sendto(sock, rbuf, rl, 0, (struct sockaddr *)&from, flen);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void dns_start(void)
{
    if (s_dns_task) return;
    s_dns_run = true;
    if (xTaskCreate(dns_hijack_task, "dnshijack", 4096, NULL, 5,
                    &s_dns_task) != pdPASS) {
        s_dns_task = NULL;
        LOG_E("dns hijack task create failed");
    }
}

/* portal 监视任务：检测到连接成功 → 显示结果 → 延时关闭热点并恢复学习界面 */
static void portal_monitor_task(void *arg)
{
    (void)arg;
    while (s_portal_mode) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* 网页主动换网（WCONN_OK）：任何模式下都关闭门户；
         * 软键盘同步配网（wifi_is_connected）：仅配网场景关闭，
         * 直连模式下 STA 自动重连成功不触发退出（用户正用 AP 传图）。
         * 三条路径捕获：网页异步连接、软键盘配网、原有链路 */
        if (wifi_connect_state() != WCONN_OK &&
            !(s_portal_provision && wifi_is_connected()))
            continue;

        char ip[20] = "?";
        wifi_get_sta_ip(ip, sizeof(ip));

        if (lan_server_is_active()) {
            epd_gfx_fill_screen(EPD_GFX_WHITE);
            epd_gfx_fill_rect(0, 0, epd_gfx_width(), 36, EPD_GFX_BLACK);
            epd_gfx_draw_text(16, 26, "WiFi Connected", EPD_GFX_WHITE, 2);
            epd_gfx_draw_text(16, 84, "Device IP:", EPD_GFX_BLACK, 1);
            epd_gfx_draw_text(16, 116, ip, EPD_GFX_BLACK, 3);
            epd_gfx_draw_text(16, 170, "Portal closing...", EPD_GFX_BLACK, 1);
            epd_gfx_flush();
        }

        /* 停留窗口：让手机端轮询到 ok 并展示结果 */
        for (int i = 0; i < 8 && s_portal_mode; i++)
            vTaskDelay(pdMS_TO_TICKS(1000));

        wifi_stop_softap();   /* 回纯 STA（STA_START 回调自动重连） */
        s_dns_run = false;    /* DNS 任务随 recv 超时退出 */
        s_portal_mode = false;
        register_mdns();      /* 首次启动在 portal 模式未注册，此处补上 */

        if (lan_server_is_active())
            lan_server_leave_receive_page();

        /* 栈串联重构（2026-09-08）：原任务上下文直调 ui_render_word
         * 恢复学习界面——portal 栈化后页栈仍在（任务不动栈，单写者
         * 纪律），改置标志由主 loop 回收 pop_if+render_top（wifi 页
         * 任务回收同款）；portal 已被用户按键退出时主 loop pop_if
         * NULL 防御零动作 */
        s_portal_auto_exit = true;
        break;
    }
    s_portal_task = NULL;
    vTaskDelete(NULL);
}

void lan_portal_enter(void)
{
    if (!s_portal_mode) {
        s_portal_mode = true;
        /* 无凭据=配网场景（连上自动关）；有凭据=AP 直连场景
         * （绕开路由器隔离直传，仅用户按键退出） */
        s_portal_provision = !wifi_has_saved_credentials();
        wifi_start_softap();
        lan_server_start();   /* portal 模式跳过 STA 检查 */
        dns_start();
        xTaskCreate(portal_monitor_task, "portal", 4096, NULL, 4,
                    &s_portal_task);
    }

    s_active = true;
    page_router_display_claim();   /* 显示占用真相源同步（P2 注册制） */

    /* TINY 竖屏紧凑版式（2026-08-25）：SSID/步骤文案 size3/size1 均按
     * 416px 宽设计超算；SSID 反白强调（配网唯一关键串），URL 定宽拆
     * 「http://」+ 网关地址两行（AP 网关为编译期常量，折行器兜底） */
    if (layout_profile_get()->kind == LAYOUT_TINY) {
        int w = epd_gfx_width();
        epd_gfx_fill_screen(EPD_GFX_WHITE);
        epd_gfx_fill_rect(0, 0, w, 24, EPD_GFX_BLACK);
        epd_gfx_draw_text(8, 18,
                          s_portal_provision ? "WiFi Setup" : "AP Direct",
                          EPD_GFX_WHITE, 1);

        epd_gfx_draw_text(8, 44, "1. Connect:", EPD_GFX_BLACK, 1);
        epd_gfx_fill_rect(4, 58, w - 8, 22, EPD_GFX_BLACK);
        epd_gfx_draw_text(8, 74, PORTAL_SSID, EPD_GFX_WHITE, 1);

        epd_gfx_draw_text(8, 100, "2. Open:", EPD_GFX_BLACK, 1);
        epd_gfx_draw_text(8, 120, "http://", EPD_GFX_BLACK, 1);
        int y = 136;
        tiny_draw_wrap_ascii(AP_IFACE_IP "/", 8, &y, w - 16,
                             EPD_GFX_BLACK);

        epd_gfx_draw_text(8, epd_gfx_height() - 24, "Any key exit",
                          EPD_GFX_BLACK, 1);
        epd_gfx_flush();
        LOG_I("AP portal active (tiny %s): SSID=" PORTAL_SSID,
              s_portal_provision ? "provision" : "direct");
        return;
    }

    /* 屏幕提示页（ASCII） */
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    epd_gfx_fill_rect(0, 0, epd_gfx_width(), 36, EPD_GFX_BLACK);
    epd_gfx_draw_text(16, 26, s_portal_provision ? "WiFi Setup" : "AP Direct",
                      EPD_GFX_WHITE, 2);

    epd_gfx_draw_text(16, 74, "1. Connect phone to hotspot:",
                      EPD_GFX_BLACK, 1);
    epd_gfx_draw_text(16, 112, PORTAL_SSID, EPD_GFX_BLACK, 3);
    epd_gfx_draw_text(16, 150, "2. Open http://" AP_IFACE_IP,
                      EPD_GFX_BLACK, 1);
    epd_gfx_draw_text(16, 168, "   (send page / wifi setup)",
                      EPD_GFX_BLACK, 1);
    epd_gfx_draw_text(16, 208, "Any key exit", EPD_GFX_BLACK, 1);
    epd_gfx_flush();

    LOG_I("AP portal active (%s): SSID=" PORTAL_SSID " page=http://"
          AP_IFACE_IP "/",
          s_portal_provision ? "provision" : "direct");
}

void lan_portal_exit(void)
{
    s_active = false;
    page_router_display_release(); /* 与 enter/portal 同步成对清除 */
    if (!s_portal_mode) return;   /* 非 portal 模式：仅清前台标志 */

    s_portal_mode = false;
    s_dns_run = false;            /* DNS 任务随 recv 超时自退（≤0.5s） */
    wifi_stop_softap();           /* 回纯 STA，自动重连已保存网络 */
    register_mdns();              /* portal 启动时未注册，此处补上 */

    LOG_I("AP portal exited by user");
}

bool lan_portal_take_auto_exit(void)
{
    if (!s_portal_auto_exit) return false;
    s_portal_auto_exit = false;
    return true;
}

/* ---- 页面协议实例（栈串联重构 2026-09-08）----
 * LAN 接收页/AP portal 原为 display_claim 外部独占（不入栈），退出
 * 逻辑埋在 base_page_on_button——栈非空时不可达，是菜单「先 exit 后
 * enter」的技术强制根源。栈化收编：任意按键事件返回 false（dispatch
 * 统一 pop+render_top 回上级页，原 base 层任意键退出同语义）；
 * enter/exit 复用幂等生命周期函数；render=NULL（任务/enter 自绘整帧，
 * render_top 有 NULL 防御）；owns_display=true 与保留的 claim 机制
 * 双保险（display_busy / top_owns_display 两道渲染守卫均覆盖）。 */
static bool lan_anykey_on_button(nav_key_t id, button_event_t event)
{
    (void)id;
    (void)event;
    return false;   /* 任意键请求退出（含长按；幂等 exit 防重复收尾） */
}

const page_t g_lan_page = { "lan_rx", NULL, lan_anykey_on_button,
                            lan_server_enter_receive_page,
                            lan_server_leave_receive_page, true };

const page_t g_portal_page = { "portal", NULL, lan_anykey_on_button,
                               lan_portal_enter, lan_portal_exit, true };
