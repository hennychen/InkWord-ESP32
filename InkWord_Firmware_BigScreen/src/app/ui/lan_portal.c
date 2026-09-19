/**
 * @file lan_portal.c
 * @brief LAN 图片门户页实现（产品化合并，2026-09-16）
 *
 * 与实验台 lan_image.c（NOT BIGSCREEN_APP 构建，bring-up 诊断保留）的
 * 关键架构差异：
 *   1. 显示复用 app 侧管线：无独立 hl/epd_init——上传流解包直写
 *      epd_gfx 1bpp 画布。上传 1bit 流 bit=1=白，与 draw_bitmap 的
 *      bit=1 画 color 语义天然对齐：整屏先铺黑底（fill_screen BLACK），
 *      白像素逐行 draw_bitmap(WHITE)。4bpp gray16/fs2 流按 FS 抖动
 *      二值化（run100 兜底移植，流式行版：本屏 GC16 直传中间灰
 *      塌白 §15 且 1bpp 画布无物理灰阶，抖动纹理保留灰度感；
 *      2026-09-17 起替代旧 nibble>=8 阈值硬切）。
 *   2. ADC2/WiFi 硬互斥的会话级解法：enter 时 button_scan_pause()
 *      （扫描软门挂起，不再采样 GPIO19=ADC2_CH8），exit 时 resume
 *      （含通道重配，WiFi 停后 ADC2 仲裁恢复的保险）。
 *   3. 会话生命周期 = 页面生命周期：enter 起（WiFi start + DNS 劫持
 *      + httpd start），exit 收（httpd_stop + dns_stop + wifi_stop）；
 *      netif/event_loop/esp_wifi_init 首会话一次建设，之后 start/stop
 *      复用。NVS 凭据（lanwifi 命名空间，run110）跨会话自动重连 STA。
 *   4. 驱屏纪律（run97）：调用 epd_draw_base/hl_update 的任务必须与
 *      feed 线程同优先级 configMAX_PRIORITIES-1——httpd 任务只做
 *      网络 I/O + 收包写画布（不驱屏），上传完成投递作业给常驻
 *      portal 任务执行；信息页重绘同任务串行（busy 期间跳过，
 *      消除与 httpd 收包写画布的并发）。
 *   5. 流式收包零大缓冲：行对齐分块 recv（1bit 行 240B / 4bpp 行
 *      960B），收满一行解一行——不再预占 1MB PSRAM（合并后稳态
 *      PSRAM 已 ~6.7MB 贴 8MB 上限，lan_image 的整块缓冲方案不可行）。
 *
 * busy 期间 LAN 页按键全吞（上传/驱屏中防退出竞态——双任务同时驱屏
 * 是灾难场景）；中键短按退出（pop → render_top 恢复菜单页）。
 */
#include "lan_portal.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "arpa/inet.h"
#include "netinet/in.h"
#include "sys/socket.h"
#include "unistd.h"

#include "lwip/ip_addr.h"

#include "button_handler.h"
#include "cjk_text.h"
#include "epd_gfx.h"
#include "lan_page.h"
#include "page_router.h"

static const char *TAG = "lan_portal";

#define IMG_W 1920
#define IMG_H 1080
#define RAW_ROW (IMG_W / 8)          /* 240：1bit 上传行字节（MSB first，bit1=白） */
#define FB_ROW (IMG_W / 2)           /* 960：4bpp 上传行字节（偶 x 低半字节，15=白） */
#define RAW_BYTES (RAW_ROW * IMG_H)  /* 259200 */
#define FB_BYTES (FB_ROW * IMG_H)    /* 1036800 */
#define LAN_SSID "InkWord-BigScreen"
#define PORTAL_BUILD "app-portal-r1"

/* ---- 会话状态（enter/exit 在 app_main 上下文；标志跨任务 volatile） ---- */
static volatile bool s_session = false;    /* 页面在栈上：WiFi/httpd 在跑 */
static volatile bool s_busy = false;       /* 上传收包/驱屏互斥（按键吞 + 任务绘门） */
static volatile bool s_showed_img = false; /* 已显示过上传图（抑制后续 info 重绘） */
static volatile bool s_info_dirty = true;  /* 信息页待重绘（enter/GOT_IP 置位） */

/* ---- 常驻作业任务（run97 纪律：优先级与 feed 线程同级） ---- */
static TaskHandle_t s_portal_task = NULL;
static SemaphoreHandle_t s_job_req;
static SemaphoreHandle_t s_job_done;
#define JOB_SHOW 0                        /* 画布内容 GC16 全驱 */
static volatile int s_job;
static volatile int s_job_ms;

/* ---- WiFi/STA 状态（lan_image.c 移植，语义不变） ---- */
typedef enum { WCONN_IDLE, WCONN_CONNECTING, WCONN_OK, WCONN_FAIL } wconn_state_t;
static volatile wconn_state_t s_wconn = WCONN_IDLE;
static volatile bool s_wifi_connected = false;
static char s_sta_ip[16] = {0};
static char s_conn_ssid[33] = {0};
static char s_conn_pass[65] = {0};
static bool s_wifi_saved = false;
static bool s_wifi_inited = false;         /* netif/event/wifi_init 一次建设标志 */
static httpd_handle_t s_httpd = NULL;

/* ------------------------------------------------------------ NVS 凭据 ---- */
/* run110 移植：GOT_IP 确认成功才落盘（错误密码不写）；lanwifi 命名空间
 * 与学习状态 NVS 不冲突；烧录 app 固件原样保留 NVS 分区（不丢凭据） */

static void wifi_save_credentials(void) {
    nvs_handle_t h;
    if (nvs_open("lanwifi", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open fail (save)");
        return;
    }
    nvs_set_str(h, "ssid", s_conn_ssid);
    nvs_set_str(h, "pass", s_conn_pass);
    nvs_commit(h);
    nvs_close(h);
    s_wifi_saved = true;
    ESP_LOGW(TAG, "WiFi credentials saved (NVS)");
}

static bool wifi_load_credentials(void) {
    nvs_handle_t h;
    if (nvs_open("lanwifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = sizeof(s_conn_ssid);
    bool ok = nvs_get_str(h, "ssid", s_conn_ssid, &l) == ESP_OK && s_conn_ssid[0] != '\0';
    if (ok) {
        l = sizeof(s_conn_pass);
        if (nvs_get_str(h, "pass", s_conn_pass, &l) != ESP_OK) s_conn_pass[0] = '\0';
    }
    nvs_close(h);
    s_wifi_saved = ok;
    return ok;
}

static void wifi_forget_credentials(void) {
    nvs_handle_t h;
    if (nvs_open("lanwifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_conn_ssid[0] = '\0';
    s_conn_pass[0] = '\0';
    s_wifi_saved = false;
    ESP_LOGW(TAG, "WiFi credentials erased (NVS)");
}

/* ------------------------------------------------------------ DNS 劫持 ---- */
/* run104 移植：所有 A 查询应答 192.168.4.1（AP 网关）→ 手机连开放
 * SoftAP 后系统 connectivity check 被劫持 → 302 弹出配置/上传页 */

static volatile bool s_dns_run = false;
static TaskHandle_t s_dns_task = NULL;

static void dns_hijack_task(void *arg) {
    (void)arg;
    static char qbuf[512];
    static char rbuf[560];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns hijack: socket failed");
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
        ESP_LOGE(TAG, "dns hijack: bind 53 failed");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGW(TAG, "DNS hijack running (all A queries -> 192.168.4.1)");
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, qbuf, sizeof(qbuf), 0,
                         (struct sockaddr *)&from, &flen);
        if (n < 12) continue;               /* 超时/超短包 */
        if ((uint8_t)qbuf[5] != 1) continue; /* 仅处理 QDCOUNT=1 */

        uint8_t *b = (uint8_t *)qbuf;
        int p = 12;
        while (p < n && b[p]) p += b[p] + 1; /* 跳过 QNAME */
        p += 5;                              /* NULL + QTYPE + QCLASS */
        if (p > n) continue;
        int qlen = p;
        uint16_t qtype = (uint16_t)((b[p - 4] << 8) | b[p - 3]);

        int rl = qlen;
        memcpy(rbuf, qbuf, qlen);
        rbuf[2] = 0x85;                      /* QR=1 AA=1 RD=1 */
        rbuf[3] = 0x80;                      /* RA=1 RCODE=0 */
        rbuf[6] = 0;
        rbuf[7] = (qtype == 1) ? 1 : 0;      /* ANCOUNT */
        if (qtype == 1) {                    /* 仅 A 查询回答 */
            static const uint8_t ans[16] = {
                0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                0x00, 0x00, 0x00, 0x1E, 0x00, 0x04,
                192, 168, 4, 1                /* RDATA = AP 网关 */
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

static void dns_start(void) {
    if (s_dns_task) return;
    s_dns_run = true;
    if (xTaskCreate(dns_hijack_task, "dnshijack", 4096, NULL, 5,
                    &s_dns_task) != pdPASS) {
        s_dns_task = NULL;
        ESP_LOGE(TAG, "dns hijack task create failed");
    }
}

static void dns_stop(void) {
    s_dns_run = false;  /* 任务 recv 超时后自退（≤0.5s） */
}

/* ------------------------------------------------------------ 解包入画布 ---- */
/* 行对齐流式转换（httpd 任务上下文，只写画布不驱屏）：
 *   1bit 行（240B）：上传 bit=1=白，draw_bitmap bit=1 画 WHITE——字节
 *     语义逐位吻合，整行直传；
 *   4bpp 行（960B→240B）：FS 抖动二值化（run100 兜底移植，流式版，
 *     2026-09-17）。本屏 GC16 直传中间灰塌白（§15）且产品管线为
 *     1bpp 画布无物理灰阶 → 灰阶只能空间抖动表达。旧版 nibble>=8
 *     阈值硬切会把渐变切成硬轮廓；FS 以 7/16 3/16 5/16 1/16 误差核
 *     保留灰度感（与 lan_image dither_4bpp_to_fb 同核同阈值 128；
 *     fs2 流的中间级 5/10 同样经抖动纹理保留）。
 * 半字节序：偶 x=低半字节、奇 x=高半字节，位图 MSB=行内最左像素
 *   （与 epdiy fb 布局逐位对应，错一位整屏错位） */

/* FS 误差行缓冲：2 行 × 1920 int32 ≈ 15KB（httpd 栈 16KB 放不下 →
 * 文件级 BSS）；s_busy 互斥下单上传串行，无并发 */
static int32_t s_fs_err[2][IMG_W];
static int s_fs_cur = 0;

static void emit_row_1bit(const uint8_t *row, int y) {
    epd_gfx_draw_bitmap(0, y, IMG_W, 1, row, EPD_GFX_WHITE);
}

static void emit_row_4bpp(const uint8_t *fb_row, int y) {
    uint8_t bits[RAW_ROW];
    memset(bits, 0, sizeof(bits));
    int32_t *ec = s_fs_err[s_fs_cur];       /* 当前行误差 */
    int32_t *en = s_fs_err[s_fs_cur ^ 1];   /* 下一行误差 */
    for (int x = 0; x < IMG_W; x++) {
        const uint8_t b = fb_row[x >> 1];
        const int32_t v =
            (int32_t)(((x & 1) ? (b >> 4) : (b & 0x0F)) * 17) + ec[x];
        const int white = v >= 128;
        if (white) bits[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        const int32_t e = v - (white ? 255 : 0);
        if (x + 1 < IMG_W) ec[x + 1] += e * 7 / 16;
        if (y + 1 < IMG_H) {
            if (x > 0) en[x - 1] += e * 3 / 16;
            en[x] += e * 5 / 16;
            if (x + 1 < IMG_W) en[x + 1] += e / 16;
        }
    }
    epd_gfx_draw_bitmap(0, y, IMG_W, 1, bits, EPD_GFX_WHITE);
    /* 行 rollover：当前行已消费完，清零后换作下轮的「下一行」缓冲 */
    memset(ec, 0, sizeof(s_fs_err[0]));
    s_fs_cur ^= 1;
}

/* ------------------------------------------------------------ 信息页 ---- */
/* portal 任务上下文绘制（enter 后异步首刷 + GOT_IP 后补 IP 行）。
 * 中文走 cjk_text（48px level 3），URL/SSID 走 FreeSans bold 24pt。 */

static void draw_info_page(void) {
    epd_gfx_fill_screen(EPD_GFX_WHITE);

    /* 标题栏（黑底白字，menu_ui 风格） */
    epd_gfx_fill_rect(0, 0, IMG_W, 100, EPD_GFX_BLACK);
    cjk_text_draw(80, 30, 3, "图片上传", EPD_GFX_WHITE);

    int y = 220;
    cjk_text_draw(200, y, 3, "一 连接 WiFi", EPD_GFX_BLACK); y += 100;
    epd_gfx_set_bold(true);
    epd_gfx_draw_text(200, y + 40, LAN_SSID, EPD_GFX_BLACK, 4); y += 130;
    epd_gfx_set_bold(false);

    cjk_text_draw(200, y, 3, "二 浏览器打开", EPD_GFX_BLACK); y += 100;
    epd_gfx_set_bold(true);
    epd_gfx_draw_text(200, y + 40, "http://192.168.4.1", EPD_GFX_BLACK, 4);
    epd_gfx_set_bold(false);
    y += 130;

    if (s_wifi_connected && s_sta_ip[0]) {
        cjk_text_draw(200, y, 3, "三 同一 WiFi 也可访问", EPD_GFX_BLACK); y += 100;
        epd_gfx_set_bold(true);
        epd_gfx_draw_text(200, y + 40, s_sta_ip, EPD_GFX_BLACK, 4);
        epd_gfx_set_bold(false);
        y += 130;
    }

    cjk_text_draw(200, IMG_H - 100, 3, "中键 短按退出", EPD_GFX_BLACK);

    epd_gfx_flush();  /* GC16 全刷（信息页低频，~3.3s 可接受） */
    ESP_LOGI(TAG, "info page drawn (sta=%s)", s_sta_ip[0] ? s_sta_ip : "none");
}

/* ------------------------------------------------------------ HTTP 处理 ---- */
/* run105 教训移植：httpd_register_uri_handler 槽满仅 warning 不中止，
 * 失败必打 ERROR 留痕 */
static void httpd_reg(httpd_handle_t h, const httpd_uri_t *u) {
    if (httpd_register_uri_handler(h, u) != ESP_OK)
        ESP_LOGE(TAG, "uri register fail: %s", u->uri);
}

static esp_err_t root_handler(httpd_req_t *req) {
    /* STA 未连接时重定向 /wifi 配置页（captive portal 自动弹出路径） */
    if (s_wconn != WCONN_OK && !s_wifi_connected) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wifi");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, "");
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, HTML_PAGE);
}

/* wf 字段回真实全刷波形（2026-09-17 binfast 接入；与串口 W 命令、
 * 页面状态行同源） */
static esp_err_t status_handler(httpd_req_t *req) {
    char body[192];
    snprintf(body, sizeof(body),
             "{\"build\":\"%s\",\"busy\":%s,\"wf\":\"%s\","
             "\"raw_bytes\":%d,\"w\":%d,\"h\":%d,\"wifi\":\"%s\",\"sta_ip\":\"%s\"}",
             PORTAL_BUILD, s_busy ? "true" : "false",
             epd_gfx_binfast_enabled() ? "binfast" : "builtin",
             RAW_BYTES, IMG_W, IMG_H,
             s_wifi_connected ? "sta" : "ap", s_sta_ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t upload_handler(httpd_req_t *req) {
    if (s_busy || !s_session) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "busy");
        return ESP_OK;
    }
    /* 以 content_len 判别像素格式（run99 协议）：259200=1bit / 1036800=4bpp */
    bool is_4bpp;
    if (req->content_len == FB_BYTES) {
        is_4bpp = true;
    } else if (req->content_len == RAW_BYTES) {
        is_4bpp = false;
    } else {
        ESP_LOGE(TAG, "bad content_len=%d expect %d(1bit) or %d(4bpp)",
                 (int)req->content_len, RAW_BYTES, FB_BYTES);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expect 259200 or 1036800 bytes");
        return ESP_OK;
    }

    s_busy = true;
    ESP_LOGW(TAG, "upload start: %d B (%s)",
             (int)req->content_len, is_4bpp ? "4bpp" : "1bit");

    /* 整屏黑底先行：draw_bitmap 只画白（bit=0 保持背景）；4bpp 走 FS
     * 抖动（跨行误差缓冲归零，每次上传独立） */
    epd_gfx_fill_screen(EPD_GFX_BLACK);
    if (is_4bpp) {
        memset(s_fs_err, 0, sizeof(s_fs_err));
        s_fs_cur = 0;
    }

    /* 行对齐流式收包：收满一行解一行（TCP 分段任意，行内多轮凑满） */
    const size_t row_bytes = is_4bpp ? FB_ROW : RAW_ROW;
    uint8_t rowbuf[FB_ROW];                /* 960B（httpd 栈 16KB 充裕） */
    int row = 0;
    size_t have = 0;
    bool ok = true;
    while (row < IMG_H) {
        int n = httpd_req_recv(req, (char *)rowbuf + have, row_bytes - have);
        if (n > 0) {
            have += (size_t)n;
            if (have < row_bytes) continue;
            if (is_4bpp) emit_row_4bpp(rowbuf, row);
            else emit_row_1bit(rowbuf, row);
            row++;
            have = 0;
        } else if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        } else {
            ESP_LOGE(TAG, "recv fail n=%d row=%d", n, row);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            ok = false;
            break;
        }
    }

    /* 解包完成投递显示作业并等待（httpd 不驱屏，run97 纪律） */
    int ms = -1;
    if (ok) {
        s_job = JOB_SHOW;
        s_job_ms = -1;
        xSemaphoreGive(s_job_req);
        if (xSemaphoreTake(s_job_done, pdMS_TO_TICKS(30000)) == pdTRUE)
            ms = s_job_ms;
    }

    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":%s,\"ms\":%d,\"build\":\"%s\"}",
             ok && ms >= 0 ? "true" : "false", ms, PORTAL_BUILD);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    s_busy = false;
    return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120, .scan_time.active.max = 150,
    };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, true));
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    wifi_ap_record_t *ap_records = calloc(num, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc fail");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&num, ap_records);

    char body[2048];
    int off = snprintf(body, sizeof(body), "{\"networks\":[");
    for (int i = 0; i < num && off < (int)sizeof(body) - 128; i++) {
        if (ap_records[i].ssid[0] == '\0') continue;
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%s}",
                        i ? "," : "", (char *)ap_records[i].ssid,
                        ap_records[i].rssi,
                        ap_records[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    free(ap_records);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t wifi_connect_handler(httpd_req_t *req) {
    char body[192];
    if (req->content_len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "body too large");
    }
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) return ESP_FAIL;
    body[r] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    char *p;
    if ((p = strstr(body, "\"ssid\""))) {
        p = strchr(p + 7, '"'); if (p) p++;
        if (p) { char *e = strchr(p, '"'); if (e) { size_t l = e - p; if (l > 32) l = 32; memcpy(ssid, p, l); } }
    }
    if ((p = strstr(body, "\"pass\""))) {
        p = strchr(p + 7, '"'); if (p) p++;
        if (p) { char *e = strchr(p, '"'); if (e) { size_t l = e - p; if (l > 64) l = 64; memcpy(pass, p, l); } }
    }
    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ssid required");
    }

    ESP_LOGW(TAG, "WiFi connect: SSID=%s, PASS=%s", ssid, pass[0] ? "***" : "(open)");
    s_wconn = WCONN_CONNECTING;
    strncpy(s_conn_ssid, ssid, sizeof(s_conn_ssid) - 1);
    strncpy(s_conn_pass, pass, sizeof(s_conn_pass) - 1);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, s_conn_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_conn_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_disconnect();
    esp_wifi_connect();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t wifi_status_handler(httpd_req_t *req) {
    char body[144];
    switch (s_wconn) {
    case WCONN_CONNECTING:
        snprintf(body, sizeof(body), "{\"state\":\"connecting\"}"); break;
    case WCONN_OK:
        snprintf(body, sizeof(body), "{\"state\":\"ok\",\"ip\":\"%s\"}", s_sta_ip); break;
    case WCONN_FAIL:
        snprintf(body, sizeof(body), "{\"state\":\"fail\",\"saved\":%s}",
                 s_wifi_saved ? "true" : "false"); break;
    default:
        if (s_wifi_connected)
            snprintf(body, sizeof(body), "{\"state\":\"ok\",\"ip\":\"%s\"}", s_sta_ip);
        else if (s_wifi_saved)
            snprintf(body, sizeof(body),
                     "{\"state\":\"idle\",\"saved\":true,\"ssid\":\"%s\"}", s_conn_ssid);
        else
            snprintf(body, sizeof(body), "{\"state\":\"idle\",\"saved\":false}");
        break;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t wifi_forget_handler(httpd_req_t *req) {
    wifi_forget_credentials();
    s_wconn = WCONN_IDLE;
    esp_wifi_disconnect();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"saved\":false}");
}

/* WiFi 配置页（lan_image.c run104 原样移植：扫描/连接/状态轮询/Forget） */
static const char WIFI_CONFIG_PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>InkWord BigScreen WiFi</title>"
"<style>"
"body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:12px;background:#f5f5f5}"
"h2{margin:8px 0}#st{padding:8px;background:#ddd;margin:8px 0}"
"ul{list-style:none;padding:0}li{padding:10px;background:#fff;margin:4px 0;border:1px solid #ccc}"
"li i{float:right;color:#888;font-style:normal}li.s{border:2px solid #333}"
"input{width:100%;box-sizing:border-box;padding:10px;margin:4px 0}"
"button{padding:12px 24px;font-size:16px;width:100%}"
"</style></head><body>"
"<h2>BigScreen WiFi Setup</h2>"
"<div id='st'>Querying status...</div>"
"<div><button onclick='scan()'>Scan Networks</button></div>"
"<ul id='list'><li>Click 'Scan Networks'</li></ul>"
"<div>"
"<input id='ssid' placeholder='SSID'>"
"<input id='pass' type='password' placeholder='Password (empty for open)'>"
"<button onclick='conn()'>Connect</button>"
"<button style='margin-top:8px;background:#eee' onclick='forget()'>Forget Saved WiFi</button>"
"</div>"
"<p><a href='/'>Back to Upload</a></p>"
"<script>"
"function scan(){"
"  document.getElementById('list').innerHTML='<li>Scanning...</li>';"
"  fetch('/wifi_scan').then(function(r){return r.json()}).then(function(d){"
"    var h='';d.networks.forEach(function(a){"
"      h+='<li onclick=\\\"pick(this)\\\" data-s=\\\"'+a.ssid+'\\\">'"
"        +a.ssid+'<i>'+a.rssi+'dBm'+(a.auth?' locked':'')+'</i></li>';"
"    });"
"    document.getElementById('list').innerHTML=h||'<li>No networks</li>';"
"  }).catch(function(){document.getElementById('list').innerHTML='<li>Scan failed</li>';});"
"}"
"function pick(li){"
"  document.getElementById('ssid').value=li.dataset.s;"
"  var ls=document.getElementsByTagName('li');"
"  for(var i=0;i<ls.length;i++)ls[i].className='';"
"  li.className='s';"
"}"
"function conn(){"
"  var s=document.getElementById('ssid').value;"
"  if(!s){alert('Select or enter SSID');return;}"
"  document.getElementById('st').textContent='Connecting to '+s+'...';"
"  fetch('/wifi_connect',{method:'POST',headers:{'Content-Type':'application/json'},"
"    body:JSON.stringify({ssid:s,pass:document.getElementById('pass').value})"
"  }).then(function(r){"
"    if(r.status!=200)document.getElementById('st').textContent='Request failed';"
"  }).catch(function(e){document.getElementById('st').textContent='Error:'+e;});"
"}"
"function forget(){"
"  if(!confirm('Forget saved WiFi? Device stays in AP mode.'))return;"
"  fetch('/wifi_forget').then(function(){location.reload();})"
"  .catch(function(e){document.getElementById('st').textContent='Error:'+e;});"
"}"
"function tick(){"
"  fetch('/wifi_status').then(function(r){return r.json()}).then(function(s){"
"    var t=document.getElementById('st');"
"    if(s.state=='ok')t.innerHTML='Connected! IP: <b>'+s.ip+'</b> | <a href=\\\"/\\\">Upload Page</a>';"
"    else if(s.state=='connecting')t.textContent='Connecting...';"
"    else if(s.state=='fail')t.textContent='Failed. Check password and retry.';"
"    else t.textContent=s.saved?('Saved: '+s.ssid+' (AP: 192.168.4.1)'):'Not connected (AP mode: 192.168.4.1)';"
"  }).catch(function(){});"
"}"
"setInterval(tick,1500);tick();scan();"
"</script></body></html>";

static esp_err_t wifi_page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, WIFI_CONFIG_PAGE);
}

/* 未注册路径 302（captive portal 兜底）；防自循环（run105） */
static esp_err_t portal_redirect_handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    const char *loc = (s_wconn != WCONN_OK && !s_wifi_connected) ? "/wifi" : "/";
    if (strcmp(req->uri, loc) == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "endpoint not registered");
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "");
}

/* ------------------------------------------------------------ WiFi 事件 ---- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_sta_ip[0] = '\0';
        if (s_wconn == WCONN_CONNECTING) {
            s_wconn = WCONN_FAIL;
            ESP_LOGW(TAG, "WiFi connect failed");
        } else if (s_wconn == WCONN_OK) {
            ESP_LOGW(TAG, "WiFi disconnected, retrying...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_wconn = WCONN_OK;
        wifi_save_credentials();       /* 连接确认成功才落盘（run110） */
        dns_stop();                    /* STA 已连，不再需要 captive portal */
        if (!s_showed_img) s_info_dirty = true;  /* 信息页补 STA IP 行 */
        ESP_LOGW(TAG, "WiFi connected! IP: %s", s_sta_ip);
    }
}

/* ------------------------------------------------------------ 会话生命周期 ---- */
static void lan_wifi_session_start(void) {
    if (!s_wifi_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
        s_wifi_inited = true;
    }

    /* APSTA：SoftAP（开放，captive portal 入口）+ STA（NVS 凭据自动连） */
    wifi_config_t ap_wc = {0};
    snprintf((char *)ap_wc.ap.ssid, sizeof(ap_wc.ap.ssid), "%s", LAN_SSID);
    ap_wc.ap.ssid_len = strlen(LAN_SSID);
    ap_wc.ap.channel = 6;
    ap_wc.ap.max_connection = 4;
    ap_wc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  /* AP 随时响应 ARP/TCP */

    if (wifi_load_credentials()) {
        wifi_config_t wc = {0};
        strncpy((char *)wc.sta.ssid, s_conn_ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, s_conn_pass, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode =
            s_conn_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        s_wconn = WCONN_CONNECTING;
        esp_wifi_connect();
        ESP_LOGW(TAG, "auto-connect to saved SSID: %s", s_conn_ssid);
    }

    dns_start();
    ESP_LOGW(TAG, "SoftAP up: %s (OPEN) -> http://192.168.4.1", LAN_SSID);
}

static void lan_httpd_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 16;          /* 本服务 8 端点，留余量（run105） */
    cfg.recv_wait_timeout = 60;         /* 4bpp 上传 1MB 秒级传输（run99） */
    cfg.send_wait_timeout = 30;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start fail");
        s_httpd = NULL;
        return;
    }

    static const httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
    static const httpd_uri_t uri_upload = {.uri = "/upload", .method = HTTP_POST, .handler = upload_handler};
    static const httpd_uri_t uri_status = {.uri = "/status", .method = HTTP_GET, .handler = status_handler};
    static const httpd_uri_t uri_wifi_page = {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_page_handler};
    static const httpd_uri_t uri_wifi_scan = {.uri = "/wifi_scan", .method = HTTP_GET, .handler = wifi_scan_handler};
    static const httpd_uri_t uri_wifi_conn = {.uri = "/wifi_connect", .method = HTTP_POST, .handler = wifi_connect_handler};
    static const httpd_uri_t uri_wifi_st = {.uri = "/wifi_status", .method = HTTP_GET, .handler = wifi_status_handler};
    static const httpd_uri_t uri_wifi_forget = {.uri = "/wifi_forget", .method = HTTP_GET, .handler = wifi_forget_handler};
    httpd_reg(s_httpd, &uri_root);
    httpd_reg(s_httpd, &uri_upload);
    httpd_reg(s_httpd, &uri_status);
    httpd_reg(s_httpd, &uri_wifi_page);
    httpd_reg(s_httpd, &uri_wifi_scan);
    httpd_reg(s_httpd, &uri_wifi_conn);
    httpd_reg(s_httpd, &uri_wifi_st);
    httpd_reg(s_httpd, &uri_wifi_forget);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, portal_redirect_handler);
}

/* ------------------------------------------------------------ portal 任务 ---- */
/* 常驻（首会话创建后不删）：显示作业执行体（run97：优先级与 feed 同级）
 * + 空闲期信息页异步重绘。一切驱屏收敛于本任务（app_main 侧 enter 只
 * 置 dirty，U 命令/词卡渲染在 LAN 栈顶期间不可达——单任务驱屏纪律） */
static void portal_task(void *arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_job_req, pdMS_TO_TICKS(500)) == pdTRUE) {
            int64_t t0 = esp_timer_get_time();
            if (s_job == JOB_SHOW) {
                /* back 置黑构造全屏 diff → GC16：白区全驱驱白（run98
                 * 基线同构）；顺带重置局刷灰染计数（epd_gfx 内部） */
                epd_gfx_force_refresh();
                s_showed_img = true;
                s_info_dirty = false;
            }
            s_job_ms = (int)((esp_timer_get_time() - t0) / 1000);
            ESP_LOGW(TAG, "job %d done: %d ms", s_job, s_job_ms);
            xSemaphoreGive(s_job_done);
        } else if (s_session && !s_busy && s_info_dirty) {
            draw_info_page();
            s_info_dirty = false;
        }
    }
}

static void portal_task_ensure(void) {
    if (s_portal_task) return;
    s_job_req = xSemaphoreCreateBinary();
    s_job_done = xSemaphoreCreateBinary();
    if (s_job_req == NULL || s_job_done == NULL) {
        ESP_LOGE(TAG, "job semaphore create fail");
        return;
    }
    if (xTaskCreate(portal_task, "lan_portal", 8192, NULL,
                    configMAX_PRIORITIES - 1, &s_portal_task) != pdPASS) {
        ESP_LOGE(TAG, "portal task create fail");
        s_portal_task = NULL;
    }
}

/* ------------------------------------------------------------ 页面协议 ---- */

bool lan_portal_active(void) {
    return s_session;
}

static void lan_portal_enter(void) {
    if (s_session) return;
    s_session = true;
    s_showed_img = false;
    s_info_dirty = true;

    /* ADC2/WiFi 互斥：先停按键采样再起 WiFi（GPIO19=ADC2_CH8） */
    button_scan_pause();
    portal_task_ensure();
    lan_wifi_session_start();
    lan_httpd_start();
    ESP_LOGW(TAG, "LAN portal session up [%s]", PORTAL_BUILD);
}

static void lan_portal_exit(void) {
    if (!s_session) return;

    /* busy 期间按键已吞（on_button），此处理论 !s_busy；防御性短等
     * （最长上传+驱屏 ~10s 量级，500ms x 20） */
    int wait = 0;
    while (s_busy && wait++ < 20) vTaskDelay(pdMS_TO_TICKS(500));

    if (s_httpd) {
        httpd_stop(s_httpd);            /* 内部关闭 socket，任务自删 */
        s_httpd = NULL;
    }
    dns_stop();

    /* 先清状态再停 WiFi：DISCONNECTED 事件 handler 的 WCONN_OK 分支
     * 会 esp_wifi_connect() 重连，stop 后该调用必失败刷错误日志 */
    s_wconn = WCONN_IDLE;
    s_wifi_connected = false;
    s_sta_ip[0] = '\0';
    esp_wifi_stop();                    /* 释放射频 → ADC2 仲裁恢复 */

    button_scan_resume();               /* 含通道重配（ADC2 恢复保险） */
    s_session = false;
    ESP_LOGW(TAG, "LAN portal session down [%s]", PORTAL_BUILD);
}

static void lan_portal_render(void) {
    /* 渲染请求只置脏标记：绘制收敛于 portal 任务（优先级纪律），render
     * 回调本身不驱屏（LAN 页在栈顶时 render_top 实际不可达，防御实现） */
    if (!s_showed_img) s_info_dirty = true;
}

static bool lan_portal_on_button(nav_key_t id, button_event_t event) {
    if (s_busy) return true;            /* 上传/驱屏期间吞键防退出竞态 */
    if (id == NAV_CENTER && event == BUTTON_EVENT_SHORT_PRESS)
        return false;                   /* 请求退出（编排层 pop+render_top） */
    return true;
}

const page_t g_lan_portal_page = {
    .name = "lan_portal",
    .render = lan_portal_render,
    .on_button = lan_portal_on_button,
    .enter = lan_portal_enter,
    .exit = lan_portal_exit,
    .owns_display = true,
};
