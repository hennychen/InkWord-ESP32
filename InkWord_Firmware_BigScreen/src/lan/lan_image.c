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
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "waveform_scanq.h"

// [run104] WiFi 配置页面（参考 InkWord_Firmware lan_pages.h WIFI_HTML）
//   扫描列表 + SSID 选择 + 密码输入 + 异步连接 + 状态轮询
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
// [run100] gray16 灰阶塌缩修复：LCD 并行路径不执行波形 phase_times（每相位=
//   一次 1080 行全扫 ≈110ms），GC16 中间灰阶的 8~20ms 白驱回修被过驱 ~10 倍
//   → 4bpp 直传上屏几乎全白（bringup 文档 §15）。本轮：设备侧 FS 二值化兜底
//   （/dither 可关）+ 页面默认 1bit FS 抖动；专用波形到位前 gray16 直出不可用。
// [run101] 扫描量子化专用波形落地（§16 第三档）：waveform_scanq 以 110ms/扫描
//   为最小时间量子重排 GC16（黑饱和 nsat 扫 + 白回修 map[to] 扫定级），/wf
//   端点远程切换波形与重排参数（免重烧迭代标定）；配合 /dither=off + 灰阶
//   标板（tools/gen_gray_ramp.py）拍照判读收敛 map。
// [run104] 临时开放 SoftAP + Captive Portal（参考 InkWord_Firmware）：
//   无 NVS，无串口输入。每次启动创建开放 SoftAP，DNS 劫持 captive portal。
//   WiFi 配置页扫描/选择/连接目标 WiFi（APSTA 模式，SoftAP 保持）。
// [run104] 临时开放 SoftAP + Captive Portal（参考 InkWord_Firmware 架构）：
//   无 NVS 持久化，无串口输入。每次启动创建开放 SoftAP（无密码），
//   DNS 劫持 + 302 重定向实现 captive portal（手机连上自动弹出配置页）。
//   WiFi 配置页扫描/选择/输入密码 → wifi_connect_async 异步连接（APSTA 模式），
//   连接成功后 STA 接口获取 IP，Mac 与设备在同一 WiFi 下可直接访问。
//   SoftAP 始终保持（192.168.4.1），STA 连接后双通道可达。
// [run105] captive portal 全链路打不开真机实证（弹窗页/手输 192.168.4.1
//   均"重定向次数过多"）：HTTPD_DEFAULT_CONFIG() 默认 max_uri_handlers=8，
//   而本服务注册 14 个端点，第 9 个起 httpd_register_uri_handler 槽满仅打
//   warning 且返回错误（httpd_uri.c）→ /wifi 未注册 → root 302→/wifi →
//   404 handler 再 302→/wifi → 无限自环。修复三件套：max_uri_handlers=16、
//   注册失败必打 ERROR（httpd_reg 封装）、404 handler 防自循环兜底。
//   附带勘误：run100 起 /dither、/wf（第 9/10 个注册）就一直没注册成功过。
// [run106] 屏幕诊断新增 /grayramp 端点 + 页面“16 级灰阶”按钮：设备侧直接
//   绘制 16 带竖带标板（tools/gen_gray_ramp.py 的 fb 等价物），配 /wf?wf=scanq
//   免电脑 curl 即可拍照迭代 map 标定；builtin 波形下可复证 §15 塌缩签名。
// [run107] 标板每带叠加带号标签（黑底白字 5x7 放大数字）：饱和黑白跃迁
//   免疫相位过驱 → 塌缩成全白的带标签仍可读，拍照判读可明确对位带号。
//   真机反馈：builtin 下带 >=5 全白曾被误读为“只显示一部分”，标签消除
//   该歧义（全带标签可见 + 背景全白 = §15 塌缩签名的预期形态，非漏刷）。
// [run108] 页面新增 2bit 抖动模式（4 级灰 nibble 0/5/10/15 + FS 误差扩散）：
//   设备端白名单识别（16 字节组合）后跳过 FS 二值化兜底直传——2bit 的
//   量化误差仅 1bit 的 1/3 → 纹理更细；中间级（5/10）依赖 scanq 波形，
//   builtin 下塌白可与 scanq 直接 A/B；黑白饱和级免疫相位过驱恒稳定。
// [run109] 波形三态 + binfast 二值快速波形：s_wf_scanq 升级 s_wf_kind 枚举
//   （builtin/scanq/binfast），/wf 扩展 wf=binfast 与 bn1/bn2 参数；页面
//   诊断区加波形切换按钮（手机完成标定循环，不再依赖地址栏）。binfast：
//   黑白跃迁各 n1/n2 扫（默认 3+3≈0.66s，vs GC16 3.3s），同色 nop 零注入
//   → 翻转少边界锐；扫描数真机迭代（不足签名：黑不黑/白不白）。
// [run110] WiFi 凭据 NVS 持久化：根因是 run104 起凭据只写 RAM，重启即失
//   （烧录=复位，感知“每次烧录后要重配”）。NVS 分区在 app 烧录时原样
//   保留（仅 erase/分区表变化/NVS 版本兑底擦除才丢）。GOT_IP 确认成功才
//   落盘（错误密码不写）；启动读 NVS 自动重连（SoftAP 保持兑底）；
//   /wifi_forget + 页面 Forget 按钮可清除。max_uri_handlers 16→24。
#define LAN_BUILD_TAG "run110"

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
// [run100] 设备侧 FS 二值化兜底开关（/dither 端点切换）：4bpp 上传先二值化再
//   入 fb，灰阶交给空间抖动表达（与页面 1bit FS 模式等效、预览即屏显）。
//   扫描量子化专用波形到位后关此开关恢复 4bpp 直传（§15 第三档）。
static volatile bool s_dither = true;
// [run101] 活动波形：builtin（ED047TC1 GC16，配 dither 兜底）或 scanq
//   （扫描量子化专用波形，配 /dither=off 直出灰阶）。/wf 端点切换；LUT 在
//   update 内逐相位现场解包（calculate_lut），故仅允许非 busy 时切换。
static const EpdWaveform* s_wf_builtin = NULL;
// [run109] 波形三态：builtin（ED047TC1 GC16）/ scanq（灰阶标定用）/
//   binfast（二值快速）。httpd 任务写、lan_image 任务读 → volatile。
typedef enum { WF_BUILTIN = 0, WF_SCANQ, WF_BINFAST } wf_kind_t;
static volatile wf_kind_t s_wf_kind = WF_BUILTIN;

static const char* wf_kind_str(wf_kind_t k) {
    return k == WF_SCANQ ? "scanq" : k == WF_BINFAST ? "binfast" : "builtin";
}
// [run102] STA 模式：WiFi 连接状态与 IP 地址
static volatile bool s_wifi_connected = false;
static char s_sta_ip[16] = {0};
// [run104] WiFi 配置页面 handler（/wifi 端点）
static esp_err_t wifi_page_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, WIFI_CONFIG_PAGE);
}

// [run104] Captive portal 状态
typedef enum { WCONN_IDLE, WCONN_CONNECTING, WCONN_OK, WCONN_FAIL } wconn_state_t;
static volatile wconn_state_t s_wconn = WCONN_IDLE;

// [run110] WiFi 凭据持久化（NVS lanwifi 命名空间）：烧录/重启后自动重连。
//   原为 wifi_connect_handler 函数内 static，提升至文件级供 GOT_IP 回调
//   （另一函数）在连接确认成功时写 NVS。
static char s_conn_ssid[33] = {0};
static char s_conn_pass[65] = {0};
static bool s_wifi_saved = false;    // NVS 有已存凭据（status 页回显用）

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
    if (nvs_open("lanwifi", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t l = sizeof(s_conn_ssid);
    bool ok = nvs_get_str(h, "ssid", s_conn_ssid, &l) == ESP_OK && s_conn_ssid[0] != '\0';
    if (ok) {
        l = sizeof(s_conn_pass);
        if (nvs_get_str(h, "pass", s_conn_pass, &l) != ESP_OK) {
            s_conn_pass[0] = '\0';
        }
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

// [run104] DNS 劫持任务（参考 InkWord_Firmware dns_hijack_task）：
//   所有 A 查询应答 192.168.4.1（AP 网关），手机连上开放 WiFi 后系统
//   connectivity check 域名被解析到本机 → HTTP 302 到 /wifi 配置页。
static volatile bool s_dns_run = false;
static TaskHandle_t s_dns_task = NULL;

static void dns_hijack_task(void* arg) {
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
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
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
                         (struct sockaddr*)&from, &flen);
        if (n < 12) continue;  // 超时/超短包
        if ((uint8_t)qbuf[5] != 1) continue;  // 仅处理 QDCOUNT=1

        uint8_t* b = (uint8_t*)qbuf;
        int p = 12;
        while (p < n && b[p]) p += b[p] + 1;  // 跳过 QNAME
        p += 5;  // NULL + QTYPE + QCLASS
        if (p > n) continue;
        int qlen = p;
        uint16_t qtype = (uint16_t)((b[p-4] << 8) | b[p-3]);

        // 响应 = 原查询（头 + Question）+ 可选应答段
        int rl = qlen;
        memcpy(rbuf, qbuf, qlen);
        rbuf[2] = 0x85;  // QR=1 AA=1 RD=1
        rbuf[3] = 0x80;  // RA=1 RCODE=0
        rbuf[6] = 0;
        rbuf[7] = (qtype == 1) ? 1 : 0;  // ANCOUNT
        if (qtype == 1) {  // 仅 A 查询回答
            static const uint8_t ans[16] = {
                0xC0, 0x0C,        // NAME 指针 → QNAME
                0x00, 0x01,        // TYPE=A
                0x00, 0x01,        // CLASS=IN
                0x00, 0x00, 0x00, 0x1E,  // TTL=30s
                0x00, 0x04,        // RDLENGTH=4
                192, 168, 4, 1     // RDATA = AP 网关
            };
            memcpy(rbuf + rl, ans, sizeof(ans));
            rl += sizeof(ans);
        }
        sendto(sock, rbuf, rl, 0, (struct sockaddr*)&from, flen);
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
    s_dns_run = false;  // 任务 recv 超时后自退（≤0.5s）
}

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
    JOB_GRAYRAMP = 5,  // 16 级灰阶标板（scanq 标定判读 / 塌缩签名复证）
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
// [run100] 设备侧 Floyd–Steinberg 二值化：4bpp gray16（s_raw）→ 二值半字节（fb）。
//   整数实现：灰阶 g=nibble*17（0..255），误差按 1/16 在「当前行/下一行」两条
//   int32 行缓冲间传递（(1920+2)*4B*2 ≈ 15KB，内部堆充裕）；阈值 128 与页面
//   thr 默认一致，设备侧与浏览器侧二值化口径统一。
static void dither_4bpp_to_fb(uint8_t* fb) {
    const int w = IMG_W, h = IMG_H;
    int32_t* err[2] = {
        (int32_t*)calloc((size_t)w + 2, sizeof(int32_t)),
        (int32_t*)calloc((size_t)w + 2, sizeof(int32_t)),
    };
    if (err[0] == NULL || err[1] == NULL) {
        ESP_LOGE(TAG, "dither err buf alloc fail, fallback memcpy");
        free(err[0]);
        free(err[1]);
        memcpy(fb, s_raw, FB_BYTES);
        return;
    }
    memset(fb, 0x00, FB_BYTES);          // 全黑底，仅按需置白（同 1bit 解包风格）
    int cur = 0;
    for (int y = 0; y < h; y++) {
        int32_t* ec = err[cur];          // 当前行误差
        int32_t* en = err[cur ^ 1];      // 下一行误差
        for (int x = 0; x < w; x++) {
            size_t p = (size_t)y * w + (size_t)x;
            uint8_t b = s_raw[p >> 1];
            int32_t v = (int32_t)((p & 1) ? (b >> 4) : (b & 0x0F)) * 17;
            int32_t old = v + ec[x];
            int white = old >= 128;
            int32_t e = old - (white ? 255 : 0);
            if (white) {
                if (p & 1) {
                    fb[p >> 1] |= 0xF0;
                } else {
                    fb[p >> 1] |= 0x0F;
                }
            }
            if (x + 1 < w) ec[x + 1] += e * 7 / 16;
            if (y + 1 < h) {
                if (x > 0) en[x - 1] += e * 3 / 16;
                en[x] += e * 5 / 16;
                if (x + 1 < w) en[x + 1] += e / 16;
            }
        }
        // 行 rollover：当前行缓冲已消费完，清零后换作下一轮的「下一行」缓冲
        memset(ec, 0, ((size_t)w + 2) * sizeof(int32_t));
        cur ^= 1;
    }
    free(err[0]);
    free(err[1]);
}

// [run108] 2bit（4 级）上传识别：页面 fs2 模式只产 nibble {0,5,10,15}，
//   合法字节仅 16 种组合；扫描命中即视为 2bit 内容。纯黑白图天然满足
//   （nibble 仅 0/15），跳过二值化兜底无损。1MB 扫描 ~1ms 可忽略。
static bool nib_is_2bit(uint8_t n) {
    return n == 0 || n == 5 || n == 10 || n == 15;
}

static bool raw_is_2bit(void) {
    for (size_t i = 0; i < FB_BYTES; i++) {
        uint8_t b = s_raw[i];
        if (!nib_is_2bit((uint8_t)(b & 0x0F)) || !nib_is_2bit((uint8_t)(b >> 4))) {
            return false;
        }
    }
    return true;
}

static void unpack_raw_to_fb(void) {
    uint8_t* fb = epd_hl_get_framebuffer(&hl);
    if (s_raw_len == FB_BYTES) {
        // [run108] 2bit 直传：识别为 4 级数据时跳过二值化兜底，中间级交
        //   物理灰阶（scanq；builtin 下塌白可观察，黑白级不受影响）。
        if (s_dither && !raw_is_2bit()) {
            dither_4bpp_to_fb(fb);
        } else {
            memcpy(fb, s_raw, FB_BYTES);
        }
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

// [run107] 5x7 数字点阵（每行低 5 位有效，经典 LED 字体）：标板带号 0..15
static const uint8_t GR_DIGIT_5X7[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
};

// [run106] 16 级灰阶标板（tools/gen_gray_ramp.py 的 fb 等价物）：16 条竖带
//   各 120px 宽，nibble 0..15 左黑右白。配 /wf?wf=scanq 拍照迭代 map 标定；
//   builtin 下预期 §15 塌缩签名（带 0 黑、1~4 渐隐、>=5 全白）。带宽 120
//   为偶数 → 半字节对（偶/奇 x）不跨带，行模板按字节 q|(q<<4) 直接铺。
//   back 取纯色底：每行横跨 16 带必与纯色不同，无干净行跳过风险。
// [run107] 每带垂直居中叠加带号标签：黑底框 + 白色 5x7 数字放大 8 倍
//   （40x56/位，两位数框 112x80 < 带宽 120）。饱和黑白跃迁免疫相位过驱，
//   塌缩带上标签依然可读；s_inv 时底/字色互换。
static void draw_gray_ramp_to_fb(void) {
    uint8_t* fb = epd_hl_get_framebuffer(&hl);
    const int band = IMG_W / 16;                  // 120px/带（1920 被 16 整除）
    uint8_t row[IMG_W / 2];                       // 960B 行模板（栈余量充裕）
    for (int xb = 0; xb < IMG_W / 2; xb++) {
        int t = (xb * 2) / band;
        uint8_t q = s_inv ? (uint8_t)(15 - t) : (uint8_t)t;
        row[xb] = (uint8_t)(q | (q << 4));        // 偶 x=低半字节 / 奇 x=高半字节
    }
    for (int y = 0; y < IMG_H; y++) {
        memcpy(fb + (size_t)y * (IMG_W / 2), row, IMG_W / 2);
    }
    // [run107] 带号标签：黑底框 + 白字（s_inv 时互换），垂直居中带内水平居中
    const int scale = 8, pad = 12;
    const int box_h = 7 * scale + 2 * pad;              // 80，1080 居中
    const int y0 = (IMG_H - box_h) / 2;
    for (int t = 0; t < 16; t++) {
        const int digits = (t >= 10) ? 2 : 1;
        const int txt_w = digits * 5 * scale + (digits - 1) * scale;
        const int box_w = txt_w + 2 * pad;              // 64 或 112（< 带宽 120）
        const int x0 = t * band + (band - box_w) / 2;
        const int bg_w = s_inv ? 1 : 0, fg_w = s_inv ? 0 : 1;
        for (int y = y0; y < y0 + box_h; y++) {
            for (int x = x0; x < x0 + box_w; x++) {
                fb_px(fb, x, y, bg_w);
            }
        }
        for (int d = 0; d < digits; d++) {
            const int digit = (digits == 2) ? ((d == 0) ? t / 10 : t % 10) : t;
            for (int r = 0; r < 7; r++) {
                const uint8_t bits = GR_DIGIT_5X7[digit][r];
                for (int c = 0; c < 5; c++) {
                    if (!(bits & (0x10 >> c))) continue;
                    const int px = x0 + pad + (d * 6 + c) * scale;
                    const int py = y0 + pad + r * scale;
                    for (int yy = 0; yy < scale; yy++) {
                        for (int xx = 0; xx < scale; xx++) {
                            fb_px(fb, px + xx, py + yy, fg_w);
                        }
                    }
                }
            }
        }
    }
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
// [run105] 端点注册封装：槽满时 httpd_register_uri_handler 仅 warning +
//   返回错误、不中止（httpd_uri.c L189），裸调用会静默丢端点（run104 事故
//   根因：/wifi 未注册导致 302 自环）→ 失败必打 ERROR 留痕。
static void httpd_reg(httpd_handle_t h, const httpd_uri_t* u) {
    if (httpd_register_uri_handler(h, u) != ESP_OK) {
        ESP_LOGE(TAG, "uri register fail: %s", u->uri);
    }
}

static esp_err_t root_handler(httpd_req_t* req) {
    // [run104] STA 未连接时所有 GET 重定向到 /wifi 配置页（captive portal）
    //   手机连开放 WiFi 后系统 connectivity check 被 DNS 劫持到 192.168.4.1，
    //   任意 HTTP 请求 → 302 → /wifi 配置页，自动弹出。
    if (s_wconn != WCONN_OK && !s_wifi_connected) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wifi");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, "");
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, HTML_PAGE);
}

static esp_err_t status_handler(httpd_req_t* req) {
    char body[192];
    snprintf(body, sizeof(body),
             "{\"build\":\"%s\",\"busy\":%s,\"inv\":%d,\"dither\":%d,\"wf\":\"%s\",\"raw_bytes\":%d,\"w\":%d,\"h\":%d,\"wifi\":\"%s\",\"sta_ip\":\"%s\"}",
             LAN_BUILD_TAG, s_busy ? "true" : "false", (int)s_inv, (int)s_dither,
             wf_kind_str(s_wf_kind), RAW_BYTES, IMG_W, IMG_H,
             s_wifi_connected ? "sta" : "ap", s_sta_ip);
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
static esp_err_t grayramp_handler(httpd_req_t* req) { return job_handler(req, JOB_GRAYRAMP); }

// /pol：运行时反转黑白极性（GET/POST 均可），不驱屏、不阻塞
static esp_err_t pol_handler(httpd_req_t* req) {
    s_inv = !s_inv;
    char body[64];
    snprintf(body, sizeof(body), "{\"inv\":%d,\"build\":\"%s\"}", (int)s_inv, LAN_BUILD_TAG);
    ESP_LOGW(TAG, "polarity inverted -> inv=%d", (int)s_inv);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// [run100] /dither：运行时切换 4bpp 上传的设备侧 FS 二值化兜底（GET/POST 均可），
//   不驱屏、不阻塞。标定扫描量子化波形时先关（/dither → dither=0）再传灰阶标板。
static esp_err_t dither_handler(httpd_req_t* req) {
    s_dither = !s_dither;
    char body[64];
    snprintf(body, sizeof(body), "{\"dither\":%d,\"build\":\"%s\"}", (int)s_dither, LAN_BUILD_TAG);
    ESP_LOGW(TAG, "device dither %s", s_dither ? "on" : "off");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// [run101] /wf：扫描量子化波形远程调参（§16 标定闭环，免重烧）：
//   GET /wf                          → 报告当前波形与参数
//   GET /wf?wf=scanq|builtin         → 切换活动波形（下次更新生效）
//   GET /wf?nsat=N&mmax=M&map=a,b,.. → 重排 scanq 参数（map 须 16 个 0..mmax 整数）
//   busy 时拒绝一切变更：LUT 在 update 内逐相位现场解包，中途换波形=撕裂。
// [run103] WiFi 扫描端点（配置模式下可用）
static esp_err_t wifi_scan_handler(httpd_req_t* req) {
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 150,
    };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, true));
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    wifi_ap_record_t* ap_records = calloc(num, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc fail");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&num, ap_records);

    // 构建 JSON 响应
    char body[2048];
    int off = snprintf(body, sizeof(body), "{\"networks\":[");
    for (int i = 0; i < num && off < (int)sizeof(body) - 128; i++) {
        if (ap_records[i].ssid[0] == '\0') continue;  // 跳过隐藏 SSID
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%s}",
                        i ? "," : "",
                        (char*)ap_records[i].ssid,
                        ap_records[i].rssi,
                        ap_records[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    free(ap_records);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// [run104] WiFi 异步连接端点（POST JSON，参考 InkWord lan_display_server）
//   不保存 NVS，仅临时连接。SoftAP 始终保持。
static esp_err_t wifi_connect_handler(httpd_req_t* req) {
    char body[192];
    if (req->content_len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "body too large");
    }
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) return ESP_FAIL;
    body[r] = '\0';

    // 简单 JSON 解析（ssid + pass）
    char ssid[33] = {0}, pass[65] = {0};
    char* p;
    if ((p = strstr(body, "\"ssid\""))) {
        p = strchr(p + 7, '"'); if (p) p++;
        if (p) { char* e = strchr(p, '"'); if (e) { size_t l = e - p; if (l > 32) l = 32; memcpy(ssid, p, l); } }
    }
    if ((p = strstr(body, "\"pass\""))) {
        p = strchr(p + 7, '"'); if (p) p++;
        if (p) { char* e = strchr(p, '"'); if (e) { size_t l = e - p; if (l > 64) l = 64; memcpy(pass, p, l); } }
    }
    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ssid required");
    }

    ESP_LOGW(TAG, "WiFi connect: SSID=%s, PASS=%s", ssid, pass[0] ? "***" : "(open)");
    // 启动异步连接任务
    s_wconn = WCONN_CONNECTING;
    // [run110] 凭据复制到文件级缓冲（GOT_IP 确认成功后写 NVS）
    strncpy(s_conn_ssid, ssid, 32);
    strncpy(s_conn_pass, pass, 64);

    // 配置 STA 并连接（APSTA 模式下 STA 接口）
    wifi_config_t wc = {0};
    strncpy((char*)wc.sta.ssid, s_conn_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, s_conn_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_disconnect();
    esp_wifi_connect();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// [run104] WiFi 状态查询端点（参考 InkWord wifi_status_get_handler）
static esp_err_t wifi_status_handler(httpd_req_t* req) {
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

// [run110] 清除已存凭据并断开 STA（/wifi 页 Forget 按钮）
static esp_err_t wifi_forget_handler(httpd_req_t* req) {
    wifi_forget_credentials();
    s_wconn = WCONN_IDLE;
    esp_wifi_disconnect();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"saved\":false}");
}

static esp_err_t wf_handler(httpd_req_t* req) {
    char q[192];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        q[0] = '\0';
    }
    char val[96];
    int nsat, mmax, map[16];
    scanq_get_params(&nsat, &mmax, map);
    int n_nsat = nsat, n_mmax = mmax, n_map[16];
    memcpy(n_map, map, sizeof(n_map));
    int bn1, bn2;
    binfast_get_params(&bn1, &bn2);
    int n_bn1 = bn1, n_bn2 = bn2;
    bool rebuild = false, bf_rebuild = false, wf_set = false;
    wf_kind_t wf_kind = s_wf_kind;

    if (httpd_query_key_value(q, "wf", val, sizeof(val)) == ESP_OK) {
        if (strcmp(val, "scanq") == 0) {
            wf_kind = WF_SCANQ;
            wf_set = true;
        } else if (strcmp(val, "builtin") == 0) {
            wf_kind = WF_BUILTIN;
            wf_set = true;
        } else if (strcmp(val, "binfast") == 0) {
            wf_kind = WF_BINFAST;
            wf_set = true;
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wf=scanq|builtin|binfast");
            return ESP_OK;
        }
    }
    if (httpd_query_key_value(q, "nsat", val, sizeof(val)) == ESP_OK) {
        n_nsat = atoi(val);
        rebuild = true;
    }
    if (httpd_query_key_value(q, "mmax", val, sizeof(val)) == ESP_OK) {
        n_mmax = atoi(val);
        rebuild = true;
    }
    if (httpd_query_key_value(q, "map", val, sizeof(val)) == ESP_OK) {
        int t = 0;
        const char* p = val;
        while (p != NULL && t < 16) {
            char* end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            n_map[t++] = (int)v;
            p = (*end == ',') ? end + 1 : NULL;
        }
        if (t != 16) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "map needs 16 ints");
            return ESP_OK;
        }
        rebuild = true;
    }
    if (httpd_query_key_value(q, "bn1", val, sizeof(val)) == ESP_OK) {
        n_bn1 = atoi(val);
        bf_rebuild = true;
    }
    if (httpd_query_key_value(q, "bn2", val, sizeof(val)) == ESP_OK) {
        n_bn2 = atoi(val);
        bf_rebuild = true;
    }
    if ((rebuild || bf_rebuild || wf_set) && s_busy) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "busy");
        return ESP_OK;
    }
    if (rebuild && !scanq_rebuild(n_nsat, n_mmax, n_map)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad nsat/mmax/map");
        return ESP_OK;
    }
    if (bf_rebuild && !binfast_rebuild(n_bn1, n_bn2)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad bn1/bn2");
        return ESP_OK;
    }
    if (wf_set && wf_kind != s_wf_kind) {
        s_wf_kind = wf_kind;
        hl.waveform = (wf_kind == WF_SCANQ)   ? scanq_waveform()
                     : (wf_kind == WF_BINFAST) ? binfast_waveform()
                                               : s_wf_builtin;
        ESP_LOGW(TAG, "waveform -> %s", wf_kind_str(s_wf_kind));
    }
    scanq_get_params(&nsat, &mmax, map);
    binfast_get_params(&bn1, &bn2);
    char body[288];
    int off = snprintf(body, sizeof(body),
                       "{\"wf\":\"%s\",\"nsat\":%d,\"mmax\":%d,\"phases\":%d,\"bn1\":%d,\"bn2\":%d,\"map\":[",
                       wf_kind_str(s_wf_kind), nsat, mmax, nsat + mmax, bn1, bn2);
    for (int t = 0; t < 16 && off < (int)sizeof(body) - 4; t++) {
        off += snprintf(body + off, sizeof(body) - (size_t)off, "%s%d",
                        t ? "," : "", map[t]);
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "],\"build\":\"%s\"}",
             LAN_BUILD_TAG);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// [run99] 未注册路径一律 302 → /：既让各平台联网检测拿不到预期响应体
//   （captive portal 弹窗触发条件），也容忍用户手输任意 URL。
static esp_err_t portal_redirect_handler(httpd_req_t* req, httpd_err_code_t err) {
    (void)err;
    // [run104] 未注册路径：STA 未连接时重定向到 /wifi（captive portal），
    //   STA 已连接时重定向到 /（上传页）。
    const char* loc = (s_wconn != WCONN_OK && !s_wifi_connected) ? "/wifi" : "/";
    // [run105] 防自循环兜底：404 的路径若恰为重定向目标（目标端点注册失败，
    //   如槽满事故），再 302 即无限重定向 → 改回明文 404 暴露问题。
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
    ESP_LOGW(TAG, "recv ok: %u B (%s) dither=%d", (unsigned)want,
             want == FB_BYTES ? "4bpp gray16" : "1bit", (int)s_dither);

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

// ------------------------------------------------------------ WiFi STA ----
// [run102] STA 模式：ESP32 接入现有 WiFi，Mac 无需连 SoftAP 即可访问。
//   SSID/密码存 NVS，首次启动串口提示输入；后续自动连接。
//   串口输入 wifi_clear 清除配置重新输入。

static esp_netif_t* s_sta_netif = NULL;

// [run102] WiFi 事件处理：获取 IP 后打印
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
        // [run104] 不自动连接——等待 web UI 发起连接请求
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_sta_ip[0] = '\0';
        if (s_wconn == WCONN_CONNECTING) {
            // 连接失败（密码错误 / 不在范围）
            s_wconn = WCONN_FAIL;
            ESP_LOGW(TAG, "WiFi connect failed");
        } else if (s_wconn == WCONN_OK) {
            // 已连接后断线：自动重连
            ESP_LOGW(TAG, "WiFi disconnected, retrying...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_wconn = WCONN_OK;
        wifi_save_credentials();  // [run110] 连接确认成功才落盘（失败密码不写）
        dns_stop();  // [run104] STA 已连接，停止 DNS 劫持（不再需要 captive portal）
        ESP_LOGW(TAG, "WiFi connected! IP: %s", s_sta_ip);
        ESP_LOGW(TAG, ">>> AP: http://192.168.4.1  STA: http://%s <<<", s_sta_ip);
    }
}


// [run104] 临时开放 SoftAP + APSTA 模式（参考 InkWord_Firmware wifi_manager）
//   SoftAP 始终活跃（开放网络，无密码），STA 接口待命（web UI 触发连接）。
//   DNS 劫持 captive portal：手机连 SoftAP 后系统探测域名被重定向到配置页。
static void lan_wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // APSTA 模式：SoftAP（开放）+ STA 待命
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    (void)ap_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, NULL));

    // SoftAP 配置：开放网络（参考 InkWord wifi_start_softap）
    wifi_config_t ap_wc = {0};
    snprintf((char*)ap_wc.ap.ssid, sizeof(ap_wc.ap.ssid), "%s", LAN_SSID);
    ap_wc.ap.ssid_len = strlen(LAN_SSID);
    ap_wc.ap.channel = 6;
    ap_wc.ap.max_connection = 4;
    ap_wc.ap.authmode = WIFI_AUTH_OPEN;   // 开放网络：连上即可访问

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 关闭 WiFi 节能（AP 模式需随时响应 ARP/TCP）
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // [run110] NVS 有已存凭据则自动连接（烧录/重启后免重配）。SoftAP
    //   保持活跃作兑底入口；密码改错时 /wifi_forget 清除后重配。
    if (wifi_load_credentials()) {
        wifi_config_t wc = {0};
        strncpy((char*)wc.sta.ssid, s_conn_ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char*)wc.sta.password, s_conn_pass, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = s_conn_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        s_wconn = WCONN_CONNECTING;
        esp_wifi_connect();
        ESP_LOGW(TAG, "auto-connect to saved SSID: %s", s_conn_ssid);
    }

    // [run104] 启动 DNS 劫持（captive portal 核心：所有域名解析到 AP 网关）
    dns_start();

    ESP_LOGW(TAG, "=== TEMPORARY AP MODE [%s] ===", LAN_BUILD_TAG);
    ESP_LOGW(TAG, "SoftAP: %s (OPEN, no password)", LAN_SSID);
    ESP_LOGW(TAG, "Portal: http://192.168.4.1 (captive portal auto-redirect)");
    ESP_LOGW(TAG, "WiFi config page: http://192.168.4.1/wifi");
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
    // [run101] 记住内建波形（ES108FC default_waveform=&epdiy_ED047TC1）并
    //   预构建 scanq；默认仍 builtin，/wf?wf=scanq 切换后下次更新生效。
    s_wf_builtin = hl.waveform;
    scanq_init();
    // [run109] binfast 预构建默认参数（懒构建同效，这里提前拿到启动日志）
    (void)binfast_waveform();
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

    // [run104→run110] NVS 现存储 WiFi 凭据（lanwifi 命名空间）：烧录/重启
    //   后自动重连。版本不匹配时擦除重建属正常兑底（仅丢凭据需重配一次）。
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    lan_wifi_start();
    // SoftAP 已就绪，直接启动 httpd（STA 连接由 web UI 异步发起）

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    cfg.max_open_sockets = 4;
    // [run105] 默认 max_uri_handlers=8 < 本服务端点数：不设此值，第 9 个起
    //   （/dither、/wf、/wifi* 等）注册静默失败 → /wifi 丢失引发 302 自环
    //   （见顶部 run105 注释）。[run110] 16 端点满配 → 提至 24 留余量。
    cfg.max_uri_handlers = 24;
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
    static const httpd_uri_t uri_grayramp = {.uri = "/grayramp", .method = HTTP_POST, .handler = grayramp_handler};
    static const httpd_uri_t uri_pol = {.uri = "/pol", .method = HTTP_GET, .handler = pol_handler};
    static const httpd_uri_t uri_dither = {.uri = "/dither", .method = HTTP_GET, .handler = dither_handler};
    static const httpd_uri_t uri_wf = {.uri = "/wf", .method = HTTP_GET, .handler = wf_handler};
    httpd_reg(httpd, &uri_root);
    httpd_reg(httpd, &uri_upload);
    httpd_reg(httpd, &uri_status);
    httpd_reg(httpd, &uri_clear);
    httpd_reg(httpd, &uri_white);
    httpd_reg(httpd, &uri_black);
    httpd_reg(httpd, &uri_pattern);
    httpd_reg(httpd, &uri_grayramp);
    httpd_reg(httpd, &uri_pol);
    httpd_reg(httpd, &uri_dither);
    httpd_reg(httpd, &uri_wf);
    // [run104] WiFi 配置端点（始终注册：扫描 + 异步连接 + 状态查询）
    static const httpd_uri_t uri_wifi_scan2 = {.uri = "/wifi_scan", .method = HTTP_GET, .handler = wifi_scan_handler};
    static const httpd_uri_t uri_wifi_conn = {.uri = "/wifi_connect", .method = HTTP_POST, .handler = wifi_connect_handler};
    static const httpd_uri_t uri_wifi_st = {.uri = "/wifi_status", .method = HTTP_GET, .handler = wifi_status_handler};
    static const httpd_uri_t uri_wifi_forget = {.uri = "/wifi_forget", .method = HTTP_GET, .handler = wifi_forget_handler};
    static const httpd_uri_t uri_wifi_page = {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_page_handler};
    httpd_reg(httpd, &uri_wifi_scan2);
    httpd_reg(httpd, &uri_wifi_conn);
    httpd_reg(httpd, &uri_wifi_st);
    httpd_reg(httpd, &uri_wifi_forget);
    httpd_reg(httpd, &uri_wifi_page);
    // [run99] captive portal：未注册路径（含各平台联网检测路径）一律 302 → /
    httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, portal_redirect_handler);

    ESP_LOGW(TAG, "AP MODE ready [%s]: http://192.168.4.1 (SoftAP: %s)",
             LAN_BUILD_TAG, LAN_SSID);
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
                case JOB_GRAYRAMP:
                    draw_gray_ramp_to_fb();
                    s_job_err = (int)epd_hl_update_screen(&hl, MODE_GC16,
                                                          epd_ambient_temperature());
                    break;
            }
            s_job_ms = (int)((esp_timer_get_time() - t0) / 1000);
            ESP_LOGW(TAG, "job %d done: err=%d ms=%d inv=%d wf=%s",
                     (int)s_job, s_job_err, s_job_ms, (int)s_inv,
                     wf_kind_str(s_wf_kind));
            xSemaphoreGive(s_job_done);
        } else {
            ESP_LOGI(TAG, "alive [%s] hb=%u busy=%d inv=%d ip=%s",
                     LAN_BUILD_TAG, ++hb, (int)s_busy, (int)s_inv,
                     s_sta_ip[0] ? s_sta_ip : "(no ip)");
        }
    }
}
