/**
 * @file lan_pages.h
 * @brief LAN 内嵌网页资产（自 lan_display_server.cpp 外移，P2 LAN 拆分
 *        第一步：资产/逻辑分离，字节零变化）
 *
 * 仅 lan_display_server.cpp 一个 TU include（static const 单实例，
 * 勿多处包含）。发送页手机浏览器打开即用；配网页配合 SoftAP portal。
 */
#ifndef INKWORD_LAN_PAGES_H
#define INKWORD_LAN_PAGES_H

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
<label><input type="radio" name="tc" value="1" onchange="render()"><span id="tcAccent">红</span></label>
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
<label><input type="checkbox" id="col" checked onchange="render()">彩色（黑白<span id="colAccent">红</span>最近色量化）</label>
</div>
<div class="row"><canvas id="cv"></canvas></div>
<div class="row">设备视角（横持设备时的效果）：<br>
<canvas id="dv"></canvas></div>
<button onclick="send()">发送到墨水屏</button>
<div id="st">预览上方画布（__PW__x__PH__）→ 点击发送</div>
<script>
var W=__PW__,H=__PH__,GW=__GW__,GH=__GH__,BPR=W/8,COLOR=__COLOR__;
/* 第三色随面板注入（desc.accent_rgb：红屏 [255,0,0]；
 * BW 面板 [0,0,0] 不显现）；文案/预览/量化调色板同源 */
var ACC=[__ACC__];
var ACSS='rgb('+ACC.join(',')+')';
var cv=document.getElementById('cv'),ctx=cv.getContext('2d');
var dv=document.getElementById('dv');
function applyGeom(){
  cv.width=W;cv.height=H;dv.width=GW;dv.height=GH;
  cv.style.width=W+'px';dv.style.width=(GW/2)+'px';
}
applyGeom();
var img=null;
/* T2.3 device-info 自适配：本设备页同源值等价 no-op；外部托管页
 * （占位符无注入值）fetch 校正几何后重建画布重绘；fetch 失败静默
 * 回退模板注入值（no-fetch 回退保留） */
fetch('/api/device-info').then(function(r){return r.json()}).then(function(d){
  if(d.panel_w!=W||d.panel_h!=H||d.gfx_w!=GW||d.gfx_h!=GH||(d.plane_count>1)!=(COLOR==1)){
    W=d.panel_w;H=d.panel_h;GW=d.gfx_w;GH=d.gfx_h;BPR=W/8;
    COLOR=d.plane_count>1?1:0;ACC=d.accent_rgb;ACSS='rgb('+ACC.join(',')+')';
    applyGeom();setupColorUI();render();
  }
}).catch(function(){});
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
  ctx.fillStyle=(COLOR&&tc&&tc.value=='1')?ACSS:'#000';
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
  var PAL=[[0,0,0],[255,255,255],ACC];
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
  /* T2.3 协议 v2 帧：8B 头 'I''W' + ver2 + bpp(1=BW/2=+accent) +
   * W/H 大端 + body；设备侧 lan_frame_classify 按长度分流，
   * v1/v1.5 旧客户端裸 body 仍兼容 */
  if(colOn())quantize();
  var bw=colOn()?s_quant.bw:pack();
  var planes=colOn()?2:1;
  var body=new Uint8Array(8+bw.length+(planes==2?s_quant.rd.length:0));
  body[0]=0x49;body[1]=0x57;body[2]=2;body[3]=planes;
  body[4]=W>>8;body[5]=W&255;body[6]=H>>8;body[7]=H&255;
  body.set(bw,8);if(planes==2)body.set(s_quant.rd,8+bw.length);
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
function setupColorUI(){ /* 三色面板：彩色 UI 显现（BW 面板零变化） */
  var on=COLOR?'':'none';
  document.getElementById('colRow').style.display=on;
  document.getElementById('tcRow').style.display=on;
}
setupColorUI();
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


#endif /* INKWORD_LAN_PAGES_H */
