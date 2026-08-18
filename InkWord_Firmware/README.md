# InkWord Firmware

基于 ESP32-S3 的墨水屏英语单词学习终端固件，运行于 Arduino 框架 (espressif32@7.0.1) + GxEPD2 驱动栈。

## 硬件方案（2026-08 实测版）

| 组件 | 型号 / 规格 | 接口 |
|------|-------------|------|
| 主控 | ESP32-S3-DevKitC-1 (16MB Flash, 8MB PSRAM) | — |
| 屏幕 | DKE DEPG0370 3.7" 240×416 (UC8253 类 COG) + EVK011 升压转接板 | 4 线 SPI 直驱 (GxEPD2) |
| 音频 | MAX98357A 功放 | I2S（待接线验证） |
| 存储 | MicroSD 卡 | SPI + FAT（未接线） |
| 按键 | 五向导航开关（无源，上/下/左/右/中 + SET/RST 侧键） | GPIO 独立输入（已接入，2026-08 取代 6 键） |

> 详细接线图见 [`../docs/WIRING_DIAGRAM.md`](../docs/WIRING_DIAGRAM.md)。
> 升压：EVK011 板上分立 boost 由屏 COG 从 FPC pin2(GDR) 自主驱动，
> MCU 仅向 J2-16 供 3.3V，无 GDR/RESE 信号。

## 引脚映射

完整定义见 [`src/gpio_config.h`](src/gpio_config.h)。

| 外设 | 引脚 | 说明 |
|------|------|------|
| **墨水屏** (EVK011 J2, 9 线) | | 4 线 SPI, BS=GPIO11 固件驱动 LOW (绝不可悬空) |
| — SCK | GPIO7 → J2-3 | SPI 时钟 |
| — SDO | GPIO8 → J2-5 | SPI 数据 (MCU 侧 MOSI，转接板丝印 SDO) |
| — D/C# | GPIO9 → J2-7 | 命令/数据 |
| — CS | GPIO10 → J2-6 | 片选 |
| — BS | GPIO11 → J2-10 | 接口模式选择, 固件驱动 LOW=4 线 SPI (备选方案: 板侧短接 GND + `EPD_BS_PIN=-1`) |
| — RST | GPIO13 → J2-8 | 硬复位 (深睡唯一唤醒途径) |
| — BUSY | GPIO12 → J2-9 | 忙信号 (LOW=忙) |
| — VCI/GND | 3V3 → J2-16 / GND → J2-1 | 供电与共地 (必须!) |
| **I2S 音频** | | 标准飞利浦 I2S |
| — BCLK | GPIO4 | 位时钟 |
| — LRCK | GPIO5 | 字选择 |
| — DOUT | GPIO6 | 数据输出 |
| **五向导航开关** | | 无源开关，上拉输入，COM 接地，五向全 RTC 域可深睡 |
| — UP | GPIO1 | 上一条 / 长按清残影全刷 |
| — DOWN | GPIO2 | 下一条 / 长按切换学习模式 |
| — LEFT | GPIO14 | 预留（配置页光标左移/返回）/ 长按 AP 门户、密码快删 |
| — RIGHT | GPIO15 | 预留（配置页光标右移）/ 长按 LAN 接收页 |
| — CENTER | GPIO21 | 发音（配置页确认/输入；待机页拉天气）/ 长按进入 Wi-Fi 配置、返回列表 |
| — SET | GPIO41 | 遮蔽/揭晓释义（待机页：轮换下一条引文）/ 长按预留 SRS「记得」 |
| — RST | GPIO42 | 回到当前模式第一条 / 长按预留 SRS「忘了」 |
| **SD 卡** (SPI3_HOST) | | 独立于 EPD 的 SPI 总线 |
| — MOSI | GPIO17 | |
| — MISO | GPIO16 | |
| — SCLK | GPIO18 | |
| — CS | GPIO47 | |

> ⚠️ SD 卡引脚勿与墨水屏混淆：GPIO10/11/12/13 全部为 EPD 占用
> (CS/BS/BUSY/RST)，SD 必须用 GPIO16/17/18/47。

## 软件架构

固件采用模块化设计，每个模块对应一个 Task (F-xx)，源码位于 [`src/`](src/)。

### 模块总览

| 模块 | 文件 | 职责 |
|------|------|------|
| **主入口** | [`main.cpp`](src/main.cpp) | 启动流程编排、按键路由、单词卡片 UI 渲染（局刷/全刷策略）、后台心跳/OTA任务 |
| **日志** | [`debug_log`](src/debug_log.h) | 统一 LOG_I / LOG_W / LOG_E / LOG_D 宏封装 |
| **屏幕驱动** | [`epd_driver`](src/epd_driver.h) + [`GxEPD2_374_DEPG0370`](src/GxEPD2_374_DEPG0370.h) | GxEPD2 自有面板类: 全刷/局刷/清屏/深睡; C 薄适配层 epd_gfx_*; 双坐标体系（底层竖屏 240×416 直通 / GFX 层横屏 416×240） |
| **音频** | [`audio_player`](src/audio_player.h) | I2S + MAX98357A, 44.1kHz/16bit, WAV/MP3 播放 |
| **按键** | [`button_handler`](src/button_handler.h) | 五向导航开关轮询去抖, 区分短按 / 长按 (1.5s) |
| **存储** | [`storage_manager`](src/storage_manager.h) | SD 卡 SPI 挂载至 `/sdcard`, 文件读写 |
| **刷新调度** | [`refresh_scheduler`](src/refresh_scheduler.h) | 局刷计数, 达阈值自动全刷清残影（学习页阈值 8；待机页已改全局刷新不再使用） |
| **词库** | [`word_parser`](src/word_parser.h) | 解析 `words.json` 词库至内存 |
| **SRS 引擎** | [`srs_engine`](src/srs_engine.h) | SM-2 间隔重复算法 (纯算法, 可单测) |
| **模式状态机** | [`study_mode_machine`](src/study_mode_machine.h) | 闪卡 / 听写 / 复习三模式切换 |
| **Wi-Fi 联网** | [`wifi_manager`](src/wifi_manager.h) | 网络栈/STA 连接、NVS 凭据持久化、SoftAP、AP 扫描、快速+慢速断线重连、异步连接 |
| **HTTP 同步** | [`sync_client`](src/sync_client.h) | 增量词库拉取、学习记录回传、心跳上报、天气拉取（附带校时） |
| **OTA** | [`ota_manager`](src/ota_manager.h) | 双分区升级: 下载 / 校验 / 切换 / 回滚 |
| **Wi-Fi 配置 UI** | [`wifi_config_ui`](src/wifi_config_ui.h) | 扫描列表 + QWERTY 软键盘配网向导（长按 C） |
| **LAN 直传/配网门户** | [`lan_display_server`](src/lan_display_server.h) | 设备端 HTTP 服务器 + 内嵌发送页 + Wi-Fi 配网页 + mDNS + SoftAP captive portal + DNS 劫持 |
| **待机页** | [`standby_page`](src/standby_page.h) + [`cjk_font`](src/cjk_font.h) | 无词库时的《传习录》引文整页（引文独占：居中楷体 Bold 24px 点阵每 5 分钟轮换 + 右下角出处；HTTP Date+后端双校时、NVS 天气缓存；轮换即全局刷新防残影） |

### 启动流程

```
setup() (Arduino)
  │
  ├─ 1. 日志 + NVS 初始化
  ├─ 2. SD 卡挂载 + 屏幕初始化 + 音频 + 按键
  ├─ 3. 刷新调度器 (局刷阈值=8)
  ├─ 4. Wi-Fi 联网 (尝试已保存凭据, 关闭 Modem-Sleep)
  ├─ 4.5 Wi-Fi 配置 UI 初始化 ── 无凭据时自动开启 AP 配网门户 (captive portal)
  ├─ 5. 标记固件有效 (防 OTA 回滚)
  ├─ 6. 加载词库 (/sdcard/words.json) + 待机页初始化 (NVS 天气缓存恢复)
  ├─ 7. 有词库: 进入上次学习模式 / 无词库: 渲染待机页 (《传习录》引文)
  └─ 8. 启动后台任务 (心跳 + OTA 检查 + 天气轮询, 每 10 分钟)
```

主任务退出后，系统由**按键回调**与**后台任务**事件驱动。

## 核心功能

### 学习模式

三种模式循环切换 (D 键切换)，模式持久化到 NVS:

| 模式 | 说明 |
|------|------|
| **闪卡** (FLASH) | 看词猜义，C 键发音 |
| **听写** (DICTATION) | 听音拼写 |
| **复习** (REVIEW) | SRS 到期词复习 |

### SM-2 间隔重复算法

```
回忆质量 q (0~5) → 更新 EaseFactor → 计算下次复习间隔
  q < 3:  重置间隔为 1 天 (重新学习)
  q >= 3: 1天 → 6天 → 6×EF天 → … (间隔逐步增长)
```

### 防残影刷新调度

```
局刷计数 < 8:  GxEPD2 displayWindow 差分局刷 (仅内容区, ~350ms, 无闪烁)
局刷计数 >= 8: 强制全屏刷新清残影 → 计数归零
GFX 局刷路径 (epd_gfx_flush_window): 先调 refresh_gfx_before_partial()
前置检查，返回 true 时需整屏重绘后全刷（待机页已改全局刷新，不经过此路径）
```

### 无词库待机页（《传习录》引文独占）

词库为空（SD 卡无 `words.json`）时，设备默认显示整页待机界面而非留白。
引文独占构图（用户 2026-08-18 定稿：仅显示《传习录》，星期/日期/农历/
月年/时间均不显示）：

```
     知是行之始，

     行是知之成。           ← 引文块 [112,8,192x176)：楷体 Bold 24px
                                点阵 8 字/行 x 5 行，行距 8px（行高 32，
                                松排版），水平居中+带内垂直居中，
                                每 5 分钟轮换一条（24 条循环）

              ——王阳明《传习录》  ← 出处 [192,216)：右下角右对齐（静态）
```

**《传习录》引文（中文子集点阵，~11.7KB flash）：**

- 24 条经典选句每 5 分钟轮换一条（知行合一、四句教、岩中花树等），一轮 2 小时
- 引文与字形由 [`tools/gen_cjk_font.swift`](tools/gen_cjk_font.swift) 从
  [`tools/chuanxilu_quotes.txt`](tools/chuanxilu_quotes.txt) 生成
  （macOS CoreText 渲染 Kaiti SC Bold 22pt -> 24x24 1bpp，163 字形，
  含出处串字符；两遍法实测墨迹盒自适应：Pass1 大画布逐字实测基线上/下
  与左右墨迹极值，Pass2 据此居中定基线，装不下自动缩字号；
  验收硬指标：全部字形 72 字节完整 + 四边 edge-touch=0），
  重生成：`cd InkWord_Firmware && swift tools/gen_cjk_font.swift`
- 排版约束在生成侧校验：每行 ≤8 字（含标点）、每条 ≤5 行、行首无标点

**时间源（双通道，分钟 tick 纯本地零网络）：**

- 应用层自治时钟：esp_timer 单调钟换算 Unix 秒（系统 time()/settimeofday 在本机损坏，弃用；SNTP 因运营商劫持 UDP 123 亦弃用）
- HTTP Date 头（主）：联网后请求 `generate_204` 探测页解析 `Date` 头校时（未同步 30s 重试 / 成功后 6h 校准）
- 后端校时兜底：天气响应携带 `serverTime`，偏差 >60s 才重置基准
- epoch 落在 [2025,2100] 之外视为时间无效，引文留白（仅出处；避免冷启动误导与畸形时间）

**天气数据链路（当前页面不显示，保留备用）：**

| 项 | 值 |
|----|-----|
| 端点 | `GET /api/device/weather`（需 `X-Device-Key` 头，同其他设备端点） |
| 响应 | `{"code":0,"message":"..","data":{"icon":2,"tempC":23,"desc":"Partly Cloudy","serverTime":1755321600,"tzOffsetMin":480}}` |
| icon | 0~7：晴/间晴/阴/雾/雨/阵雨/雪/雷暴（后端完成 WMO code 映射，顺序对应 `weather_icons.h` 勿改） |
| 轮询 | 后台任务每 ~30 分钟自动拉取；短按中立即拉取（阻塞 ≤10s） |
| 缓存 | NVS 持久化（3 小时内有效），重启即有画面 |

**刷新策略（2026-08-18 用户定稿：每 5 分钟轮换，一律全局刷新）：**

- 引文下标 = epoch / `STANDBY_QUOTE_INTERVAL_S`（默认 300s，可用
  `-DSTANDBY_QUOTE_INTERVAL_S=600` 等覆盖）% 24，无状态派生，
  重启/校时自然对齐同一窗口；变化即整页全局刷新（一天 288 次）
- 全刷走 GxEPD2 `display(false)` 全刷模式（写 previous 缓冲，
  自带残影清理）；局刷路径真机实测显示异常且有残影，已弃用
- 清屏采用黑白交替深清（`epd_clear_screen`）：先全黑全刷再回白，
  洗掉长时间驻留的陈年黑迹（仅白帧全刷翻转不彻底会留浅影）
- 长按上手动清残影（黑白交替深清 + 整页重绘，与学习页同语义）

天气图标（8 个 40×40 单色位图，~1.6KB flash）与图标绘制当前未上屏，
由 [`tools/gen_weather_icons.py`](tools/gen_weather_icons.py) 生成可随时复用。

### Wi-Fi 配置 UI

设备提供**三种配网/换网方式**，任选其一：

| 方式 | 入口 | 适用场景 |
|------|------|----------|
| **AP 配网门户**（推荐） | 开机无凭据自动开启；或长按 E 键 | 首次配网 / 换网，手机自动弹页体验最佳 |
| **网页配网** | 发送页右上角“Wi-Fi 设置” | 已联网状态下直接换网，无需重启 |
| **软键盘配网** | 长按 C 键 | 无第二台设备时的屏上向导（见下文） |

**AP 配网门户工作原理：**

```
设备开启热点 InkWord-Setup (开放)
  + DNS 劫持 (53/UDP 所有 A 查询应答 192.168.4.1)
  + HTTP catch-all 302 重定向到 /wifi
       ↓
手机连接热点 → 系统联网检测 (connectivitycheck 等)
  → 域名被劫持到设备 → 自动弹出配网页
       ↓
选择网络 (扫描列表) + 输入密码 → 异步连接 (不阻塞 HTTP)
       ↓
状态轮询 → 成功后屏显设备 IP → 自动关热点回 STA
```

**软键盘配网 UI（长按 C）：**

设备首次使用或需要更换网络时，通过屏幕引导完成 Wi-Fi 配网。

**进入方式：**
- 运行中长按 C 键手动进入（开机无凭据时已改为自动开启 AP 配网门户）

**交互流程：**

```
列表页 (扫描附近 AP)
  ├─ A/B: 上下选择网络
  ├─ C:   确认 → 进入密码输入页
  ├─ D:   短按/长按退出配置
  │
  └─→ 密码页 (QWERTY 软键盘)
       ├─ A/B: 键盘上下移动
       ├─ E/F: 键盘左右移动
       ├─ C:   输入字符 / 触发"OK"连接
       ├─ D:   短按删除字符 / 长按返回列表
       │
       └─→ 连接 → 结果页 → 自动返回学习模式
```

**软键盘三态：** 小写字母 / 大写字母 (Shift) / 数字符号 (123)

**线程模型：** 独立 `wifi_ui_task` 处理所有 UI 逻辑 (扫描/连接/全刷)，按键事件通过 FreeRTOS 队列非阻塞转发，不影响 20ms 周期的按键去抖扫描。

### OTA 双分区升级

```
检查更新 → 下载固件 → 写入备用 OTA 分区 → MD5 校验 → 切换启动分区
                                  ↓ 校验失败
                              放弃升级，保持当前分区
启动后标记有效 → 防止异常回滚
```

## 墨水屏网页直传（LAN Display）

手机/电脑浏览器将**文本或图片**发送到墨水屏显示，无需后端服务器。

### 三种使用通道

| 通道 | 进入方式 | 访问地址 | 适用场景 |
|------|----------|----------|----------|
| **STA 同网直传** | 长按 F 键 | `http://<设备IP>/` 或 `http://inkword.local/` | 手机与设备同一路由器（正常家庭网络） |
| **AP 直连** | 长按 E 键 | 手机连 `InkWord-Setup` 热点 → `http://192.168.4.1/` | 路由器开了 AP 隔离 / 无路由器环境，完全不依赖路由器 |
| **配网门户附带** | 开机无凭据自动 | 同 AP 直连，配网成功后即可顺手发图 | 首次使用 |

服务启动时机：联网后 2 秒内自动启动（后台任务每 2s 轮询，无窗口上限）；
STA 模式同时注册 mDNS（`inkword.local`，iOS/macOS 支持佳，Android 建议用 IP）。

### 发送页功能（内嵌单文件网页，零外部依赖）

- **文本模式**：多行输入 + 字号选择（16/24/32/48px），中文由手机浏览器字体渲染
- **图片模式**：上传任意格式图片，等比缩放适配屏幕
- **旋转选择**（文本/图片共用）：自动（横图转横屏）/ 0° / 90° / 180° / 270°
  - 选 90° 后文本在 416×240 横屏视口内重新断行排版并垂直居中，横持设备正立满幅观看
- **双预览**：竖屏缓冲预览（240×416）+ 设备横屏视角预览（416×240），所见即所得
- **抖动开关**：Floyd-Steinberg 误差扩散，模拟灰度层次（默认开）
- **反色开关**：白字黑底

### 技术架构：浏览器端转码 + 设备薄服务

```
手机浏览器                          ESP32-S3
──────────                          ─────────
Canvas 渲染 (系统字体/图片缩放)
  → Floyd-Steinberg 抖动
  → 打包竖屏 1bpp 帧 (12480B,
    行宽 30B, MSB first, bit=1 白)
  → POST /api/display    ──────→  校验长度 → 整帧直刷 epd_full_refresh()
                                    + 残影计数归零
                                    + 学习页下次强制全刷
```

固件零解码逻辑（无 JPEG/PNG 库、无 CJK 字形依赖）；转码全部在浏览器完成，
上传的即最终像素。GET 统一经 `/*` 通配路由分发（`uri_match_fn=
httpd_uri_match_wildcard`），POST 精确注册；captive portal 探测域名 302 到配网页。

### HTTP API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 发送页（内嵌 HTML） |
| GET | `/wifi` | Wi-Fi 配网页（扫描/连接/状态轮询） |
| POST | `/api/display` | 整帧 1bpp 位图（Content-Length 必须 = 12480） |
| POST | `/api/wifi/connect` | 异步连接 `{"ssid":..,"pass":..}` |
| GET | `/api/wifi/scan` | AP 扫描列表 JSON |
| GET | `/api/wifi/status` | 连接状态 JSON（idle/connecting/ok/fail + IP） |

### 故障排查：页面连不上

1. **服务是否启动**：长按右看屏幕是否显示 URL（联网后 2s 内自动启动，无需长按）
2. **mDNS 通但 ping/TCP 不通**（毫秒级 `No route to host`）：路由器开了 **AP 隔离**
   （拦客户端间单播，放行组播/广播）→ 关闭路由器隔离，或改用**长按左 AP 直连**
3. **IP 变了**：路由器 DHCP 重新分配 → 长按右重看屏幕上的最新 IP
4. **路由器重启后设备失联**：旧固件缺陷已修复（5 次快速重连失败后每 30s 慢速重连）

## 按键操作速查（五向导航开关）

### 待机页（词库为空时）

| 键 | 短按 | 长按 |
|----|------|------|
| 上 | 忽略 | 清残影全刷 |
| 下 | 忽略 | 忽略 |
| 中 | 立即拉取天气 | 进入 Wi-Fi 配置 |
| 左 | 忽略 | 进入 AP 直连/配网门户 |
| 右 | 忽略 | 进入 LAN 接收页 |
| SET | 轮换下一条引文 | 预留 SRS「记得」 |
| RST | 忽略 | 预留 SRS「忘了」 |

> 无词库无可翻内容，短按忽略以节省刷新次数；长按语义与学习页一致，
> 任意键退出配网/LAN 页后待机页自动整页重绘。

### 学习模式 (默认)

| 键 | 短按 | 长按 |
|----|------|------|
| 上 | 上一条 | 清残影全刷 |
| 下 | 下一条 | 切换学习模式 |
| 中 | 发音 | 进入 Wi-Fi 软键盘配置 |
| 左 | 预留（释义滚动扩展） | 进入 AP 直连/配网门户（手机连 InkWord-Setup 直传） |
| 右 | 预留（释义滚动扩展） | 进入 LAN 接收页（同网浏览器直传） |
| SET | 遮蔽/揭晓释义（闪卡自测，再按切换） | 预留 SRS「记得」评分 |
| RST | 回到当前模式第一条 | 预留 SRS「忘了」评分 |

> 释义遮蔽态右栏显示 `[SET] to reveal` 提示；翻页/切模式后自动回全显。
> SET/RST 长按为 SM-2 质量分（记得=q4 / 忘了=q0）预留位，
> SRS 闭环接入学习记录上报后启用。

> 接收页 / portal 激活期间，**任意按键**退出并回到学习界面（portal 模式同时关热点回 STA）。
> 接收页与配网期间学习页渲染自动屏蔽，直刷后会强制下次全刷，无残影/花屏风险。

### Wi-Fi 配置 UI

| 键 | 列表页 | 密码页 |
|----|--------|--------|
| 上 | 上移选中 | 键盘上移 |
| 下 | 下移选中 | 键盘下移 |
| 左 | 退出 | 键盘左移 / 长按快删字符 |
| 右 | — | 键盘右移 |
| 中 | 确认 / 重新扫描 | 输入字符 / 连接 / 长按返回列表 |

## 目录结构

```
InkWord_Firmware/
├── src/
│   ├── main.cpp                # 主入口 (Arduino setup/loop)
│   ├── debug_log.{c,h}         # 日志封装
│   ├── gpio_config.h           # 全局引脚映射
│   ├── GxEPD2_374_DEPG0370.{cpp,h} # GxEPD2 面板类 (DEPG0370 专用初始化序列)
│   ├── Fonts/Arial14pt7b.h    # 字体 (fontconvert 生成)
│   ├── epd_driver.{cpp,h}      # 墨水屏驱动适配层 (epd_gfx_* C API)
│   ├── audio_player.{c,h}      # I2S 音频
│   ├── button_handler.{c,h}    # 按键扫描去抖
│   ├── storage_manager.{c,h}   # SD 卡存储
│   ├── refresh_scheduler.{c,h} # 刷新调度
│   ├── word_parser.{c,h}       # 词库解析
│   ├── srs_engine.{c,h}        # SM-2 算法
│   ├── study_mode_machine.{c,h}# 学习模式状态机
│   ├── wifi_manager.{c,h}      # Wi-Fi 联网
│   ├── wifi_config_ui.{c,h}    # Wi-Fi 配置 UI（软键盘）
│   ├── lan_display_server.{cpp,h} # LAN 直传/配网门户（HTTP 服务+内嵌网页+mDNS+DNS 劫持）
│   ├── sync_client.{c,h}       # HTTP 同步（含天气拉取/校时）
│   ├── ota_manager.{c,h}       # OTA 升级
│   ├── standby_page.{c,h}      # 无词库待机页（《传习录》引文+出处）
│   ├── lunar_calendar.{c,h}    # 农历查表（swift NSCalendar 离线生成，2025~2035）
│   ├── weather_icons.h         # 40×40 天气图标位图（脚本生成）
│   ├── CMakeLists.txt          # ESP-IDF 组件注册
│   └── idf_component.yml       # ESP-IDF 组件依赖清单
├── tools/
│   └── gen_weather_icons.py    # 天气图标位图生成脚本（可复现）
├── partitions_default.csv      # 分区表 (OTA 双分区)
├── platformio.ini              # PlatformIO 配置
├── .gitignore
└── README.md
```

## 分区表

支持 OTA 双 app 分区方案:

| 分区 | 类型 | 偏移 | 大小 |
|------|------|------|------|
| nvs | data | 0x9000 | 24 KB |
| phy_init | data | 0xF000 | 4 KB |
| factory | app | 0x10000 | 3 MB |
| ota_0 | app | — | 3 MB |
| ota_1 | app | — | 3 MB |
| otadata | data | — | 8 KB |
| nvs_keys | data | — | 4 KB |
| spiffs | data | — | 1 MB |

## 构建与烧录

### 环境要求

- PlatformIO Core（macOS 需 Homebrew Python 3.11+）
- espressif32@7.0.1 平台包（PlatformIO 自动管理，Arduino 框架）

### 编译与上传

```bash
cd InkWord_Firmware

# 编译（必须用 Python 3.11，系统默认 pio 跑在旧 Python 上会报版本错）
/opt/homebrew/bin/python3.11 -m platformio run -e inkword-s3

# 编译并烧录 (USB, CP2102 通常为 /dev/cu.usbserial-0001)
/opt/homebrew/bin/python3.11 -m platformio run -e inkword-s3 -t upload

# 串口监控 (115200; 烧录前需先退出，否则串口占用报 port is busy)
/opt/homebrew/bin/python3.11 -m platformio device monitor --port /dev/cu.usbserial-0001 --baud 115200
```

> 注：Arduino 预编译库 CONFIG_LOG_MAXIMUM_LEVEL=ERROR，串口只能看到
> E 级日志与 Serial.printf 输出，LOG_I/LOG_W 在编译期被裁剪。

### 依赖组件

| 依赖 | 来源 | 用途 |
|------|------|------|
| `GxEPD2 @ 1.6.9` | PlatformIO Library Registry | 墨水屏驱动基类 |
| `Adafruit GFX` @ 1.12.6 | PlatformIO Library Registry | 绘图栈/字体渲染 |
| `esp_wifi` | ESP-IDF 内置 | Wi-Fi STA + SoftAP (APSTA) |
| `esp_http_server` | ESP-IDF 内置 | 设备端 HTTP 服务器（发送页/配网/直传） |
| `mdns` | ESP-IDF 内置 | `inkword.local` 域名广播 |
| `lwip` sockets | ESP-IDF 内置 | DNS 劫持 (53/UDP) |
| `esp_https_ota` | ESP-IDF 内置 | OTA 升级 |
| `fatfs` / `spiffs` | ESP-IDF 内置 | 文件系统 |
| `json` (cJSON) | ESP-IDF 内置 | 词库解析 |

### SD 卡词库格式

将 `words.json` 放置于 SD 卡根目录，格式示例:

```json
[
  {
    "text": "abandon",
    "phonetic": "/əˈbændən/",
    "meaning": "vt. 放弃; 抛弃",
    "example": "He abandoned his car.",
    "audio": "abandon.mp3",
    "tag": "CET4",
    "difficulty": 3,
    "id": 1
  }
]
```

音频文件放置于 `/sdcard/audio/` 目录。

## 配置

关键配置位于 [`platformio.ini`](platformio.ini)（板型 esp32-s3-devkitc-1、
环境 inkword-s3）与 [`sdkconfig.inkword-s3`](sdkconfig.inkword-s3)。
屏幕初始化序列在 [`GxEPD2_374_DEPG0370.cpp`](src/GxEPD2_374_DEPG0370.cpp)
（PSR=0xDF、全刷 CDI=0x97、局刷 CDI=0x17，对照官方 demo 修正）。

日志级别可通过 `menuconfig` → Component config → Log output 调整，或运行时调用 `log_set_global_level()`。

## License

Proprietary — LexInk
