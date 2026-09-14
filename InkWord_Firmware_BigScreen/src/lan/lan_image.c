// LAN 图片上传显示（适配里程碑 M1）。
//
// 链路：手机/PC 连 SoftAP → 打开 http://192.168.4.1 → 选图 → 浏览器 canvas
//   等比 contain（白底）+ 灰度 + Floyd–Steinberg 抖动 → 1920x1080 1bit 打包
//   （MSB first，bit1=白，259200B）POST /upload → 设备解包入 epdiy fb →
//   全屏 MODE_GC16 更新。
//
// 设计决策：
//   1. 解码/缩放/抖动全在浏览器侧：设备零图像解码依赖（不引 esp_jpeg/pngle），
//      设备只做解包+更新，适配期最简最稳；
//   2. 管线复用 run87 定稿配置：EPD_ROT_LANDSCAPE 原生 1920x1080，不走旋转
//      路径（旋转路径在本 patched 管线未验证，不引入新变量）；屏横放观看；
//   3. 更新在 httpd 任务内同步阻塞（GC16 ~3.3s），期间新请求由 s_busy 拒
//      （503），单演示连接足够；feed 线程优先级高于 httpd 任务，吞吐不受影响；
//   4. 电源路径与 demo 一致：raw GPIO46 使能，禁止 epd_poweron（本板无 I2C
//      控制链，run89 教训）；
//   5. AP 模式免配网：SSID InkWord-BigScreen / WPA2 inkword123，IP 固定
//      192.168.4.1（esp_netif AP 默认）。
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <epdiy.h>

#include <lwip/ip_addr.h>

#include "driver/gpio.h"

#include "board_config.h"
#include "lan_image.h"
#include "lan_page.h"

static const char* TAG = "lan_image";

#define IMG_W 1920
#define IMG_H 1080
#define RAW_BYTES (IMG_W * IMG_H / 8)   // 259200：上传裸流 1bit 打包（8px/字节）
#define FB_BYTES (IMG_W / 2 * IMG_H)    // 1036800：epdiy fb 为 4bpp 半字节打包
#define LAN_SSID "InkWord-BigScreen"
#define LAN_PASS "inkword123"
// [run99] 强制门户（Captive Portal）入口地址：esp_netif AP 默认网关，
//   同时充当 DHCP 下发的 DNS 与 DNS 劫持的应答地址。
#define PORTAL_IP "192.168.4.1"

// [run96] 构建标识：run95 的 front/back 交换改动未被编译（源码 mtime 晚于
//   firmware.bin 13 分钟，烧录失败后板子复位重跑 run94），而 run94/run95 的
//   串口签名完全相同（均为全屏 diff + 30 相位），无法区分 → 误判为"波形反向"。
//   教训：每次烧录必须在日志里看到本标识，且确认 firmware.bin 比源码新。
//   波形侧已离线解码证实无问题（tools/decode_waveform.py）：ED047TC1 GC16
//   的 to=15,from=0 = 前 15 相位 nop + 后 15 相位驱白，方向正确。
// [run98] 复核修正：run94.log 与 run95.log md5 不同（thread 编号有别）→ 确为
//   两次独立运行，但文案与签名逐字一致，且 run95.log 落盘时刻 21:35 等于
//   firmware.bin 构建时刻、源码编辑在 21:47:54 → 两次跑的是同一份 run94 固件。
//   结论不变：to=15,from=0（白驱）至今无任何真机验证。故本轮把白/黑/图案/
//   极性全部做成远程端点，判读不再依赖重烧。
#define LAN_BUILD_TAG "run99"

// 屏定义实体在 demo_seller.c（卖家 main.c L54-61 定义，全环境链接）
extern const EpdDisplay_t ES108FC;

static EpdiyHighlevelState hl;
static uint8_t* s_raw = NULL;          // PSRAM 接收缓冲（按 4bpp 上限 1036800B 分配）
// [run99] 实际接收长度，决定像素格式：FB_BYTES=4bpp 灰阶 / RAW_BYTES=1bit
static volatile size_t s_raw_len = 0;
static volatile bool s_busy = false;   // 更新互斥（httpd 单任务串行，防并发 update）
// [run98] 黑白极性运行时反转（/pol 端点切换）：白驱究竟使屏变白还是变黑，
//   真机从未验证过（见 LAN_BUILD_TAG 注释）。若 /white 实测变黑，点 /pol 后
//   再点 /white 即可确认是极性理解偏差，无需重烧固件。
static volatile int s_inv = 0;

// ------------------------------------------------------------ 显示作业 ----
// [run97] 关键约束（main.c L52-53 真机实证）：调用 epd_draw_base 的任务必须与
//   feed 线程（epd_prep）同优先级 configMAX_PRIORITIES-1，否则帧完成后
//   frame_done 就绪也无任务可调度 → 静默挂死。httpd 任务默认优先级仅
//   tskIDLE_PRIORITY+5，故 /upload、/clear 一律不在 httpd 任务内直接驱屏，
//   而是投递作业给 lan_image 任务（configMAX_PRIORITIES-1）执行，httpd 只
//   做网络 I/O + 等待。epd_clear 同受影响（内部也阻塞于 frame_done）。
typedef enum {
    JOB_WHITE = 0,     // 全屏白（GC16 diff）
    JOB_BLACK = 1,     // 全屏黑（GC16 diff）
    JOB_PATTERN = 2,   // 四象限测试图案
    JOB_UPDATE = 3,    // 上传图像
    JOB_CLEAR = 4,     // epd_clear() 对照（不可靠，仅留作对比）
} display_job_t;

static SemaphoreHandle_t s_job_req;    // httpd → lan_image
static SemaphoreHandle_t s_job_done;   // lan_image → httpd
static volatile display_job_t s_job;
static volatile int s_job_err;
static volatile int s_job_ms;

// [run99] 按上传长度分派两种像素格式：
//   FB_BYTES(1036800) = 4bpp 16 级灰阶，半字节布局与 fb 完全一致 → 直接
//     memcpy（页面打包顺序：p>>1 定字节、p&1 定高/低半字节、15=白/0=黑，
//     与 epdiy fb 约定逐位吻合；错一位就整屏错位，故不可凭直觉改）；
//   RAW_BYTES(259200) = 1bit 二值（MSB first，bit1=白）→ 全黑底后逐像素置白。
//   尺寸必须 FB_BYTES（4bpp）：run93 误用 8bpp → 1MB 堆溢出。
//   get_framebuffer 返回 front_fb，即 diff 的 to（目标图）。
static void unpack_raw_to_fb(void) {
    uint8_t* fb = epd_hl_get_framebuffer(&hl);
    if (s_raw_len == FB_BYTES) {
        memcpy(fb, s_raw, FB_BYTES);
    } else {
        memset(fb, 0x00, FB_BYTES);
        for (size_t p = 0; p < (size_t)IMG_W * IMG_H; p++) {
            if (s_raw[p >> 3] & (0x80 >> (p & 7))) {
                if (p & 1) {
                    fb[p >> 1] |= 0xF0;
                } else {
                    fb[p >> 1] |= 0x0F;
                }
            }
        }
    }
    if (s_inv) {
        // 4bpp 半字节按位取反即 v -> 15-v，正好黑白互换（灰阶同步反转）
        for (size_t i = 0; i < (size_t)FB_BYTES; i++) {
            fb[i] = (uint8_t)~fb[i];
        }
    }
}

// [run98] 纯色填充：front=目标色 / back=相反色。back 必须取相反色——
//   highlevel.c L128 对 diff_area 宽或高为 0 直接 return SUCCESS，双缓冲同色
//   即 no-op（run92 实证：屏留残影、无任何扫描）。
//   s_inv=1 时交换两者，等价于把目标色反相。
static void fill_solid(int white) {
    uint8_t tgt = white ? 0xFF : 0x00;   // 每半字节 15=白 / 0=黑
    uint8_t src = white ? 0x00 : 0xFF;
    if (s_inv) {
        uint8_t t = tgt;
        tgt = src;
        src = t;
    }
    memset(hl.front_fb, tgt, FB_BYTES);
    memset(hl.back_fb, src, FB_BYTES);
}

// 写单像素到 4bpp fb（偶 x=低半字节 / 奇 x=高半字节，白=15、黑=0）
static inline void fb_px(uint8_t* fb, int x, int y, int white) {
    size_t p = (size_t)y * IMG_W + (size_t)x;
    uint8_t v = white ? 0x0F : 0x00;
    if (p & 1) {
        fb[p >> 1] = (uint8_t)((fb[p >> 1] & 0x0F) | (v << 4));
    } else {
        fb[p >> 1] = (uint8_t)((fb[p >> 1] & 0xF0) | v);
    }
}

// [run98] 四象限测试图案：左上全黑 / 右上 16px 棋盘 / 左下 8px 竖条 /
//   右下 24px 斜条，外加 16px 黑框与中心十字。比照片更易判读，可一次性
//   验证 x/y 方向、半字节奇偶映射与灰阶响应是否正确。
static void draw_pattern_to_fb(void) {
    uint8_t* fb = epd_hl_get_framebuffer(&hl);
    memset(fb, 0x00, FB_BYTES);          // 全黑底，仅按需置白
    const int hw = IMG_W / 2, hh = IMG_H / 2;
    for (int y = 0; y < IMG_H; y++) {
        for (int x = 0; x < IMG_W; x++) {
            int w = 1;
            if (x < 16 || x >= IMG_W - 16 || y < 16 || y >= IMG_H - 16) {
                w = 0;                                     // 黑框
            } else {
                int dx = x - hw;
                int dy = y - hh;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx < 4 || dy < 4) {
                    w = 0;                                 // 中心十字
                } else {
                    int q = (x < hw) * 2 + (y < hh);
                    if (q == 0) {
                        w = 0;                             // 全黑
                    } else if (q == 1) {
                        w = (x / 16 + y / 16) % 2;         // 16px 棋盘
                    } else if (q == 2) {
                        w = (x / 8) % 2;                   // 8px 竖条
                    } else {
                        w = ((x + y) / 24) % 2;            // 24px 斜条
                    }
                }
            }
            if (s_inv) w = !w;
            if (w) fb_px(fb, x, y, 1);
        }
    }
    // from 取与目标相反的纯色底，保证每行都脏（否则纯黑象限所在行可能与
    //   back 同色而被 highlevel 判为干净行、跳过同步）
    memset(hl.back_fb, s_inv ? 0x00 : 0xFF, FB_BYTES);
}

// 投递显示作业并等待完成（httpd 任务侧调用）。超时 60s：clear≈6.6s、
//   GC16 全屏≈3.3s，留足余量。
static bool run_display_job(display_job_t job, int* err, int* ms) {
    s_job = job;
    s_job_err = -1;
    s_job_ms = -1;
    xSemaphoreGive(s_job_req);
    if (xSemaphoreTake(s_job_done, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "display job %d timeout", (int)job);
        return false;
    }
    *err = s_job_err;
    *ms = s_job_ms;
    return true;
}

// ------------------------------------------------------------ 上传页面 ----
// [run99] 页面已拆到 lan_page.h：加入旋转/镜像/适配/灰阶/多输出模式后
//   字符串突破 250 行，与驱动逻辑混写不可维护。本项目无文件系统与资源
//   打包机制，沿用"字符串常量 + include"。

// ------------------------------------------------------------ HTTP 处理 ----
static esp_err_t root_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, HTML_PAGE);
}

static esp_err_t status_handler(httpd_req_t* req) {
    char body[160];
    snprintf(body, sizeof(body),
             "{\"build\":\"%s\",\"busy\":%s,\"inv\":%d,\"raw_bytes\":%d,\"w\":%d,\"h\":%d}",
             LAN_BUILD_TAG, s_busy ? "true" : "false", (int)s_inv, RAW_BYTES, IMG_W, IMG_H);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// [run98] 诊断端点统一入口：white/black/pattern/clear 均为"投递作业 + 等
//   结果"，仅 job 类型不同。httpd 任务只做网络 I/O（见顶部优先级约束）。
//   响应体带 build 标识与 inv，页面一眼可确认在机版本与当前极性。
static esp_err_t job_handler(httpd_req_t* req, display_job_t job) {
    if (s_busy) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "busy");
        return ESP_OK;
    }
    s_busy = true;
    int err = -1, ms = -1;
    bool ok = run_display_job(job, &err, &ms);
    char body[128];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"job\":%d,\"err\":%d,\"ms\":%d,\"inv\":%d,\"build\":\"%s\"}",
             (ok && err == EPD_DRAW_SUCCESS) ? "true" : "false",
             (int)job, err, ms, (int)s_inv, LAN_BUILD_TAG);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    s_busy = false;
    return ESP_OK;
}

static esp_err_t white_handler(httpd_req_t* req)   { return job_handler(req, JOB_WHITE); }
static esp_err_t black_handler(httpd_req_t* req)   { return job_handler(req, JOB_BLACK); }
static esp_err_t pattern_handler(httpd_req_t* req) { return job_handler(req, JOB_PATTERN); }
static esp_err_t clear_handler(httpd_req_t* req)   { return job_handler(req, JOB_CLEAR); }

// /pol：运行时反转黑白极性（GET/POST 均可），不驱屏、不阻塞
static esp_err_t pol_handler(httpd_req_t* req) {
    s_inv = !s_inv;
    char body[64];
    snprintf(body, sizeof(body), "{\"inv\":%d,\"build\":\"%s\"}", (int)s_inv, LAN_BUILD_TAG);
    ESP_LOGW(TAG, "polarity inverted -> inv=%d", (int)s_inv);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// [run99] 未注册路径一律 302 → /：既让各平台联网检测拿不到预期响应体
//   （captive portal 弹窗触发条件），也容忍用户手输任意 URL。
static esp_err_t portal_redirect_handler(httpd_req_t* req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "");
}

static esp_err_t upload_handler(httpd_req_t* req) {
    if (s_busy) {
        // IDF http_server 枚举无 503 常量，自定义状态行
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "update in progress");
        return ESP_OK;
    }
    // [run99] 以 content_len 判别像素格式，免加协议头：
    //   1036800 = 4bpp 16 级灰阶，259200 = 1bit 二值（抖动/阈值）
    size_t want;
    if (req->content_len == FB_BYTES) {
        want = FB_BYTES;
    } else if (req->content_len == RAW_BYTES) {
        want = RAW_BYTES;
    } else {
        ESP_LOGE(TAG, "bad content_len=%d expect=%d(4bpp) or %d(1bit)",
                 (int)req->content_len, FB_BYTES, RAW_BYTES);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expect 1036800 or 259200 bytes");
        return ESP_OK;
    }
    s_busy = true;
    uint8_t* p = s_raw;
    size_t left = want;
    while (left > 0) {
        int n = httpd_req_recv(req, (char*)p, left);
        if (n > 0) {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        ESP_LOGE(TAG, "recv fail n=%d left=%u", n, (unsigned)left);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
        s_busy = false;
        return ESP_OK;
    }
    s_raw_len = want;
    ESP_LOGW(TAG, "recv ok: %u B (%s)", (unsigned)want,
             want == FB_BYTES ? "4bpp gray16" : "1bit");

    // 解包 + 更新一律交给高优先级 lan_image 任务（见顶部显示作业注释）
    int err = -1, ms = -1;
    bool ok = run_display_job(JOB_UPDATE, &err, &ms);

    char body[128];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"err\":%d,\"ms\":%d,\"bytes\":%u,\"inv\":%d,\"build\":\"%s\"}",
             (ok && err == EPD_DRAW_SUCCESS) ? "true" : "false", err, ms,
             (unsigned)want, (int)s_inv, LAN_BUILD_TAG);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    s_busy = false;
    return ESP_OK;
}

// ------------------------------------------------------------ WiFi AP ----
// [run99] Captive Portal：连上 AP 后系统自动弹出上传页面。
//   各平台的联网检测（iOS captive.apple.com/hotspot-detect.html、Android
//   connectivitycheck.gstatic.com/generate_204、Windows msftconnecttest.com/
//   connecttest.txt）都要先做 DNS 解析，因此三步缺一不可：
//     1) DHCP 下发 DNS = 192.168.4.1（esp_netif_set_dns_info 对带
//        ESP_NETIF_DHCP_SERVER 标志的 netif 会转调 dhcps_dns_setserver_by_type）；
//     2) 本机 UDP/53 对任意域名一律回 A 记录 192.168.4.1（DNS 劫持）；
//     3) HTTP 对未注册路径 302 → /，使检测拿不到预期响应体 → 系统判定
//        "需要登录"并弹出内置浏览器。
//   HTTPS 探测（部分 Android 版本）会因证书不符失败，同样触发弹窗。

// 极简 DNS 应答：不解析域名，直接回固定 A 记录。报文 = 原查询头（改
//   flags/ancount）+ 原 question 段回显 + 1 条 answer；answer 用 0xC00C 名字
//   压缩指针指回偏移 12 的 QNAME，免去域名拷贝与长度校验。
static void dns_hijack_task(void* arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "dns socket fail: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "dns bind :53 fail: errno=%d", errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "DNS hijack up on :53 -> %s", PORTAL_IP);

    static uint8_t q[512], r[512];
    while (1) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int n = recvfrom(fd, q, sizeof(q), 0, (struct sockaddr*)&ca, &cl);
        if (n < 12) continue;                 // 连 DNS 头都不够，丢弃
        // 定位 question 段结束：QNAME 由若干 label 组成、以 0 结尾，后跟
        //   QTYPE(2) + QCLASS(2)。label 长度 >63 或为压缩指针均视为非法。
        int qend = 12;
        bool bad = false;
        while (qend < n && q[qend] != 0) {
            int lbl = q[qend];
            if (lbl > 63 || qend + 1 + lbl >= n) { bad = true; break; }
            qend += 1 + lbl;
        }
        qend += 5;                            // 终止 0 + QTYPE + QCLASS
        if (bad || qend > n || qend + 16 > (int)sizeof(r)) continue;

        memcpy(r, q, (size_t)qend);           // 头 + question 原样回显
        r[2] = 0x81;                          // QR=1 RD=1
        r[3] = 0x80;                          // RA=1 RCODE=0
        r[6] = 0; r[7] = 1;                   // ANCOUNT = 1
        r[8] = 0; r[9] = 0; r[10] = 0; r[11] = 0;   // NS/ARCOUNT = 0

        uint8_t* a = r + qend;
        a[0] = 0xC0; a[1] = 0x0C;             // 名字压缩指针 -> 偏移 12
        a[2] = 0;    a[3] = 1;                // TYPE  = A
        a[4] = 0;    a[5] = 1;                // CLASS = IN
        a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 30;   // TTL 30s（短，便于重连刷新）
        a[10] = 0;   a[11] = 4;               // RDLENGTH
        a[12] = 192; a[13] = 168; a[14] = 4; a[15] = 1;
        sendto(fd, r, (size_t)qend + 16, 0, (struct sockaddr*)&ca, cl);
    }
}

static esp_netif_t* s_ap_netif = NULL;

static void lan_wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // [run99] 必须留住 netif 句柄：下发 DHCP DNS 选项要用（run98 前丢弃）
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wc = {0};
    snprintf((char*)wc.ap.ssid, sizeof(wc.ap.ssid), "%s", LAN_SSID);
    snprintf((char*)wc.ap.password, sizeof(wc.ap.password), "%s", LAN_PASS);
    wc.ap.ssid_len = (uint8_t)strlen(LAN_SSID);
    wc.ap.channel = 6;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP up: %s / %s", LAN_SSID, LAN_PASS);

    // DHCP 下发本机为 DNS。必须在 esp_wifi_start()（DHCPS 已起）之后，
    //   否则 netif 的 DHCP_SERVER 分支尚未生效。用 lwIP 的 IP_ADDR4 而非
    //   esp_netif_set_ip4_addr：后者只收 esp_ip4_addr_t*，而本工程
    //   CONFIG_LWIP_IPV6=y 为双栈，dns.ip 是 ip_addr_t；IP_ADDR4 在双栈下会
    //   一并设好 IPADDR_TYPE_V4，单栈下退化为纯 IPv4 赋值，两边都对。
    if (s_ap_netif != NULL) {
        esp_netif_dns_info_t dns;
        memset(&dns, 0, sizeof(dns));
        IP_ADDR4(&dns.ip, 192, 168, 4, 1);
        esp_err_t derr = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGW(TAG, "dhcps DNS -> %s : %s", PORTAL_IP, esp_err_to_name(derr));
    } else {
        ESP_LOGE(TAG, "ap netif NULL，无法下发 DNS（门户弹窗将不可靠）");
    }

    // DNS 劫持服务：与驱屏无关，给低优先级（httpd 同级 5），绝不得抢占
    //   feed 线程（configMAX_PRIORITIES-1）——见顶部优先级约束注释。
    if (xTaskCreate(dns_hijack_task, "dns_hijack", 1 << 12, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "dns_hijack 任务创建失败");
    }
}

// ------------------------------------------------------------ 任务 ----
void lan_image_task(void* arg) {
    (void)arg;
    ESP_LOGW(TAG, "=== BUILD %s ===", LAN_BUILD_TAG);

    // 电源脚输出模式 + 上电（与 demo 同路径，raw GPIO 直控）
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BS_EPD_POWER_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(BS_EPD_POWER_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    epd_init(&epd_board_v7, &ES108FC, EPD_LUT_64K);
    hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    epd_set_rotation(EPD_ROT_LANDSCAPE);

    // [run99] 缓冲按 4bpp 上限分配（1036800B），1bit 上传只用前 1/4。
    //   PSRAM 余量：fb 1MB + difference_fb 2MB + 本缓冲 1MB，8MB PSRAM 充裕。
    s_raw = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (s_raw == NULL) {
        ESP_LOGE(TAG, "raw buf alloc fail (%d B)", FB_BYTES);
        return;
    }

    // [run98] 白基线改走 GC16 diff（front=白 / back=黑 → to=15,from=0）：
    //   这是 run88-run91 局部刷新与卖家 demo 都验证过的 epd_draw_base 路径，
    //   每帧有 frame k= 日志（可观测），健康签名 3269ms/30 帧。
    //   放弃 run96/97 的 epd_clear()：其内部 epd_push_pixels_lcd 缺
    //   lcd_do_update 里 run72 的每帧残留排空（lq_reset），而 LCD 每帧实际
    //   消费少于生产（1080 产 / ~1000 消）→ 66 帧是否真扫完不可控：
    //   run96 同一行代码 6623ms（~100ms/帧，合理）vs run97 仅 643ms
    //   （~10ms/帧，frame_done 立即返回），10 倍漂移。run97 疑仅推完
    //   前段驱黑帧即返回 → 屏停在纯黑（用户实测）。且该路径无 frame k=
    //   日志，不可观测 → 不适合做基线。
    // 历史：run92 双缓冲同白 → diff 空 no-op；run93 按 8bpp memset → 1MB
    //   堆溢出；run94 front=黑/back=白 → to=0,from=15 驱黑（纯黑，符合波形）；
    //   run95 的 front/back 交换从未上板（两次日志为同一份固件）。
    int64_t tc = esp_timer_get_time();
    fill_solid(1);
    int berr = (int)epd_hl_update_screen(&hl, MODE_GC16, epd_ambient_temperature());
    ESP_LOGW(TAG, "baseline white via GC16: err=%d ms=%d inv=%d",
             berr, (int)((esp_timer_get_time() - tc) / 1000), (int)s_inv);
    // 更新后 highlevel 已按脏行把 back 同步为 front；再显式置白，确保首次
    //   上传的 diff 为 to=图像 / from=白（demo 已验证的方向）。
    memset(hl.back_fb, 0xFF, FB_BYTES);

    // 显示作业通道（必须在 httpd 启动前就绪）
    s_job_req = xSemaphoreCreateBinary();
    s_job_done = xSemaphoreCreateBinary();
    if (s_job_req == NULL || s_job_done == NULL) {
        ESP_LOGE(TAG, "job semaphore create fail");
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    lan_wifi_start();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    cfg.max_open_sockets = 4;
    // [run99] 4bpp 上传达 1MB（SoftAP 实测数秒），默认 5s 收发超时会中途
    //   断流 → recv fail；lru_purge 避免陈旧连接占满 4 个 socket。
    cfg.recv_wait_timeout = 60;
    cfg.send_wait_timeout = 30;
    cfg.lru_purge_enable = true;
    httpd_handle_t httpd = NULL;
    if (httpd_start(&httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start fail");
        return;
    }
    static const httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
    static const httpd_uri_t uri_upload = {.uri = "/upload", .method = HTTP_POST, .handler = upload_handler};
    static const httpd_uri_t uri_status = {.uri = "/status", .method = HTTP_GET, .handler = status_handler};
    static const httpd_uri_t uri_clear = {.uri = "/clear", .method = HTTP_POST, .handler = clear_handler};
    static const httpd_uri_t uri_white = {.uri = "/white", .method = HTTP_POST, .handler = white_handler};
    static const httpd_uri_t uri_black = {.uri = "/black", .method = HTTP_POST, .handler = black_handler};
    static const httpd_uri_t uri_pattern = {.uri = "/pattern", .method = HTTP_POST, .handler = pattern_handler};
    static const httpd_uri_t uri_pol = {.uri = "/pol", .method = HTTP_GET, .handler = pol_handler};
    httpd_register_uri_handler(httpd, &uri_root);
    httpd_register_uri_handler(httpd, &uri_upload);
    httpd_register_uri_handler(httpd, &uri_status);
    httpd_register_uri_handler(httpd, &uri_clear);
    httpd_register_uri_handler(httpd, &uri_white);
    httpd_register_uri_handler(httpd, &uri_black);
    httpd_register_uri_handler(httpd, &uri_pattern);
    httpd_register_uri_handler(httpd, &uri_pol);
    // [run99] captive portal：未注册路径（含各平台联网检测路径）一律 302 → /
    httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, portal_redirect_handler);

    ESP_LOGW(TAG, "LAN ready [%s]: join SSID %s (pass %s) -> http://192.168.4.1",
             LAN_BUILD_TAG, LAN_SSID, LAN_PASS);
    // [run96] 心跳带构建标识：本 WCH 适配器的 RTS 复位脉冲无效，而启动
    //   日志只在复位后头几秒输出 → 抓不到就无法确证在机版本（run95 即因
    //   此误判）。每 10s 重打标识，任意时刻挂上读取器即可确认。
    // [run97] 本循环同时是显示作业执行体：必须留在本任务（优先级
    //   configMAX_PRIORITIES-1，与 epd_prep feed 线程同级）。
    unsigned hb = 0;
    while (1) {
        if (xSemaphoreTake(s_job_req, pdMS_TO_TICKS(10000)) == pdTRUE) {
            int64_t t0 = esp_timer_get_time();
            switch (s_job) {
                case JOB_WHITE:
                    fill_solid(1);
                    s_job_err = (int)epd_hl_update_screen(&hl, MODE_GC16,
                                                          epd_ambient_temperature());
                    break;
                case JOB_BLACK:
                    fill_solid(0);
                    s_job_err = (int)epd_hl_update_screen(&hl, MODE_GC16,
                                                          epd_ambient_temperature());
                    break;
                case JOB_PATTERN:
                    draw_pattern_to_fb();
                    s_job_err = (int)epd_hl_update_screen(&hl, MODE_GC16,
                                                          epd_ambient_temperature());
                    break;
                case JOB_UPDATE:
                    unpack_raw_to_fb();
                    s_job_err = (int)epd_hl_update_screen(&hl, MODE_GC16,
                                                          epd_ambient_temperature());
                    break;
                case JOB_CLEAR:
                    // [run98] 仅留作对照：验证 epd_clear() 时长是否仍漂移
                    //   （正常 66 帧应 ≈ 6.6s；若再现 ~0.6s 即帧未等完）。
                    epd_clear();
                    memset(hl.front_fb, 0xFF, FB_BYTES);
                    memset(hl.back_fb, 0xFF, FB_BYTES);
                    s_job_err = EPD_DRAW_SUCCESS;
                    break;
            }
            s_job_ms = (int)((esp_timer_get_time() - t0) / 1000);
            ESP_LOGW(TAG, "job %d done: err=%d ms=%d inv=%d",
                     (int)s_job, s_job_err, s_job_ms, (int)s_inv);
            xSemaphoreGive(s_job_done);
        } else {
            ESP_LOGI(TAG, "alive [%s] hb=%u busy=%d inv=%d",
                     LAN_BUILD_TAG, ++hb, (int)s_busy, (int)s_inv);
        }
    }
}
