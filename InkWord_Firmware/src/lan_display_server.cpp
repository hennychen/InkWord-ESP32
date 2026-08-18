/**
 * @file lan_display_server.cpp
 * @brief 局域网直传显示服务实现（方案 B 测试）
 *
 * 端点：
 *   GET  /              内嵌上传页：Canvas 渲染 + Floyd-Steinberg 抖动，
 *                       在浏览器端产出竖屏 240x416 1bpp 帧（中文经浏览器字体渲染）；
 *                       文本/图片均支持旋转（自动/0/90/180/270°），
 *                       可选横屏排布满幅显示
 *   POST /api/display   原始整帧 12480 字节（行宽 30，MSB first，bit=1 白）
 *                       → epd_full_refresh 整帧直刷
 *   GET  /wifi          Wi-Fi 配网页：扫描列表选 SSID + 密码输入（两种模式均可用）
 *   GET  /api/wifi/scan|status、POST /api/wifi/connect（异步连接，状态轮询）
 *   GET  其他任意 URI   302 重定向（captive portal 探测域名 → 弹出配网页）
 *
 * 两种工作模式：
 *   STA 在线：mDNS http://inkword.local（iOS/macOS 佳，Android 兼容有限），
 *             主页右上角入口进 /wifi 直接换网；
 *   AP 配网（portal）：SoftAP InkWord-Setup + DNS 劫持（53 端口全应答 192.168.4.1）
 *             → 手机连热点后系统探测域名被重定向 → 自动弹出配网页；
 *             连接成功后自动回学习界面并关闭热点。
 *
 * 线程模型：handler 在 httpd 任务中直接刷屏；与按键任务的学习页渲染存在潜在竞争，
 *           测试版通过“接收页激活时屏蔽学习页渲染 + 外部直刷后强制下次全刷”缓解。
 */
#include "lan_display_server.h"
#include "debug_log.h"
#include "epd_driver.h"
#include "refresh_scheduler.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "study_mode_machine.h"

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

static const char *TAG = "LAN";

#define AP_IFACE_IP     "192.168.4.1"          /* SoftAP 默认网关 IP */
#define PORTAL_SSID     "InkWord-Setup"

static httpd_handle_t s_server = NULL;
static bool s_active = false;                  /* 接收页在前台 */
static uint8_t s_frame[EPD_FB_SIZE];           /* 整帧接收缓冲（12,480 字节） */

static bool s_portal_mode = false;             /* AP 配网门户激活 */
static bool s_portal_provision = false;        /* 无凭据配网场景（连上即自动关）；
                                                 * 有凭据时为 AP 直连模式（用户按键退出） */
static volatile bool s_dns_run = false;        /* DNS 劫持任务运行标志 */
static TaskHandle_t s_dns_task = NULL;
static TaskHandle_t s_portal_task = NULL;

/* main.cpp 提供：外部直刷后强制下一次学习界面渲染走全刷 */
extern "C" void ui_force_full_refresh_next(void);
/* main.cpp 提供：学习界面渲染入口（portal 结束后恢复画面用） */
extern "C" void ui_render_word(study_mode_t mode, int index);

/* ============================================================
 * 内嵌网页（单文件，无外部依赖；手机浏览器打开即用）
 * ============================================================ */
static const char PAGE_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>InkWord 发送</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:12px;background:#f5f5f5}
h2{margin:8px 0}
#cv{background:#fff;border:2px solid #333;width:240px;image-rendering:pixelated}
#dv{background:#fff;border:2px solid #333;width:208px;image-rendering:pixelated}
.row{margin:8px 0}
textarea,input[type=file],select{width:100%;box-sizing:border-box}
button{padding:12px 24px;font-size:16px;width:100%}
#st{margin-top:8px;padding:6px;background:#ddd;word-break:break-all}
</style>
</head>
<body>
<h2>InkWord 墨水屏发送</h2>
<p><a href="/wifi">Wi-Fi 设置</a></p>
<div class="row">
<label><input type="radio" name="mode" value="text" checked onchange="onMode()">文本</label>
&nbsp;&nbsp;
<label><input type="radio" name="mode" value="img" onchange="onMode()">图片</label>
</div>
<div class="row">旋转：
<select id="rot" onchange="render()">
<option value="auto" selected>自动（横图转横屏）</option>
<option value="0">0°（竖屏）</option>
<option value="90">90°（横屏）</option>
<option value="180">180°</option>
<option value="270">270°</option>
</select></div>
<div id="textPanel" class="row">
<textarea id="txt" rows="5">Hello InkWord 你好墨水屏</textarea>
<div>字号：
<select id="fs">
<option>16</option><option selected>24</option>
<option>32</option><option>48</option>
</select></div>
</div>
<div id="imgPanel" class="row" style="display:none">
<input type="file" id="fi" accept="image/*" onchange="onFile()">
</div>
<div class="row">
<label><input type="checkbox" id="dith" checked onchange="render()">抖动（灰度模拟）</label>
&nbsp;&nbsp;
<label><input type="checkbox" id="inv" onchange="render()">反色</label>
</div>
<div class="row"><canvas id="cv" width="240" height="416"></canvas></div>
<div class="row">设备横屏视角（横持设备时的效果）：<br>
<canvas id="dv" width="416" height="240"></canvas></div>
<button onclick="send()">发送到墨水屏</button>
<div id="st">预览上方画布（240x416）→ 点击发送</div>
<script>
var W=240,H=416,BPR=W/8;
var cv=document.getElementById('cv'),ctx=cv.getContext('2d');
var img=null;
function onMode(){
  var t=document.querySelector('input[name=mode]:checked').value=='text';
  document.getElementById('textPanel').style.display=t?'':'none';
  document.getElementById('imgPanel').style.display=t?'none':'';
  render();
}
function onFile(){
  var f=document.getElementById('fi').files[0];
  if(!f){img=null;render();return;}
  var r=new FileReader();
  r.onload=function(e){
    var i=new Image();
    i.onload=function(){img=i;render();};
    i.src=e.target.result;
  };
  r.readAsDataURL(f);
}
function base(){
  ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);
  ctx.fillStyle='#000';
}
/* 文本渲染：先按旋转后视口宽度断行收集，再整体旋转绘制（垂直居中）。
 * 90°/270° 时视口为 416x240，即横屏排布（与设备 GFX rotation=1 对齐） */
function renderText(){
  var fs=+document.getElementById('fs').value;
  ctx.font=fs+'px sans-serif';
  var r=curRot(),vw=W,vh=H;
  if(r==90||r==270){vw=H;vh=W;}
  var maxW=vw-16,lh=fs*1.3,li,ci;
  var src=document.getElementById('txt').value.split('\n'),out=[];
  for(li=0;li<src.length;li++){
    var chars=Array.from(src[li]),line='';
    for(ci=0;ci<chars.length;ci++){
      var t=line+chars[ci];
      if(ctx.measureText(t).width>maxW&&line!==''){
        out.push(line);line=chars[ci];
        if((out.length+1)*lh>vh)break;
      }else{line=t;}
    }
    out.push(line);
    if((out.length+1)*lh>vh)break;
  }
  var maxLines=Math.floor(vh/lh);
  if(out.length>maxLines)out.length=maxLines;
  ctx.save();
  ctx.translate(W/2,H/2);
  ctx.rotate(r*Math.PI/180);
  var x=-vw/2+8,y=-vh/2+fs+(vh-out.length*lh)/2;
  for(li=0;li<out.length;li++){ctx.fillText(out[li],x,y);y+=lh;}
  ctx.restore();
}
/* 当前旋转角：自动=图片模式且横图时转 90°（与 GFX rotation=1 横屏视角对齐，
 * 由 Adafruit writePixel case1 映推得：内容顺时针 90° 写入竖屏缓冲即横屏正立）；
 * 文本模式自动=0°（由用户手动选横屏） */
function curRot(){
  var v=document.getElementById('rot').value;
  if(v=='auto')return(mode()=='img'&&img&&img.width>img.height)?90:0;
  return +v;
}
function mode(){return document.querySelector('input[name=mode]:checked').value}
function drawImg(){
  if(!img)return;
  var r=curRot(),aw=W,ah=H;
  if(r==90||r==270){aw=H;ah=W;} /* 旋转后可用视口变为 416x240 */
  var s=Math.min(aw/img.width,ah/img.height);
  var dw=img.width*s,dh=img.height*s;
  ctx.save();
  ctx.translate(W/2,H/2);
  ctx.rotate(r*Math.PI/180);
  ctx.drawImage(img,-dw/2,-dh/2,dw,dh);
  ctx.restore();
}
/* 设备横屏视角预览：竖屏缓冲固定旋转 -90°（与固件 GFX rotation=1 一致） */
function devView(){
  var dc=document.getElementById('dv').getContext('2d');
  dc.fillStyle='#fff';dc.fillRect(0,0,416,240);
  dc.save();
  dc.translate(208,120);
  dc.rotate(-Math.PI/2);
  dc.drawImage(cv,-120,-208);
  dc.restore();
}
function render(){
  base();
  if(document.querySelector('input[name=mode]:checked').value=='text')renderText();
  else drawImg();
  devView();
}
/* Canvas → 竖屏 1bpp 帧：行宽 30 字节，MSB first，bit=1 白 */
function pack(){
  var d=ctx.getImageData(0,0,W,H).data;
  var g=new Float32Array(W*H);
  for(var i=0;i<W*H;i++)g[i]=0.299*d[i*4]+0.587*d[i*4+1]+0.114*d[i*4+2];
  var inv=document.getElementById('inv').checked;
  var out=new Uint8Array(BPR*H);
  var x,y,i,old,nw,bit,er;
  if(document.getElementById('dith').checked){
    for(y=0;y<H;y++)for(x=0;x<W;x++){
      i=y*W+x;old=g[i];nw=old>=128?255:0;
      bit=(nw===255);if(inv)bit=!bit;
      if(bit)out[y*BPR+(x>>3)]|=0x80>>(x&7);
      er=old-nw;
      if(x+1<W)g[i+1]+=er*7/16;
      if(y+1<H){
        if(x>0)g[i+W-1]+=er*3/16;
        g[i+W]+=er*5/16;
        if(x+1<W)g[i+W+1]+=er*1/16;
      }
    }
  }else{
    for(i=0;i<W*H;i++){
      x=i%W;y=(i-x)/W;
      bit=g[i]>=128;if(inv)bit=!bit;
      if(bit)out[y*BPR+(x>>3)]|=0x80>>(x&7);
    }
  }
  return out;
}
function send(){
  var st=document.getElementById('st');
  st.textContent='发送中...';
  fetch('/api/display',{
    method:'POST',
    headers:{'Content-Type':'application/octet-stream'},
    body:pack()
  }).then(function(r){
    return r.text().then(function(t){
      st.textContent=(r.status==200?'已显示: ':'失败(' + r.status + '): ')+t;
    });
  }).catch(function(e){
    st.textContent='发送失败: '+e;
  });
}
document.getElementById('txt').oninput=render;
document.getElementById('fs').onchange=render;
render();
</script>
</body>
</html>)HTML";

/* ============================================================
 * Wi-Fi 配网页（/wifi）：扫描选网 + 密码输入 + 状态轮询
 * ============================================================ */
static const char WIFI_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>InkWord Wi-Fi</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:12px;background:#f5f5f5}
h2{margin:8px 0}
#st{padding:8px;background:#ddd;margin:8px 0}
ul{list-style:none;padding:0}
li{padding:10px;background:#fff;margin:4px 0;border:1px solid #ccc}
li i{float:right;color:#888;font-style:normal}
li.s{border:2px solid #333}
input{width:100%;box-sizing:border-box;padding:10px;margin:4px 0}
button{padding:12px 24px;font-size:16px;width:100%}
</style>
</head>
<body>
<h2>Wi-Fi 设置</h2>
<div id="st">查询状态中...</div>
<div><button onclick="scan()">扫描网络</button></div>
<ul id="list"><li>点击“扫描网络”</li></ul>
<div>
<input id="ssid" placeholder="SSID">
<input id="pass" type="password" placeholder="密码">
<button onclick="conn()">连接</button>
</div>
<p><a href="/">← 返回发送页</a></p>
<script>
function scan(){
  document.getElementById('list').innerHTML='<li>扫描中...</li>';
  fetch('/api/wifi/scan').then(function(r){return r.json()}).then(function(d){
    var h='';
    d.forEach(function(a){
      h+='<li onclick="pick(this)" data-s="'+a.ssid+'">'+
         a.ssid+'<i>'+a.rssi+'dBm'+(a.auth?' 锁':'')+'</i></li>';
    });
    document.getElementById('list').innerHTML=h||'<li>未找到网络</li>';
  }).catch(function(e){
    document.getElementById('list').innerHTML='<li>扫描失败</li>';
  });
}
function pick(li){
  document.getElementById('ssid').value=li.dataset.s;
  var ls=document.getElementsByTagName('li');
  for(var i=0;i<ls.length;i++)ls[i].className='';
  li.className='s';
}
function conn(){
  var s=document.getElementById('ssid').value;
  if(!s){alert('请选择或输入 SSID');return}
  document.getElementById('st').textContent='连接中 '+s+' ...';
  fetch('/api/wifi/connect',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:s,pass:document.getElementById('pass').value})
  }).then(function(r){
    if(r.status!=200)document.getElementById('st').textContent='请求失败';
  }).catch(function(e){
    document.getElementById('st').textContent='请求失败:'+e;
  });
}
function tick(){
  fetch('/api/wifi/status').then(function(r){return r.json()}).then(function(s){
    var t=document.getElementById('st');
    if(s.state=='ok')t.innerHTML='已连接，设备 IP：<b>'+s.ip+'</b>，热点将自动关闭';
    else if(s.state=='connecting')t.textContent='连接中...';
    else if(s.state=='fail')t.textContent='连接失败，请检查密码后重试';
    else t.textContent='未连接';
  }).catch(function(){});
}
setInterval(tick,1500);tick();
scan();
</script>
</body>
</html>)HTML";

/* ============================================================
 * HTTP handlers
 * ============================================================ */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, sizeof(PAGE_HTML) - 1);
}

static esp_err_t wifi_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WIFI_HTML, sizeof(WIFI_HTML) - 1);
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
    if (req->content_len != EPD_FB_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "body must be 12480 bytes (240x416 1bpp)");
        return ESP_OK;
    }

    int received = 0;
    while (received < EPD_FB_SIZE) {
        int r = httpd_req_recv(req, (char *)s_frame + received,
                               EPD_FB_SIZE - received);
        if (r <= 0) {
            LOG_E("display upload recv failed at %d/%d", received, EPD_FB_SIZE);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }

    /* 整帧直刷（竖屏原生格式），并同步两处“上一帧”语义：
     * 1) 残影调度局刷计数归零（外部全刷等价于一次全刷）；
     * 2) 学习界面下次渲染强制全刷（GFX previous 缓冲已失配） */
    epd_full_refresh(s_frame);
    refresh_notify_full_done();
    ui_force_full_refresh_next();

    LOG_I("LAN frame displayed (%d bytes)", received);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
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
    cfg.max_uri_handlers = 4;                /* GET 通配 + POST×2 */
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

void lan_server_enter_receive_page(void)
{
    /* portal 模式下重绘配网提示页（AP 服务后台保持） */
    if (s_portal_mode) {
        lan_portal_enter();
        return;
    }

    lan_server_start(); /* 幂等；未联网时仅提示 */

    s_active = true;

    /* GFX 横屏 416x240，ASCII（FreeSans 无 CJK 字形） */
    epd_gfx_fill_screen(EPD_GFX_WHITE);

    epd_gfx_fill_rect(0, 0, EPD_GFX_WIDTH, 36, EPD_GFX_BLACK);
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
        epd_gfx_draw_text(16, 120, "Long press F for WiFi portal,",
                          EPD_GFX_BLACK, 2);
        epd_gfx_draw_text(16, 150, "or long press C (keyboard UI).",
                          EPD_GFX_BLACK, 2);
    }

    epd_gfx_flush();
    LOG_I("LAN receive page shown (server=%s ip=%s)",
          lan_server_is_running() ? "on" : "off", has_ip ? ip : "none");
}

void lan_server_leave_receive_page(void)
{
    s_active = false;
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
    char qbuf[512];
    char rbuf[560];

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
    if (xTaskCreate(dns_hijack_task, "dnshijack", 3072, NULL, 5,
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
            epd_gfx_fill_rect(0, 0, EPD_GFX_WIDTH, 36, EPD_GFX_BLACK);
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

        ui_render_word(study_mode_current(), 0);   /* 恢复学习界面 */
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

    /* 屏幕提示页（ASCII） */
    epd_gfx_fill_screen(EPD_GFX_WHITE);
    epd_gfx_fill_rect(0, 0, EPD_GFX_WIDTH, 36, EPD_GFX_BLACK);
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
    if (!s_portal_mode) return;   /* 非 portal 模式：仅清前台标志 */

    s_portal_mode = false;
    s_dns_run = false;            /* DNS 任务随 recv 超时自退（≤0.5s） */
    wifi_stop_softap();           /* 回纯 STA，自动重连已保存网络 */
    register_mdns();              /* portal 启动时未注册，此处补上 */

    LOG_I("AP portal exited by user");
}
