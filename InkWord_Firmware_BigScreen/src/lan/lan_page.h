// InkWord BigScreen 上传页面（run99）
//
// 拆分为独立头文件的理由：run98 前 HTML 以 C 字符串内嵌于 lan_image.c，加
//   旋转/灰阶/适配等控件后会突破 300 行字符串拼接，与驱动逻辑混在一起不可
//   维护；本项目无文件系统与资源打包机制，故沿用"字符串常量 + include"。
//
// 页面能力：
//   1. 变换：旋转 0/90/180/270、水平/垂直镜像、适配模式（完整 contain /
//      填满 cover / 拉伸 stretch）、缩放与 XY 偏移（竖图横屏必备）；
//   2. 灰阶处理：亮度、对比度、Gamma、反色；
//   3. 输出模式：16 级灰阶直出（4bpp）/ 1bit Floyd-Steinberg 抖动 /
//      1bit 阈值，阈值可调；
//   3b. [run100] 默认 1bit FS 抖动：LCD 并行路径不执行波形 phase_times，
//      gray16 直传的中间灰阶会塌缩为白（bringup 文档 §15）；选灰阶直出时
//      设备默认再做一次 FS 二值化兜底（/dither 可关），屏显为抖动灰而非
//      平滑灰，预览仅示量化灰阶、与屏显纹理不同；
//   3c. [run108] 2bit 抖动：4 级灰（nibble 0/5/10/15）+ FS 误差扩散，量化
//      误差仅 1bit 的 1/3 → 纹理更细；设备白名单识别后跳过二值化兜底
//      直传，中间级依赖 scanq 波形，builtin 下塌白可 A/B；
//   3d. [run108] 清晰度三件套：量化前 USM 锐化（3x3 box 近似，预补墨水屏
//      边界扩散，照片建议 40~70%）；1bit Bayer 8x8 有序抖动（无 FS 蠕虫/
//      拖尾，打印业标准手法）；1bit FS 改蛇形扫描（奇偶行反向，消方向纹）；
//      [2026-09-17] USM 默认 0→50（墨水屏物理边界扩散普适，文字图可手动归零）
//   4. 预览即所得：打包后反解回 canvas，所见即屏显（含灰阶量化误差）；
//   5. 性能：拖动滑块只做 canvas 变换（实时），松手 220ms 后才做像素级
//      处理与打包（1920x1080 逐像素在手机上约数百 ms，不可每帧做）；
//   6. [run109] 波形三态切换：builtin/scanq/binfast 页面直切（原需地址栏
//      /wf?wf=..），手机即可完成标定循环；binfast 为二值快速波形（更锐更快，
//      只配 1bit 模式）；nsat/mmax/map 与 bn1/bn2 细参数仍经 /wf 查询串调整。
//
// 半字节布局必须与固件 fb 完全一致（偶 x = 低半字节 / 奇 x = 高半字节，
//   15 = 白、0 = 黑），否则 4bpp 直传（固件端 memcpy）会错位。
#ifndef LAN_PAGE_H
#define LAN_PAGE_H

static const char HTML_PAGE[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>InkWord BigScreen</title><style>"
    "body{font-family:system-ui,-apple-system,sans-serif;margin:0 auto;max-width:60rem;"
    "padding:1rem;background:#fafafa;color:#222}"
    "h2{margin:.3rem 0}.sub{color:#666;font-size:.85rem;margin:.2rem 0 .8rem}"
    "h3{margin:1rem 0 .35rem;font-size:.95rem;border-bottom:1px solid #ddd;padding-bottom:.25rem}"
    ".row{display:flex;flex-wrap:wrap;gap:.5rem;align-items:center;margin:.4rem 0}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(16rem,1fr));gap:.2rem .8rem}"
    "canvas{max-width:100%;max-height:58vh;border:1px solid #999;background:#fff;"
    "display:block;margin:.5rem auto}"
    "button{padding:.42rem .9rem;font-size:.92rem;border:1px solid #bbb;border-radius:6px;"
    "background:#fff;cursor:pointer}"
    "button:disabled{opacity:.4;cursor:default}"
    "button.on{background:#2c7be5;color:#fff;border-color:#2c7be5}"
    "label{font-size:.88rem;color:#444}"
    "input[type=range]{vertical-align:middle;width:8.5rem}"
    ".v{font-variant-numeric:tabular-nums;color:#2c7be5;min-width:2.8rem;display:inline-block}"
    "#msg{white-space:pre-wrap;color:#555;margin-top:.6rem;font-size:.88rem;background:#fff;"
    "border:1px solid #e0e0e0;border-radius:6px;padding:.5rem}"
    "#sz{color:#666;font-size:.83rem}"
    "</style></head><body>"
    "<h2>InkWord BigScreen</h2>"
    "<div class='sub'>10.8 英寸 1920x1080 · 默认 1bit FS 抖动（预览即屏显）· 全屏 GC16 刷新约 3-4 秒</div>"
    "<div style='margin:8px 0'><a href='/wifi' style='font-size:14px'>⚙ WiFi 设置</a></div>"

    "<h3>1 · 选择图片</h3>"
    "<div class='row'><input type='file' id='f' accept='image/*'>"
    "<span id='dim' class='sub'></span></div>"

    "<h3>2 · 变换（旋转 / 镜像 / 适配）</h3>"
    "<div class='row'><label>旋转</label>"
    "<button class='rot on' data-v='0'>0°</button>"
    "<button class='rot' data-v='90'>90°</button>"
    "<button class='rot' data-v='180'>180°</button>"
    "<button class='rot' data-v='270'>270°</button>"
    "<button id='mh'>水平镜像</button><button id='mv'>垂直镜像</button></div>"
    "<div class='row'><label>适配</label>"
    "<button class='fit on' data-v='contain'>完整显示</button>"
    "<button class='fit' data-v='cover'>填满裁切</button>"
    "<button class='fit' data-v='stretch'>拉伸变形</button></div>"
    "<div class='grid'>"
    "<div class='row'><label>缩放 <span class='v' id='zoomv'>1.00</span></label>"
    "<input type='range' id='zoom' min='0.1' max='3' step='0.01' value='1'></div>"
    "<div class='row'><label>水平偏移 <span class='v' id='oxv'>0</span></label>"
    "<input type='range' id='ox' min='-960' max='960' step='1' value='0'></div>"
    "<div class='row'><label>垂直偏移 <span class='v' id='oyv'>0</span></label>"
    "<input type='range' id='oy' min='-540' max='540' step='1' value='0'></div>"
    "</div>"

    "<h3>3 · 灰阶处理</h3>"
    "<div class='grid'>"
    "<div class='row'><label>亮度 <span class='v' id='brv'>0</span></label>"
    "<input type='range' id='br' min='-100' max='100' step='1' value='0'></div>"
    "<div class='row'><label>对比度 <span class='v' id='ctv'>1.00</span></label>"
    "<input type='range' id='ct' min='0.3' max='3' step='0.01' value='1'></div>"
    "<div class='row'><label>Gamma <span class='v' id='gav'>1.00</span></label>"
    "<input type='range' id='ga' min='0.3' max='3' step='0.01' value='1'></div>"
    "<div class='row'><label>锐化 <span class='v' id='shv'>50</span></label>"
    "<input type='range' id='sh' min='0' max='200' step='5' value='50'></div>"
    "<div class='row'><label>反色</label><button id='neg'>关</button></div>"
    "</div>"

    "<h3>4 · 输出模式</h3>"
    "<div class='row'>"
    "<button class='mode' data-v='gray16'>16 级灰阶</button>"
    "<button class='mode' data-v='fs2'>2bit 抖动</button>"
    "<button class='mode on' data-v='fs'>1bit FS</button>"
    "<button class='mode' data-v='bayer'>1bit Bayer</button>"
    "<button class='mode' data-v='thr'>1bit 阈值</button>"
    "<label id='thrl'>阈值 <span class='v' id='thrv'>128</span></label>"
    "<input type='range' id='thr' min='1' max='254' step='1' value='128'></div>"
    "<div class='row'><span class='sub'>注：并行管线不执行波形相位时长，16 级灰阶直出中间灰塌缩为白；"
    "产品固件（APP 版）对灰阶/2bit 上传一律 FS 抖动呈现（2026-09-17 起，观感同 1bit FS），"
    "实验台固件配 /dither=off + /wf?wf=scanq 可物理灰阶；"
    "Bayer 纹理规整无拖尾，FS 蛇形扫描消方向条纹；照片建议锐化 50~150%</span></div>"

    "<h3>5 · 预览与上传</h3>"
    "<canvas id='cv' width='1920' height='1080'></canvas>"
    "<div class='row'><button id='up' disabled>上传并刷新</button>"
    "<button id='dl' disabled>下载处理后图片</button><span id='sz'></span></div>"

    "<h3>屏幕诊断</h3>"
    "<div class='row'><label>波形</label>"
    "<button id='wfb'>builtin</button><button id='wfs'>scanq</button>"
    "<button id='wff'>binfast</button><span id='wfi' class='sub'></span></div>"
    "<div class='row'><button id='wh'>全白</button><button id='bk'>全黑</button>"
    "<button id='pt'>测试图案</button><button id='gr'>16 级灰阶</button>"
    "<button id='pl'>反转极性</button><button id='st'>查询状态</button></div>"
    "<div id='msg'>就绪，请选择图片。</div>"

    "<script>"
    "const W=1920,H=1080,NP=W*H;"
    "const cv=document.getElementById('cv'),"
    "cx=cv.getContext('2d',{willReadFrequently:true}),"
    "msg=document.getElementById('msg'),up=document.getElementById('up'),"
    "sz=document.getElementById('sz'),dl=document.getElementById('dl');"
    "let img=null,rot=0,mirH=false,mirV=false,fit='contain',mode='fs',packed=null;"
    "const P={zoom:1,ox:0,oy:0,br:0,ct:1,ga:1,neg:false,thr:128,sh:50};"

    // [run111] 高质量降采样：大图一步 drawImage 到 1920x1080 会因浏览器低质
    //   插值 + 跳采样丢细节/生摩尔纹（上传图片发糊的客户端根因之一）。
    //   prep 缓存按 图片id+量化步长 键控，滑块拖动中复用；金字塔逐级减半
    //   降采样后再交付主画布变换（旋转/镜像/偏移仍走原路径）。
    "let imgSeq=0,prep=null,prepKey='';"
    "function makePrep(sx,sy){"
    " const k=imgSeq+'|'+(Math.round(sx*20)/20)+'|'+(Math.round(sy*20)/20);"
    " if(k===prepKey)return;"
    " let src=img,sw=img.width,sh=img.height;"
    " const tw=Math.max(1,Math.round(sw*sx)),th=Math.max(1,Math.round(sh*sy));"
    " while(sw>=tw*2&&sh>=th*2){"
    "  sw=Math.max(tw,Math.round(sw/2));sh=Math.max(th,Math.round(sh/2));"
    "  const c=document.createElement('canvas');c.width=sw;c.height=sh;"
    "  const cc=c.getContext('2d');cc.imageSmoothingEnabled=true;"
    "  cc.imageSmoothingQuality='high';cc.drawImage(src,0,0,sw,sh);src=c;"
    " }"
    " const c=document.createElement('canvas');c.width=tw;c.height=th;"
    " const cc=c.getContext('2d');cc.imageSmoothingEnabled=true;"
    " cc.imageSmoothingQuality='high';cc.drawImage(src,0,0,tw,th);"
    " prep=c;prepKey=k;"
    "}"

    // 仅做 canvas 变换绘制（不含像素处理），供滑块拖动实时反馈
    "function drawOnly(){"
    " cx.setTransform(1,0,0,1,0,0);"
    " cx.imageSmoothingEnabled=true;cx.imageSmoothingQuality='high';"
    " cx.fillStyle='#fff';cx.fillRect(0,0,W,H);"
    " if(!img)return false;"
    " const swap=(rot===90||rot===270);"
    " const iw=swap?img.height:img.width,ih=swap?img.width:img.height;"
    " let sx,sy;"
    " if(fit==='stretch'){sx=W/img.width;sy=H/img.height;}"
    " else{const s=fit==='contain'?Math.min(W/iw,H/ih):Math.max(W/iw,H/ih);sx=sy=s;}"
    " sx*=P.zoom;sy*=P.zoom;"
    " makePrep(sx,sy);"
    " cx.translate(W/2+P.ox,H/2+P.oy);"
    " cx.rotate(rot*Math.PI/180);"
    " cx.scale(sx*(mirH?-1:1),sy*(mirV?-1:1));"
    // prep 以原图尺寸为目的地绘制：缩放补偿取整误差，旋转/镜像几何不变
    " cx.drawImage(prep,-img.width/2,-img.height/2,img.width,img.height);"
    " cx.setTransform(1,0,0,1,0,0);"
    " return true;"
    "}"

    // 打包后反解回 canvas：预览即屏显效果（含灰阶量化与抖动）
    "function preview(){"
    " const im=cx.createImageData(W,H),o=im.data;"
    " for(let p=0;p<NP;p++){"
    "  let v;"
    "  if(mode==='gray16'||mode==='fs2'){const b=packed[p>>1],q=(p&1)?(b>>4):(b&15);v=q*17;}"
    "  else{v=(packed[p>>3]&(128>>(p&7)))?255:0;}"
    "  const i=p<<2;o[i]=v;o[i+1]=v;o[i+2]=v;o[i+3]=255;"
    " }"
    " cx.putImageData(im,0,0);"
    "}"

    // [run108] USM 锐化（3x3 box blur 近似）：v + amt*(v - blur)，预补偿
    //   墨水屏边界扩散；边界 clamp。另：Bayer 8x8 有序抖动矩阵（0..63）。
    "const B8=[0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,"
    "12,44,4,36,14,46,6,38,60,28,52,20,62,30,54,22,"
    "3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,"
    "15,47,7,39,13,45,5,37,63,31,55,23,61,29,53,21];"
    "function usm(g){"
    " const amt=P.sh/100,b=new Float32Array(NP);"
    " for(let y=0;y<H;y++){const ym=(y>0?y-1:0)*W,yp=(y<H-1?y+1:H-1)*W,yr=y*W;"
    "  for(let x=0;x<W;x++){const xm=x>0?x-1:0,xp=x<W-1?x+1:W-1;"
    "   b[yr+x]=(g[ym+xm]+g[ym+x]+g[ym+xp]+g[yr+xm]+g[yr+x]+g[yr+xp]"
    "    +g[yp+xm]+g[yp+x]+g[yp+xp])/9;}}"
    " for(let p=0;p<NP;p++){const v=g[p]+amt*(g[p]-b[p]);"
    "  g[p]=v<0?0:(v>255?255:v);}}"
    // 像素级处理：灰度 -> 亮度/对比度/Gamma/反色/USM -> 按模式打包
    "function process(){"
    " if(!drawOnly()){packed=null;up.disabled=true;dl.disabled=true;sz.textContent='';return;}"
    " const d=cx.getImageData(0,0,W,H).data,g=new Float32Array(NP);"
    " const ct=P.ct,br=P.br,ga=P.ga,neg=P.neg;"
    " for(let i=0,p=0;i<d.length;i+=4,p++){"
    "  let v=.299*d[i]+.587*d[i+1]+.114*d[i+2];"
    "  v=(v-128)*ct+128+br;"
    "  v=v<0?0:(v>255?255:v);"
    "  if(ga!==1)v=255*Math.pow(v/255,ga);"
    "  g[p]=neg?255-v:v;"
    " }"
    " if(P.sh>0)usm(g);"
    " if(mode==='gray16'||mode==='fs2'){"
    "  packed=new Uint8Array(NP/2);"
    "  if(mode==='gray16'){"
    "   for(let p=0;p<NP;p++){"
    "    const q=(g[p]*15/255+.5)|0;"
    "    if(p&1)packed[p>>1]|=q<<4;else packed[p>>1]|=q;"
    "   }"
    "  }else{"
    // 2bit：4 级 FS（量化步长 85，nibble=q*5），量化误差仅 1bit 的 1/3
    "   for(let y=0;y<H;y++)for(let x=0;x<W;x++){"
    "    const p=y*W+x,v=g[p];"
    "    const q=Math.max(0,Math.min(3,Math.round(v/85)));"
    "    if(p&1)packed[p>>1]|=q*5<<4;else packed[p>>1]|=q*5;"
    "    const er=v-q*85;"
    "    if(x+1<W)g[p+1]+=er*7/16;"
    "    if(y+1<H){if(x>0)g[p+W-1]+=er*3/16;g[p+W]+=er*5/16;"
    "     if(x+1<W)g[p+W+1]+=er/16;}"
    "   }"
    "  }"
    " }else{"
    "  packed=new Uint8Array(NP/8);"
    "  const th=P.thr;"
    "  if(mode==='thr'){"
    "   for(let p=0;p<NP;p++)if(g[p]>=th)packed[p>>3]|=128>>(p&7);"
    "  }else if(mode==='bayer'){"
    // Bayer 8x8：有序抖动无蠕虫/拖尾，阈值=(m+0.5)*4 均匀覆盖 0..255
    "   for(let y=0;y<H;y++)for(let x=0;x<W;x++){"
    "    const p=y*W+x;"
    "    if(g[p]>=(B8[(y&7)*8+(x&7)]+.5)*4)packed[p>>3]|=128>>(p&7);"
    "   }"
    "  }else{"
    // FS 蛇形扫描：奇数行右→左，误差核随扫描向镜像，消方向性条纹
    "   for(let y=0;y<H;y++){"
    "    const ltr=(y&1)===0,dx=ltr?1:-1;"
    "    for(let i=0;i<W;i++){"
    "     const x=ltr?i:W-1-i,p=y*W+x,v=g[p],b=v>=th?1:0;"
    "     if(b)packed[p>>3]|=128>>(p&7);"
    "     const er=v-(b?255:0);"
    "     if(ltr?x+1<W:x-1>=0)g[p+dx]+=er*7/16;"
    "     if(y+1<H){"
    "      if(ltr?x-1>=0:x+1<W)g[p-dx+W]+=er*3/16;"
    "      g[p+W]+=er*5/16;"
    "      if(ltr?x+1<W:x-1>=0)g[p+dx+W]+=er/16;}"
    "    }"
    "   }"
    "  }"
    " }"
    " preview();"
    " up.disabled=false;dl.disabled=false;"
    " sz.textContent='待上传 '+packed.length+' 字节（'"
    "  +(mode==='gray16'?'4bpp 16 级灰':mode==='fs2'?'4bpp 4 级 FS':'1bit 二值')+'）';"
    "}"

    // 像素处理较重（手机约数百 ms），滑块拖动只重绘变换，停止 220ms 后才打包
    "let pt=null;"
    "function render(){drawOnly();packed=null;up.disabled=true;dl.disabled=true;"
    " clearTimeout(pt);pt=setTimeout(process,220);}"

    "function bindSlider(id,key,dec){"
    " const el=document.getElementById(id),lb=document.getElementById(id+'v');"
    " el.oninput=()=>{P[key]=parseFloat(el.value);"
    "  lb.textContent=dec?P[key].toFixed(dec):(P[key]|0);"
    "  drawOnly();clearTimeout(pt);pt=setTimeout(process,220);};"
    " el.oninput();"
    "}"
    "bindSlider('zoom','zoom',2);bindSlider('ox','ox',0);bindSlider('oy','oy',0);"
    "bindSlider('br','br',0);bindSlider('ct','ct',2);bindSlider('ga','ga',2);bindSlider('sh','sh',0);"
    "document.getElementById('thr').oninput=e=>{"
    " P.thr=parseInt(e.target.value);document.getElementById('thrv').textContent=P.thr;"
    " if(mode==='fs'||mode==='thr')render();};"

    // 按钮组：旋转 / 适配 / 输出模式（互斥高亮）
    "function group(cls,cb){"
    " document.querySelectorAll('.'+cls).forEach(b=>b.onclick=()=>{"
    "  document.querySelectorAll('.'+cls).forEach(x=>x.classList.remove('on'));"
    "  b.classList.add('on');cb(b.dataset.v);});"
    "}"
    "group('rot',v=>{rot=parseInt(v);render();});"
    "group('fit',v=>{fit=v;render();});"
    "group('mode',v=>{mode=v;document.getElementById('thrl').style.opacity="
    "  (v==='gray16'||v==='fs2'||v==='bayer')?'.35':'1';render();});"
    "document.getElementById('mh').onclick=e=>{mirH=!mirH;"
    " e.target.classList.toggle('on',mirH);render();};"
    "document.getElementById('mv').onclick=e=>{mirV=!mirV;"
    " e.target.classList.toggle('on',mirV);render();};"
    "document.getElementById('neg').onclick=e=>{P.neg=!P.neg;"
    " e.target.textContent=P.neg?'开':'关';e.target.classList.toggle('on',P.neg);render();};"

    "document.getElementById('f').onchange=e=>{"
    " const file=e.target.files[0];if(!file)return;"
    " msg.textContent='读取图片中...';"
    " const im=new Image();"
    " im.onload=()=>{img=im;imgSeq++;prepKey='';"
    "  document.getElementById('dim').textContent=im.width+' x '+im.height+' · '+file.name;"
    // 竖图自动转 90°：手机照片多为竖向，横屏下 contain 会留大片白边
    "  if(im.height>im.width){rot=90;"
    "   document.querySelectorAll('.rot').forEach(x=>x.classList.toggle('on',"
    "    x.dataset.v==='90'));}"
    "  msg.textContent='图片已载入，可调整变换与灰阶后上传。';render();};"
    " im.onerror=()=>{msg.textContent='图片解码失败';};"
    " im.src=URL.createObjectURL(file);"
    "};"

    "up.onclick=async()=>{"
    " if(!packed)return;up.disabled=true;"
    " msg.textContent='上传中（'+packed.length+' 字节）...';const t=Date.now();"
    " try{"
    "  const r=await fetch('/upload',{method:'POST',"
    "   headers:{'Content-Type':'application/octet-stream'},body:packed});"
    "  msg.textContent='上传 HTTP '+r.status+' '+await r.text()"
    "   +'\\n总耗时 '+((Date.now()-t)/1000).toFixed(1)+'s';"
    " }catch(err){msg.textContent='上传失败: '+err;}"
    " up.disabled=false;"
    "};"

    // 导出处理后图片，便于在电脑上核对灰阶/抖动效果
    "dl.onclick=()=>{cv.toBlob(b=>{const a=document.createElement('a');"
    " a.href=URL.createObjectURL(b);a.download='inkword_preview.png';a.click();});};"

    "async function job(id,url,label){"
    " const b=document.getElementById(id);b.disabled=true;"
    " msg.textContent=label+'中（约 4s）...';const t=Date.now();"
    " try{const r=await fetch(url,{method:'POST'});"
    "  msg.textContent=label+' HTTP '+r.status+' '+await r.text()"
    "   +'\\n耗时 '+((Date.now()-t)/1000).toFixed(1)+'s';"
    " }catch(err){msg.textContent=label+'失败: '+err;}"
    " b.disabled=false;"
    "}"
    "document.getElementById('wh').onclick=()=>job('wh','/white','全白');"
    "document.getElementById('bk').onclick=()=>job('bk','/black','全黑');"
    "document.getElementById('pt').onclick=()=>job('pt','/pattern','测试图案');"
    // [run106] 16 带灰阶标板：builtin 波形下呈 §15 塌缩签名；配 /wf?wf=scanq
    //   （电脑端或浏览器地址栏访问）可拍照迭代 map 标定，免上传 raw。
    "document.getElementById('gr').onclick=()=>job('gr','/grayramp','灰阶标板');"
    // [run109] 波形三态切换：scanq 标灰阶（灰阶标板/2bit）、binfast 二值
    //   快速（1bit 图片更快更锐）；/wf 响应体带回全部波形参数。
    "async function setwf(u,label){"
    " try{const r=await fetch(u);const t=await r.text();"
    "  msg.textContent='波形→'+label+' HTTP '+r.status+' '+t;"
    "  try{document.getElementById('wfi').textContent='当前 '+JSON.parse(t).wf;}"
    "  catch(e){}}"
    " catch(err){msg.textContent='切换失败: '+err;}}"
    "document.getElementById('wfb').onclick=()=>setwf('/wf?wf=builtin','builtin');"
    "document.getElementById('wfs').onclick=()=>setwf('/wf?wf=scanq','scanq');"
    "document.getElementById('wff').onclick=()=>setwf('/wf?wf=binfast','binfast');"
    "fetch('/status').then(r=>r.json())"
    " .then(s=>{document.getElementById('wfi').textContent='当前 '+s.wf;}).catch(()=>{});"
    "document.getElementById('pl').onclick=async()=>{"
    " try{const r=await fetch('/pol');"
    "  msg.textContent='极性已切换 '+await r.text()+'，请再点全白/全黑对比';"
    " }catch(err){msg.textContent='切换失败: '+err;}};"
    "document.getElementById('st').onclick=async()=>{"
    " try{const r=await fetch('/status');msg.textContent='状态 '+await r.text();}"
    " catch(err){msg.textContent='查询失败: '+err;}};"
    "drawOnly();"
    "</script></body></html>";

#endif  // LAN_PAGE_H
