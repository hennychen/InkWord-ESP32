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
 *   POST /api/display   协议 v2 双长度（2026-08-22 彩色传图）：
 *                       v1 单平面 epd_fb_size() 字节（行宽 PW/8，MSB
 *                       first，bit=1 白）——多平面面板余平面（红）设备侧
 *                       补零，旧客户端/脚本兼容，行为与历史版一致；
 *                       v2 双平面 epd_fb_total() 字节（[0]=B/W bit=1
 *                       白 + [1]=红 bit=1 红，与面板 plane 布局直通），
 *                       三色面板彩色传图；两者均 epd_full_refresh 整帧
 *                       直刷。BW 面板两长度相等自然退化 v1
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
 * 线程模型：handler 在 httpd 任务中直接刷屏；与按键任务的学习页渲染存在潜在竞争，
 *           测试版通过“接收页激活时屏蔽学习页渲染 + 外部直刷后强制下次全刷”缓解。
 */
#include "lan_display_server.h"
#include "debug_log.h"
#include "epd_driver.h"
#include "esp_mac.h"          /* v2.0：stats 端点 mac 字段（App 绑定凭据） */
#include "layout_profile.h"   /* 2026-08-25：TINY 档紧凑版式分派 */
#include "refresh_scheduler.h"
#include "wifi_manager.h"
#include "wifi_config_ui.h"
#include "study_mode_machine.h"
#include "deck_manager.h"
#include "learning_state.h"
#include "word_parser.h"
#include "storage_manager.h"
#include "gpio_config.h"      /* SD_MOUNT_POINT */

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
static uint8_t *s_frame = NULL;   /* 整帧接收缓冲：epd_fb_total() 首用分配
                                   * （Phase 6 多面板；协议 v2 双平面填满，
                                   * v1 单平面余平面清零） */

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
/* main.cpp 提供：切书编排（v1.3 T3.1：NVS+词库重载+状态作废+进度隔离） */
extern "C" bool deck_flow_switch(int idx);

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
#cv{background:#fff;border:2px solid #333;image-rendering:pixelated}
#dv{background:#fff;border:2px solid #333;image-rendering:pixelated}
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
</select>
<span id="tcRow" style="display:none">　字色：
<label><input type="radio" name="tc" value="0" checked onchange="render()">黑</label>
<label><input type="radio" name="tc" value="1" onchange="render()">红</label>
</span></div>
</div>
<div id="imgPanel" class="row" style="display:none">
<input type="file" id="fi" accept="image/*" onchange="onFile()">
</div>
<div class="row">
<label><input type="checkbox" id="dith" checked onchange="render()">抖动（灰度模拟）</label>
&nbsp;&nbsp;
<label><input type="checkbox" id="inv" onchange="render()">反色</label>
</div>
<div class="row" id="colRow" style="display:none">
<label><input type="checkbox" id="col" checked onchange="render()">彩色（黑白红最近色量化）</label>
</div>
<div class="row"><canvas id="cv"></canvas></div>
<div class="row">设备视角（横持设备时的效果）：<br>
<canvas id="dv"></canvas></div>
<button onclick="send()">发送到墨水屏</button>
<div id="st">预览上方画布（__PW__x__PH__）→ 点击发送</div>
<script>
var W=__PW__,H=__PH__,GW=__GW__,GH=__GH__,BPR=W/8,COLOR=__COLOR__;
var cv=document.getElementById('cv'),ctx=cv.getContext('2d');
var dv=document.getElementById('dv');
cv.width=W;cv.height=H;dv.width=GW;dv.height=GH;
cv.style.width=W+'px';dv.style.width=(GW/2)+'px';
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
 * 90°/270° 时视口交换宽高（H x W），即横竖屏互换排布（与设备
 * gfx 奇数旋转的横屏视角对齐；横向原生面板则互换为竖屏） */
function renderText(){
  var fs=+document.getElementById('fs').value;
  ctx.font=fs+'px sans-serif';
  var tc=document.querySelector('input[name=tc]:checked');
  ctx.fillStyle=(COLOR&&tc&&tc.value=='1')?'#f00':'#000';
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
  if(r==90||r==270){aw=H;ah=W;} /* 旋转后可用视口交换宽高 */
  var s=Math.min(aw/img.width,ah/img.height);
  var dw=img.width*s,dh=img.height*s;
  ctx.save();
  ctx.translate(W/2,H/2);
  ctx.rotate(r*Math.PI/180);
  ctx.drawImage(img,-dw/2,-dh/2,dw,dh);
  ctx.restore();
}
/* 设备视角预览：竖置面板（gfx 奇数旋转，GW!=W）缓冲旋 -90° 呈横持
 * 视角（与固件转置方向互补）；横向原生面板（gfx_rotation=0，GW==W）
 * gfx 即面板方向，直接呈现。彩色模式预览量化结果（WYSIWYG） */
var s_quant=null;
function colOn(){return COLOR&&document.getElementById('col').checked;}
/* 三色量化（2026-08-22 彩色传图）：{黑,白,红} RGB 最近色 + Floyd-
 * Steinberg 三通道误差扩散（dith）；误差按 inv 映射后显示色算（屏
 * 幕实际呈现色）。输出 s_quant={bw,rd,q}：双平面帧 + 量化预览画布 */
function quantize(){
  var d=ctx.getImageData(0,0,W,H).data;
  var buf=new Float32Array(W*H*3);
  for(var i=0;i<W*H;i++){buf[i*3]=d[i*4];buf[i*3+1]=d[i*4+1];buf[i*3+2]=d[i*4+2];}
  var inv=document.getElementById('inv').checked;
  var dith=document.getElementById('dith').checked;
  var bw=new Uint8Array(BPR*H),rd=new Uint8Array(BPR*H);
  var PAL=[[0,0,0],[255,255,255],[255,0,0]];
  var q=document.createElement('canvas');q.width=W;q.height=H;
  var qc=q.getContext('2d'),im=qc.createImageData(W,H);
  for(var y=0;y<H;y++)for(var x=0;x<W;x++){
    var i=y*W+x,r=buf[i*3],g=buf[i*3+1],b=buf[i*3+2];
    var best=0,bd=1e12;
    for(var p=0;p<3;p++){var dr=r-PAL[p][0],dg=g-PAL[p][1],db=b-PAL[p][2],
      dist=dr*dr+dg*dg+db*db;if(dist<bd){bd=dist;best=p;}}
    var cls=best;if(inv&&cls!=2)cls=1-cls; /* 反色：黑白互换红保持 */
    if(cls==1)bw[y*BPR+(x>>3)]|=0x80>>(x&7);
    if(cls==2)rd[y*BPR+(x>>3)]|=0x80>>(x&7);
    var c=PAL[cls];
    im.data[i*4]=c[0];im.data[i*4+1]=c[1];im.data[i*4+2]=c[2];im.data[i*4+3]=255;
    if(dith){
      var er=r-c[0],eg=g-c[1],eb=b-c[2];
      if(x+1<W){buf[(i+1)*3]+=er*7/16;buf[(i+1)*3+1]+=eg*7/16;buf[(i+1)*3+2]+=eb*7/16;}
      if(y+1<H){
        if(x>0){buf[(i+W-1)*3]+=er*3/16;buf[(i+W-1)*3+1]+=eg*3/16;buf[(i+W-1)*3+2]+=eb*3/16;}
        buf[(i+W)*3]+=er*5/16;buf[(i+W)*3+1]+=eg*5/16;buf[(i+W)*3+2]+=eb*5/16;
        if(x+1<W){buf[(i+W+1)*3]+=er/16;buf[(i+W+1)*3+1]+=eg/16;buf[(i+W+1)*3+2]+=eb/16;}
      }
    }
  }
  qc.putImageData(im,0,0);
  s_quant={bw:bw,rd:rd,q:q};
}
function devView(){
  var src=cv;
  if(colOn()){quantize();src=s_quant.q;}
  var dc=dv.getContext('2d');
  dc.fillStyle='#fff';dc.fillRect(0,0,GW,GH);
  dc.save();
  if(GW!=W){
    dc.translate(GW/2,GH/2);
    dc.rotate(-Math.PI/2);
    dc.drawImage(src,-W/2,-H/2);
  }else{
    dc.drawImage(src,0,0);
  }
  dc.restore();
}
function render(){
  base();
  if(document.querySelector('input[name=mode]:checked').value=='text')renderText();
  else drawImg();
  devView();
}
/* Canvas → 面板物理 1bpp 帧：行宽 W/8 字节，MSB first，bit=1 白 */
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
  var body;
  if(colOn()){ /* 协议 v2：双平面（B/W bit=1 白 + 红 bit=1 红）拼接 */
    quantize();
    body=new Uint8Array(s_quant.bw.length+s_quant.rd.length);
    body.set(s_quant.bw,0);body.set(s_quant.rd,s_quant.bw.length);
  }else body=pack();
  fetch('/api/display',{
    method:'POST',
    headers:{'Content-Type':'application/octet-stream'},
    body:body
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
if(COLOR){ /* 三色面板：彩色 UI 显现（BW 面板零变化） */
  document.getElementById('colRow').style.display='';
  document.getElementById('tcRow').style.display='';
}
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

/* 上传页几何占位符替换（Phase 6 多面板）：__PW__/__PH__ 面板物理尺寸
 * （浏览器画布与帧格式），__GW__/__GH__ GFX 几何（设备视角预览），
 * __COLOR__ 色彩能力（fb_total>fb_size 即多平面，彩色 UI 注入）。
 * 单遍扫描就地展开；缓冲预留 32B 余量（5 个占位符均短） */
static size_t page_subst(const char *tpl, char *out, size_t out_cap,
                         const char *vals[5])
{
    static const char *const tags[5] =
        {"__PW__", "__PH__", "__GW__", "__GH__", "__COLOR__"};
    size_t o = 0, i = 0;
    while (tpl[i] && o + 1 < out_cap) {
        int sub = -1;
        if (tpl[i] == '_') {
            for (int k = 0; k < 5; k++) {
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
    char pw[8], ph[8], gw[8], gh[8], col[8];
    snprintf(pw, sizeof(pw), "%d", epd_panel_width());
    snprintf(ph, sizeof(ph), "%d", epd_panel_height());
    snprintf(gw, sizeof(gw), "%d", epd_gfx_width());
    snprintf(gh, sizeof(gh), "%d", epd_gfx_height());
    snprintf(col, sizeof(col), "%d",
             epd_fb_total() > epd_fb_size() ? 1 : 0); /* 多平面=彩色 */
    const char *vals[5] = {pw, ph, gw, gh, col};

    const size_t cap = sizeof(PAGE_HTML) + 32;
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
    /* LAN 协议 v2 双长度（2026-08-22 彩色传图）：单平面 fb_size()（v1
     * 兼容，多平面面板余平面补零）或双平面 fb_total()（[0]=B/W bit=1
     * 白 + [1]=红 bit=1 红，与面板 plane 布局直通） */
    const size_t frame_bytes = epd_fb_size();
    const size_t total_bytes = epd_fb_total();
    const bool color_frame = req->content_len == (size_t)total_bytes &&
                             total_bytes > frame_bytes;
    if (req->content_len != frame_bytes && !color_frame) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "body must be %u (1bpp bw) or %u bytes (bw+red planes)",
                 (unsigned)frame_bytes, (unsigned)total_bytes);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }
    const size_t recv_len = color_frame ? total_bytes : frame_bytes;

    if (!s_frame) { /* 首用分配（epd_driver_init 后几何就绪） */
        s_frame = (uint8_t *)malloc(epd_fb_total());
        if (!s_frame) {
            LOG_E("frame buffer alloc failed (%u B)", (unsigned)epd_fb_total());
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "no memory");
            return ESP_FAIL;
        }
    }

    int received = 0;
    while (received < (int)recv_len) {
        int r = httpd_req_recv(req, (char *)s_frame + received,
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
    /* v1 单平面 → 多平面面板余平面（红）清零；v2 双平面已填满直通 */
    if (!color_frame && total_bytes > frame_bytes)
        memset(s_frame + frame_bytes, 0x00, total_bytes - frame_bytes);

    /* 整帧直刷（面板物理原生格式），并同步两处“上一帧”语义：
     * 1) 残影调度局刷计数归零（外部全刷等价于一次全刷）；
     * 2) 学习界面下次渲染强制全刷（GFX previous 缓冲已失配） */
    epd_full_refresh(s_frame);
    refresh_notify_full_done();
    ui_force_full_refresh_next();

    LOG_I("LAN frame displayed (%d bytes%s)", received,
          color_frame ? ", color" : "");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
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
        httpd_query_key_value(query, "id", id, sizeof(id));
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
    int idx = 0;                              /* "" 恒在 [0] */
    for (int i = 0; i < deck_manager_count(); i++) {
        if (strcmp(deck_manager_at(i)->id, id) == 0) {
            idx = i;
            break;
        }
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
    cfg.max_uri_handlers = 6;                /* GET 通配 + POST×4（display/wifi/deck×2） */
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
    if (!s_portal_mode) return;   /* 非 portal 模式：仅清前台标志 */

    s_portal_mode = false;
    s_dns_run = false;            /* DNS 任务随 recv 超时自退（≤0.5s） */
    wifi_stop_softap();           /* 回纯 STA，自动重连已保存网络 */
    register_mdns();              /* portal 启动时未注册，此处补上 */

    LOG_I("AP portal exited by user");
}
