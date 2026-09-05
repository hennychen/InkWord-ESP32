# 墨读 / InkRead 多屏兼容方案设计文档

## 一、文档信息

| 项目 | 内容 |
|:---|:---|
| 产品名称 | 墨读 / InkRead |
| 文档版本 | V1.0（首版设计） |
| 更新日期 | 2026-08-22 |
| 状态 | 已评审 —— 全部代码级证据经三人交叉验证（架构 / 性能 / 风险三视角） |
| 关联文档 | [PRD_V2.1.md](PRD_V2.1.md) / [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) |
| 涉及代码 | [InkWord_Firmware/src/](../InkWord_Firmware/src/) |

> **行号引用约定**：本文档大量引用 `文件 L行号` 作为代码级证据。行号基于
> 2026-08-22 当前代码基线，代码迭代后行号可能漂移——每条证据均附宏名 /
> 符号名，漂移时以符号搜索为准（如全局搜 `SB_QUOTE_X0`）。
> 本项目既有文档（PRD / WIRING）不标行号，本文档为设计文档新增此体例。
>
> **实现演进勘注（2026-09-03，代码为权威）**：P1 收官与架构优化系列落地后，
> 本文若干机制描述与现状存在已勘误偏差，正文保留设计原貌：
> - 「选择机制编译期 INKWORD_PANEL_ID」：实现三段演进——Phase 3 落地为
>   INKWORD_PANEL_* 宏链 → 2026-09-03 裁剪，面板 env 直接注入
>   `-D EPD_PANEL_DEFAULT_ID="注册名"`（epd_panel.h 仅 #ifndef 兜底）；
> - 「量产预留 NVS 运行期选屏」：P1 收官（2026-09-02）已落地——NVS
>   `set_panel` 键运行期覆盖默认 ID（settings_keys.h 键表 / epd_driver_init）；
> - 新屏接入三步中的「epd_panel.h 加条件分支」：已取消，现仅需 env 注入
>   DEFAULT_ID（固件 README 新屏 SOP 已同步）；
> - 模块布局：main.cpp 已拆分（word_card_ui 渲染族 / sync_session 云端编排）；
>   NVS 键集中 settings_keys.h；epd_bus 新增族标准电源序列
>   （bus_ssd16_* / bus_uc_*，byte 级一致者收敛）。
>
> **备份纪律（T2.4 起，修 E1）**：历史 .bak 存量已全库清零（git 从未跟踪
> 过 .bak；.gitignore `*.bak*` 防增量）。现行纪律为「commit 即备份」：
> 改动前不再留 .bak 副本，验证通过即分阶段 commit，回退走 git 历史。
> 2026-08-22 基线的 .bak 文件已不存在，行号证据仅以符号名为准。

---

## 二、背景与现状盘点

### 2.1 硬件载体切换

当前固件将 3.7" DEPG0370（240x416 BW）的几何、时序、色彩能力**编译期定死**在
驱动与 UI 各层。硬件侧正在从 **EVK011-C 转接板**（24P FPC 锁定 3.7" 一款屏）切换到
**v1.4 通用驱动板**（24P / 26P / 34P FPC 三合一座子、板上自主升压解耦 COG 时序、
双 CS、可选采样电阻 0.47R / 3R），客观上要求固件从"单屏工程"演进为"多屏工程"。

### 2.2 新增资料（Info/ink_test，2026-08-22 分析）

卖家提供的 GxEPD2 试屏工具（GxEPD2_Example 改造，含 ESP32 / C3 / S3 / 8266 接线
预设）。当前激活行为 2.13" GDEH0213B72（128x250，SSD1675A，见
[ink_test.ino](../Info/ink_test/ink_test.ino) L58），[Readme.txt](../Info/ink_test/Readme.txt)
L30 明示「不知型号就逐个试驱动」——该"试屏法"方法论被吸收进本文档的 probe 环境
设计（§14.3）。

### 2.3 耦合点全量证据表（17 条）

> 本表是全文的锚：后续各章的论断均可回溯到此表的行号证据。`层` 列为目标架构
> （§四）中该耦合的归属层。

| # | 文件 | 行号 | 证据摘录 | 层 |
|:--|:-----|:-----|:---------|:--|
| 1 | [epd_driver.h](../InkWord_Firmware/src/epd_driver.h) | L27-31 | `EPD_WIDTH(240)/EPD_HEIGHT(416)/EPD_GFX_WIDTH(416)/EPD_GFX_HEIGHT(240)/EPD_FB_SIZE` 五个编译期宏 | L3 |
| 2 | [epd_driver.h](../InkWord_Firmware/src/epd_driver.h) | L125-126 | `EPD_GFX_BLACK=1 / EPD_GFX_WHITE=0`——颜色语义硬编码 1bpp | L3 |
| 3 | [epd_driver.cpp](../InkWord_Firmware/src/epd_driver.cpp) | L44-45 | `static GxEPD2_374_DEPG0370 s_epd2(...)` 静态面板实例，编译期绑死单屏 | L0 实例化点 |
| 4 | [epd_driver.cpp](../InkWord_Firmware/src/epd_driver.cpp) | L48, L53-54 | 堆分配 `GFXcanvas1(416x240)` + 静态双帧 `s_port_new/s_port_prev[EPD_FB_SIZE]`（12,480B x2） | L3 帧缓冲动态化对象 |
| 5 | [epd_driver.cpp](../InkWord_Firmware/src/epd_driver.cpp) | L83-98 | `canvas_to_panel` 转置硬编码 rotation=1：L86 stride=52、L92 `(EPD_GFX_HEIGHT-1)-cy`（即 239-cy 魔数） | L3 转置泛化点 |
| 6 | [epd_driver.cpp](../InkWord_Firmware/src/epd_driver.cpp) | L103-119 | `gfx_rect_to_panel` 窗口旋转：L116 `rx = EPD_GFX_HEIGHT-rx-rw` | L3 同上 |
| 7 | [epd_driver.cpp](../InkWord_Firmware/src/epd_driver.cpp) | L60-73 | Latin 字体表仅 FreeSans/Arial 9-24pt 四档（CJK 走 cjk_font 旁路） | L4 字体档位 |
| 8 | [epd_driver.cpp](../InkWord_Firmware/src/epd_driver.cpp) | L393-396, L422-425 | `hwReset/initFullDemo/demoWriteFull/demoWriteDualNoWindow/updateDemoPartial` 面板专属 demo 序列直调 | L0→L2 ops 函数指针化 |
| 9 | [standby_page.c](../InkWord_Firmware/src/standby_page.c) | L91-101 | `SB_QUOTE_X0=112 / SB_QUOTE_Y0=8 / SB_QUOTE_W=192 / SB_QUOTE_H=176 / SB_ATTR_Y0=192 / SB_ATTR_X1=392` 绝对坐标（注释直书 `(416-192)/2`） | L5→L4 |
| 10 | [wifi_config_ui.c](../InkWord_Firmware/src/wifi_config_ui.c) | L82-83 | `SCR_W=epd_gfx_width() / SCR_H=epd_gfx_height()`——**全工程最佳动态获取范式**，推广基准 | L4 推荐范式 |
| 11 | [wifi_config_ui.c](../InkWord_Firmware/src/wifi_config_ui.c) | L54-79 | 键盘几何全硬编码 416 宽：`TITLE_H=30 / BOTTOM_LINE_Y=220 / KB_START_Y=74 / KB_KEY_W=36 / kb_w_r2[] / kb_w_r3[] / PWD_SHOW_MAX` 等 | L5→L4 |
| 12 | [main.cpp](../InkWord_Firmware/src/main.cpp) | L127-144 | 学习页 17 个 `UI_` 布局常量：`UI_STATUS_H=32 / UI_VSEP_X=248 / UI_MEAN_X=264 / UI_WORD_BASE=100 / UI_FOOT_BASE=224` 等 | L5→L4 **最大改造点** |
| 13 | [reader_engine.c](../InkWord_Firmware/src/reader_engine.c) | L31-38 | `R_AREA_Y=32 / R_MARGIN_X=8 / R_MAX_W=(EPD_GFX_WIDTH-16) / R_MAX_H=(EPD_GFX_HEIGHT-36)` 宏派生半动态（较良好，仅 L386 有 `-80` 魔数） | L4 |
| 14 | [lan_display_server.cpp](../InkWord_Firmware/src/lan_display_server.cpp) | L53, L79, L119-125, L497-500 | `s_frame[EPD_FB_SIZE]` 静态接收缓冲；网页画布/CSS/JS 字面 `240/416`（L79 CSS、L119 `<canvas width="240" height="416">`、L125 `var W=240,H=416`）；C 侧校验 `content_len != EPD_FB_SIZE`（**宏派生**，L500 报错文案才含字面 12480）——线协议级几何耦合 | 跨切面 |
| 15 | [gpio_config.h](../InkWord_Firmware/src/gpio_config.h) | L33-39 | EVK011 板级引脚 `EPD_BS/SCK/MOSI/DC/CS/BUSY/RESET_PIN = 11/7/8/9/10/12/13`；L28-31 注释已写明 BS=-1 路径与条件编译保护 | L1 现状雏形 |
| 16 | [GxEPD2_374_DEPG0370.h](../InkWord_Firmware/src/GxEPD2_374_DEPG0370.h) | L26-37 | 类静态属性 `WIDTH=240 / hasColor=false / hasPartialUpdate / power_on_time=50 / full_refresh_time=1500...`——**事实上已是面板描述符原型** | L2 结构参考 |
| 17 | [platformio.ini](../InkWord_Firmware/platformio.ini) | L49-71 | 仅 `[env:inkword-s3]` + `[env:inkword-s3-demo]` 两个 env，无面板维度构建环境 | 构建矩阵落点 |

### 2.4 两个重要现状澄清（勘误既有认知）

1. **CJK 字库已是三级 16/20/24px**（非"24px 单档"）：见
   [cjk_font.h](../InkWord_Firmware/src/cjk_font.h) L16-L20（`CJK_FONT_LEVELS 3`、
   3,892 字形）与 [cjk_text.h](../InkWord_Firmware/src/cjk_text.h) L32-L50
   （`cjk_text_width/draw/draw_wrap` 均已带 `level` 参数）。字库 bin ~631KB 已含三级。
   多屏适配的字库工作只剩"SMALL 档位接线 level 0"（§十一）。
2. **刷新调度已是纯阈值计数制**：见 [refresh_scheduler.c](../InkWord_Firmware/src/refresh_scheduler.c)
   L42-L54，L47-48 注释明示 2026-08-18 起不再预清屏（"先白后画"已废弃），局刷为
   无窗口整屏双 RAM 差分，计数达阈值由调用方整屏重绘走真全刷。

---

## 三、目标谱系与首批三屏

### 3.1 两轴正交模型

| 轴 | 档位 | 说明 |
|:---|:-----|:-----|
| **尺寸轴** | 2.7" (176x264) ~ 7.5" (800x480) | 覆盖小中大三档布局（§八） |
| **色彩轴** | BW / 3C (B/W/R) / 4C (B/W/R/Y) / 6C (Spectra 6: B/W/R/Y/B/G) | 色彩平面数 1 / 2 / 2 / 2~3 |

两轴正交组合成完整谱系，但**先编译期正交、后运行时组合**：单固件工程内通过
构建矩阵选择面板（§十四），不做运行时热切换。

### 3.2 在手屏幕清单（原首批三屏 + 后续接入）

| 屏 | 分辨率 | 色彩 | FPC | 控制器（候选） | 本地库面板类 | 状态 | 布局档位（横屏） | 双帧+画布内存 |
|:---|:-------|:-----|:----|:---------------|:-------------|:-----|:-----------------|:--------------|
| DEPG0370 | 240x416 | BW | 24P | UC8253 | 自写 GxEPD2_374_DEPG0370 | ✅ 在用 | MID (416x240) | 25+12.5KB |
| Hink E042A13-A0 | 400x300 | BWR | 24P | SSD1619（GDEH042Z96 同族） | 手写序列 panel_e042a13_ssd1619.cpp | ✅ 已验（2026-08-22 真机） | MID (400x300) | 60+30KB ≈90KB |
| WF0270 2.7" 三色 | 176x264 | BWR | 24P | SSD1680（实测判定；IL91874 文档候选证伪） | 手写序列 panel_wf0270_ssd1680.cpp | ✅ 已验（真机；三色无快局刷 §13.2） | SMALL (264x176, 16px CJK) | 23+12KB ≈35KB |
| 维峰 WF0290T5PCZ10230H | 128x296 | BW | 24P（WFT0290CZ10LP） | UC8151D（GDEW029T5D 同族，三轮收敛） | 手写序列 panel_wft0290.cpp（GxEPD2_290_T5D 忠实） | ✅ 已验（2026-08-26 真机） | TINY (128x296 竖屏持机) | 9.4+4.7KB ≈14KB |

- **2026-08-26 WFT0290 bring-up 收敛注记**（三轮判族 + 局刷调优，完整序列与
  实测史见 [panel_wft0290.cpp](../InkWord_Firmware/src/panels/panel_wft0290.cpp)
  文件头）：屏体标签 WF0290T5PCZ10230H（维峰），排线才是 WFT0290CZ10LP——
  FPC 料号是屏族检索键（GxEPD2 选型表：WFT0290CZ10 = GDEW029T5/T5D 族 =
  UC8151D）。一轮 SSD1680 假设证伪（BUSY 空闲 HIGH + 0x2F 读 0xFF）；二轮
  UC8253 demo 移植花屏（UC8151D PSR 只收 1 字节 + 必须显式发 0x61 TRES，
  UC8253 双字节 PSR 第二字节误锁存）；三轮 GxEPD2_290_T5D 序列一比一修复
  （全刷实测 3370ms vs 同族 3251ms 交叉铁证）。**局刷调优实锄两坑**：
  ① 双刷（passes=2）的残影与「翻页回退」双症同源于第二遍同向过驱动（单相
  LUT 无反向修正，粒子过冲回弹），passes=1 后两症俱消——`epd_gfx_flush_window`
  原硬编码 2 已改读 desc.passes；② 四相推拉（T1 反向预脉冲 + T3 主驱动，
  M06 压缩格式）时钟生效（950ms 实测）但残影无改善——单相快刷残影为固有
  特性，靠保养全刷周期清除（阈值 desc 化，wft0290 取 4，main.cpp 原硬编码
  8 已改读 desc）。**UC8151 LUT 压缩格式**（M06 同族揭示，跨面板知识）：
  每组 6B = 首字节 4×2bit 相电平（高 2bit=相1）+ 4 相长（时基 20ms/单位，
  实测 710ms@32 单位吻合）+ 组重复数；BW=0x48 / WB=0x84 / 其余 0x00。

- **2026-08-22 真机勘误**：初判「UC8176/IL0398 系（GDEW042Z15 同族，
  GxEPD2_420c 驱动）」有误，IC 实为 **SSD1619**（用户丝印考证：GDEH042Z96
  同族；RAM 命令 0x24/0x26、刷新 0x22+0x20、BUSY HIGH=忙 三差异真机实证）。
  GxEPD2 1.6.9 无 Z96 类 → 绕开库手写 SPI 序列（Waveshare epd4in2b_V2 +
  GDEH042Z96 官方规格书 §4.1）。bring-up 三根因全链路修复：① IC 判错
  重建驱动；② ESP32-S3 SPI.writeBytes 批量写 RAM 不落地（恒定全红），
  改 transfer 逐字节连发；③ 上层 ac_layer_color 条件颠倒致 red plane
  恒全置。终态：待机页文字正常，全刷 14580ms 稳定。
- Hink 型号不在 GxEPD2 官方面板列表；技术资料参考（2026-08-22 用户提供）：
  大连佳显 4.2 寸三色产品页
  <https://www.e-paper-display.cn/products_detail/productId=532.html>（B/W/R）；
  GDEH042Z96 官方规格书 §4.1 初始代码已入驱动（2017 批次 OTP 需显式
  下发 0x74/0x7E/0x0C/0x2B/0x01/0x3A/0x3B）。
- 两款三色屏均无快速局刷（§13.2；佳显官方产品页明示「仅单色屏支持局刷，
  三色屏不支持」，2026-08-22 厂商级确认，与库内参数互证）→ 首个色彩屏接入
  即验证 UX 降级路径（待机页引文手动轮换、全刷 15~25s 状态提示）。
- 4.2" 三色 ≈90KB 内部 SRAM 可承载（与 WiFi/BLE 栈共存偏紧，PSRAM 兜底就绪）；
  2.7" 富余。
- v1.4 板三合一座子对两款新屏 24P FPC 的兼容性属硬件判断，**待上机确认**。

### 3.3 四条铁律（源自既定架构决策）

1. **单工程不分仓库/分支**：全部面板共存于 InkWord_Firmware/src/；
2. **UI 层只依赖 `epd_gfx_width()/height()`**：严禁硬编码坐标（以
   wifi_config_ui.c L82-83 为范式基准）；
3. **驱动差异封装进 panels/ 独立文件**：面板专属时序不泄漏到 epd_driver；
4. **选择机制编译期 `INKWORD_PANEL_ID`**（PlatformIO env），量产预留 NVS 运行期扩展。

### 3.4 非目标（明确边界）

| 非目标 | 理由 |
|:-------|:-----|
| 运行时面板热插拔/自动探测 | 未测面板的运行时崩溃路径不可控；与现有 env 编译期体系不符 |
| 多面板同工程同帧渲染 | 单面板实例即可满足全部场景 |
| 6C（Spectra 6）首批实现 | 本地 GxEPD2 无此面板类，需自写驱动（架构预留，Phase 8+） |
| EPDiy / IDF 重构 | PRD V2.1 已定 Arduino + GxEPD2 为基线，EPDiy 为远期评估 |

---

## 四、五层架构总览

### 4.1 分层图

```
┌─────────────────────────────────────────────────────────────┐
│ L5 UI 页面层  standby_page / reader_engine / main(学习页) /  │
│               wifi_config_ui —— 只见 gfx API + layout 档位   │
├─────────────────────────────────────────────────────────────┤
│ L4 布局档位层  layout_profile —— 按短边像素分档，与面板轴解耦 │
├─────────────────────────────────────────────────────────────┤
│ L3 GFX 抽象层  epd_gfx（epd_driver.cpp 泛化）—— 画布/字体/  │
│               转置/多色彩平面，尺寸与色彩全部运行期取自 desc │
├─────────────────────────────────────────────────────────────┤
│ L2 面板描述符层  epd_panel.h + panels/*.cpp                  │
│               —— epd_panel_desc_t 注册表（色彩能力/平面编码）│
├─────────────────────────────────────────────────────────────┤
│ L1 板级支持层  board_v14 / board_evk011                      │
│               —— 与面板轴正交的转接板差异（引脚/BS/CS）      │
├─────────────────────────────────────────────────────────────┤
│ L0 驱动实现   GxEPD2 BW/3C/4C 面板类 / 自写直驱序列          │
│               （GxEPD2_374_DEPG0370 先例）                   │
└─────────────────────────────────────────────────────────────┘
数据流：UI 绘制 → GFXcanvas（L3）→ 转置/多平面展开（L3）
      → ops.write_*（L2 分发）→ 面板类/demo 序列（L0）→ SPI → COG
```

### 4.2 每层职责与依赖规则

| 层 | 职责 | 允许依赖 |
|:--|:-----|:---------|
| L5 | 页面内容与交互；只调用 `epd_gfx_*` + layout 参数 | L4、L3 |
| L4 | 按运行期 gfx 尺寸选档，提供布局参数表 | L3 |
| L3 | 画布/绘图/字体/转置/刷新编排；持有唯一 desc 指针 | L2 |
| L2 | 面板描述符与 ops 注册表；`epd_panel_get_by_id()` 查表 | L1、L0 |
| L1 | 板级引脚表、BS/CS 配置、电源策略 | 无（纯宏/表） |
| L0 | 控制器命令序列（GxEPD2 面板类或自写） | L1 引脚宏 |

**规则：只允许上层依赖下层，禁止跨层与反向依赖**（如 L5 不得直接摸 SPI）。

### 4.3 现状代码 → 目标层映射

| 现状文件 | 目标层 | 迁移动作 |
|:---------|:-------|:---------|
| epd_driver.cpp（几何/转置/刷新编排部分） | L3 | 泛化尺寸与色彩（Phase 2） |
| GxEPD2_374_DEPG0370.* | L0 | 原地保留，被 panels/ 包装（Phase 1） |
| （新增）epd_panel.h + panels/*.cpp | L2 | 全新（Phase 1） |
| gpio_config.h EPD_* 引脚段 | L1 | 拆 board 宏（Phase 0） |
| main.cpp / standby_page.c / wifi_config_ui.c 布局常量 | L5→L4 | 参数化进 profile（Phase 4） |
| cjk_text/cjk_font | L4 侧工具 | level 接线（Phase 5） |
| lan_display_server.cpp | 跨切面 | 协议 v2（Phase 6） |

---

## 五、L2 面板描述符设计（核心章）

### 5.1 epd_panel_desc_t 结构体

字段命名对齐 [GxEPD2_374_DEPG0370.h](../InkWord_Firmware/src/GxEPD2_374_DEPG0370.h)
L26-37 既有静态属性（WIDTH/hasPartialUpdate/power_on_time...），降低迁移认知成本：

```c
/* epd_panel.h —— 面板描述符（L2 核心数据结构） */
typedef enum {
    EPD_CTRL_UC8253, EPD_CTRL_SSD1680, EPD_CTRL_SSD1681,
    EPD_CTRL_IL0398, EPD_CTRL_IL91874, EPD_CTRL_UC8179,
    EPD_CTRL_JD79686, EPD_CTRL_SSD1619, EPD_CTRL_UNKNOWN,
} epd_controller_t;   /* 色彩面板控制器在选型时按 SOP（§十六）核对 */

typedef enum { EPD_COLOR_BW, EPD_COLOR_3C, EPD_COLOR_4C, EPD_COLOR_6C }
    epd_color_mode_t;

typedef enum { EPD_FB_AUTO, EPD_FB_SRAM, EPD_FB_PSRAM } epd_fb_location_t;

typedef struct epd_panel_desc {
    /* —— 身份 —— */
    const char          *name;          /* "depg0370_uc8253" */
    epd_controller_t    controller;

    /* —— 几何 —— */
    uint16_t            panel_w, panel_h;   /* 物理竖屏分辨率 */
    uint8_t             gfx_rotation;       /* UI 横屏旋转 {0,1,2,3} */
    /* gfx_w/h 派生规则：rotation 奇数 = (panel_h, panel_w)，偶数 = (panel_w, panel_h) */

    /* —— 色彩 —— */
    epd_color_mode_t    color_mode;
    uint8_t             plane_count;    /* BW=1 / 3C,4C=2 / 6C=2~3 */
    uint8_t             palette[16];    /* 逻辑色→平面位掩码（§9.2） */
    epd_fb_location_t   fb_location;    /* 默认 AUTO：>128KB 阈值落 PSRAM */

    /* —— 时序 —— */
    uint16_t            rst_pulse_ms;
    uint8_t             busy_level;     /* BUSY 空闲电平（0/1） */
    uint32_t            busy_timeout_ms;/* 色彩面板 20~60s 各异，禁用全局默认 */
    uint16_t            power_on_ms, power_off_ms;
    uint32_t            full_ms, partial_ms;

    /* —— 刷新策略 —— */
    bool                partial_enabled;/* 色彩面板一律 false（§13.2） */
    uint8_t             passes;         /* 局刷遍数（默认 2，可 1） */
    uint8_t             partial_count_full_refresh; /* 计数阈值 */
    bool                window_8align;  /* 面板侧 x/w 8 像素对齐要求 */

    /* —— ops 函数指针表（L0 序列的统一入口） —— */
    struct {
        int  (*probe)(void);                        /* 读 MANUFACTURE_ID 等探测 */
        int  (*init)(bool partial_mode);
        int  (*write_full)(const uint8_t *frame);   /* 竖屏原始帧直通 */
        int  (*write_planes)(const uint8_t *const *planes); /* 多平面统一入口 */
        int  (*write_dual)(const uint8_t *prev, const uint8_t *new_);
        int  (*partial)(const uint8_t *prev, const uint8_t *new_);
        void (*power_off)(void);
        void (*deep_sleep)(void);
    } ops;
} epd_panel_desc_t;

/* 运行期查表（编译期由 INKWORD_PANEL_ID 选定；量产预留 NVS 覆盖） */
const epd_panel_desc_t *epd_panel_get_by_id(const char *id);
```

### 5.2 时序字段的实证来源

| desc 字段 | DEPG0370 现值出处 | 三色面板对照 |
|:----------|:------------------|:-------------|
| power_on_ms / full_ms / partial_ms | GxEPD2_374_DEPG0370.h L30-33（50 / 1500 / 500ms） | GxEPD2_420c.h L30-33（40 / 16000 / 16000ms） |
| busy_timeout_ms | GxEPD2_374 类内默认 | GxEPD2_420c/270c 均 20s；epd4c 族 25~50s（如 GDEY029F51H busy 50s）——**必须按面板填写** |
| partial_enabled | true（双 RAM 差分局刷在用） | 420c hasPartialUpdate=true 但 `usePartialUpdateWindow=false`（.h L28 注释 "needs be false to work"），且 full==partial==16s →「局刷」实为全屏闪刷，desc 一律按 false 设计 |

### 5.3 注册机制

每个 panels/*.cpp 导出一个 `const epd_panel_desc_t` 并经
`EPD_PANEL_REGISTER()` 宏挂入静态注册数组；编译期由 `-D INKWORD_PANEL_ID=xxx`
决定链接期选中的唯一 desc（未选中的面板单元以弱符号/条件编译排除出固件，
不占 Flash）。运行期 `epd_panel_get_by_id()` 仅在注册表内查表，为 NVS 量产
扩展预留。

```c
/* epd_panel.h —— 注册宏（链接期拼装静态注册表） */
#define EPD_PANEL_REGISTER(desc_var, id_str)                        \
    __attribute__((used)) const epd_panel_desc_t *                  \
        k_panel_##desc_var = &desc_var;

/* panels/panel_depg0370_uc8253.cpp —— 首个注册单元（Phase 1 交付物） */
const epd_panel_desc_t g_panel_depg0370 = {
    .name           = "depg0370_uc8253",
    .controller     = EPD_CTRL_UC8253,
    .panel_w        = 240,  .panel_h       = 416,
    .gfx_rotation   = 1,
    .color_mode     = EPD_COLOR_BW,
    .plane_count    = 1,
    .palette        = { [EPD_GFX_WHITE]=0x00, [EPD_GFX_BLACK]=0x01,
                        [EPD_GFX_ACCENT]=0x01, [EPD_GFX_AUX]=0x01 }, /* BW 退化 */
    .fb_location    = EPD_FB_AUTO,
    .busy_timeout_ms = 3000,
    .power_on_ms = 50,  .power_off_ms = 40,
    .full_ms = 1500,   .partial_ms  = 500,
    .partial_enabled = true,  .passes = 2,
    .partial_count_full_refresh = 8,
    .window_8align   = true,
    .ops = { /* 填 GxEPD2_374_DEPG0370 包装函数（附录 A 映射） */ },
};
EPD_PANEL_REGISTER(g_panel_depg0370, "depg0370_uc8253")
```

> **§5.3 兑现状态（2026-09-05）**：落地形态为 `epd_panel.c` 静态数组
> `s_registry[]`（extern 声明 + 追加一行三步注册）而非链接器 section
> 拼装——gc-sections 会回收无根引用的面板单元，静态数组是唯一可靠
> 形态；统一固件 `inkword-s3` + NVS `set_panel` 运行期选屏已落地，面板
> env 的 `-D EPD_PANEL_DEFAULT_ID` 仅作 bring-up 钉默认便利。注册表现
> 10 屏。新增两道防护：① `epd_panel_desc_check()` 契约校验器（14 项：
> name/几何范围/帧缓冲 ≤512KB/rotation≤3/color_mode↔plane_count 匹配/
> palette 掩码/dpi≠0/busy 参数/passes/ops 必填/partial_enabled↔partial
> ops 一致性），`epd_driver` init 时全注册表自检（违规 LOG_W）+ 选中屏
> fail-fast + 注册名唯一性；② UC8253 族 ops 宏模板
> `panels/uc8253_ops.h`（DEPG0370 与 3.1" 两屏合用，序列字节逐字保留，
> 减重复 ~120 行；GxEPD2 两类合并经预研否定——序列字节实质差异，前置
> 3.1" 序列定稿 + 黄金帧基线，详见宏头注释）。

---

## 六、L3 GFX 抽象层泛化

### 6.1 API 兼容契约（调用方零改动为硬指标）

[epd_driver.h](../InkWord_Firmware/src/epd_driver.h) 现有 19 个导出函数全部
**保持签名不变**：

| 类别 | 函数 | 多屏化处理 |
|:-----|:-----|:-----------|
| 几何（2） | `epd_gfx_width / epd_gfx_height` | 返回 desc 派生值（BW 快路径零开销） |
| 绘图（9） | `fill_screen / fill_rect / draw_rect / draw_hline / draw_vline / draw_text / text_bounds / draw_bitmap / read_window` | 画布按 desc 尺寸堆分配；`color` 参数语义扩展为逻辑色枚举（§9.4） |
| 刷新（3） | `flush / flush_window / flush_window_passes` | 内部按 desc.partial_enabled 分派；窗口按 gfx_rotation 转置 |
| 面板级（7） | `epd_driver_init / epd_power_on / epd_power_off / epd_clear_screen / epd_full_refresh / epd_deep_sleep / epd_get_manufacturer` | 转发 desc.ops；`epd_full_refresh` 保持竖屏原始帧直通语义（LAN 兼容） |

### 6.2 帧缓冲动态化

静态数组（epd_driver.cpp L53-54）改为 `epd_driver_init` 内按 desc 堆分配：

```c
size_t fb = desc->panel_w / 8 * desc->panel_h;      /* 单平面单帧 */
s_port_new  = epd_fb_alloc(fb * desc->plane_count); /* 新帧（多平面连续） */
s_port_prev = epd_fb_alloc(fb * desc->plane_count); /* 屏幕真实快照（局刷差分基准） */
```

`epd_fb_alloc` 按 desc.fb_location：AUTO 时总量 ≤128KB 走内部 SRAM
（`malloc`），超限落 PSRAM（`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`，
§十）。**帧缓冲只走 CPU 读写 + SPI 逐行发送，禁用 MALLOC_CAP_DMA**（PSRAM
DMA 误配是已知陷阱）。

### 6.3 转置泛化（消除 239 魔数）

`canvas_to_panel`（L83-98）/`gfx_rect_to_panel`（L103-119）按 `desc.gfx_rotation`
四方向通用公式实现，现 rotation=1 的
`px = (gfx_h-1) - cy; py = cx` 成为 case 1；同时 L86 的 stride=52、L116 的
`rx = EPD_GFX_HEIGHT-rx-rw` 全部改为按 desc 尺寸计算。四方向映射表（对齐
GxEPD2_BW setRotation 语义，_reverse 翻转位序）：

| rotation | gfx 尺寸 | 画布→面板像素映射 | 窗口映射 |
|:---------|:---------|:------------------|:---------|
| 0 | (panel_w, panel_h) | `px=cx; py=cy` 直通 | `px=x; py=y; pw=w; ph=h` |
| **1（现役）** | (panel_h, panel_w) | `px=(gfx_h-1)-cy; py=cx` | `rx=y; ry=x; rw=h; rh=w; rx=gfx_h-rx-rw` |
| 2 | (panel_w, panel_h) | `px=(gfx_w-1)-cx; py=(gfx_h-1)-cy` | 中心对称 |
| 3 | (panel_h, panel_w) | `px=cy; py=(gfx_w-1)-cx` | 镜像交换 |

首批三屏 rotation 全为 1（横屏持机），0/2/3 仅需保证编译正确 + 单元测试
坐标系用例覆盖（不必真机验证）——竖屏原生面板（如 4.2" 400x300 本就横优）
可选 rotation=0 零转置直通路径。

### 6.4 面板可插拔单元（panels/）

- **`panels/epd_bus.h/.cpp`（T1.1 已落地 ✓）**：L0 公共 SPI 原语收敛层——
  原六面板手写底层（epd_cmd / epd_dat / epd_write_buf / wait_refresh_done
  同构副本）归一为 `bus_cmd / bus_dat / bus_dat_stream / bus_dat_begin/put/
  end / bus_wait_idle / bus_wait_busy / bus_detect_alive`，SPI 三大陷阱
  注释单点沉淀（writeBytes RAM 不落地、CS↑ 重置地址计数器→单 CS 事务
  连续流、BUSY 两段式等待）。面板文件只留命令序列表 / LUT 常量 /
  行宽特例（K_ROW_BYTES）/ 真值表注释；gdew027c44 逐字节独立 CS 铁律
  （EK79652 锁存）以面板内 epd_write_plane 循环调 bus_dat 保留；
  depg0370 GxEPD2 包装路径不复用总线原语，仅 ops.diag 借 epd_bus
  位掩读；
- **`panels/panel_depg0370_uc8253.cpp`**：包装现有 GxEPD2_374_DEPG0370 +
  demo 忠实序列（`hwReset → initPartialDemo → demoWriteDualNoWindow →
  updateDemoPartial`，调用点 epd_driver.cpp L422-425；demo 原函数名映射见
  附录 A），导出 `const epd_panel_desc_t`；
- **新面板接入模板**（按本地库实测）：
  - **四色无需自写**——GxEPD2_4C + epd4c/ 面板族（GDEY029F51H 等）直接可用；
  - **三色走 epd3c/ 族**（GxEPD2_420c / GxEPD2_270c）；
  - **仅 6 色（Spectra 6）沿 GxEPD2_7C 模板自写驱动类**（全库无 6C 匹配，
    代码级确认）；
  - **UC 系沿用双 RAM 差分范式**（DEPG0370 先例）；
- **关键归一**：局刷两种流派（UC8253 无窗口双 RAM 全屏差分 vs SSD1680
  partial window）在 ops 层抹平，上层只传 prev/new 双帧，不感知流派。

### 6.5 构建接入

PlatformIO arduino 框架自动递归编译 `src/` 全部源文件，新增 `panels/`
子目录构建零障碍；[src/CMakeLists.txt](../InkWord_Firmware/src/CMakeLists.txt)
当前不参与构建（其 L5-8 注释明示 arduino 走 platformio.ini），但 SRCS 列表
需同步追加新文件作 IDF 迁移留档（双轨维护义务）。

---

## 七、L1 板级支持层

### 7.1 板宏与面板宏正交

```
INKWORD_BOARD_V14 / INKWORD_BOARD_EVK011   （板级轴，二选一）
INKWORD_PANEL_ID=xxx                        （面板轴，构建矩阵）
两轴独立组合，构建矩阵二维查表（§十四）
```

### 7.2 两板差异表

| 项 | EVK011-C（现役） | v1.4 通用板 |
|:---|:-----------------|:------------|
| EPD_BS_PIN | 11（gpio_config.h L33） | **-1**（板上硬接 4 线 SPI） |
| FPC 座子 | 24P 锁定 3.7" | 24P/26P/34P 三合一 ◐ 待上机确认 |
| CS 数量 | 单 CS | 双 CS（可选双屏/外设） |
| 采样电阻 | 固定 | 0.47R / 3R 可选 |
| 升压 | COG 自主（J1 FPC 引 GDR/RESE） | 板载自主，解耦 COG 时序 |

gpio_config.h L28-31 已写明 BS=-1 路径与 `epd_driver_init` 条件编译保护——
**Phase 0 有现成先例可循**，板宏拆分即把该模式推广到全部板级差异引脚。

---

## 八、L4 布局档位与 L5 UI 改造

### 8.1 四档 profile

| 档位 | 判定（运行期 gfx 短边） | 代表屏 | 版式 |
|:-----|:------------------------|:-------|:-----|
| LAYOUT_TINY | <140 px（2026-08-23 新增） | 2.13" (122x250)、2.9" (128x296) 电子标签屏竖屏 | 单列、16px CJK（level 0）、超紧凑头部（状态栏 24/边距 8）、学习页 3~5 字/行×7~9 行、待机引文 16px 紧排版、配网走 AP 门户（屏上键盘不适用） |
| LAYOUT_SMALL | 140~199 px | 2.7" (264x176) | 单列、16px CJK（level 0） |
| LAYOUT_MID | 200~319 px | 3.7" (416x240)、4.2" (400x300，含黑白版骨架) | 现有学习页单列同族 |
| LAYOUT_LARGE | ≥320 px | 7.5" (800x480) | 多列 / 更大留白 |

档位由运行期 gfx 尺寸判定函数选择（`layout_profile_get()`），与具体面板解耦；
各页面给出对应档位布局参数表——**参数表的值即下表现有常量的参数化归宿**。

以学习页（main.cpp L127-144）为例，MID 档 profile 字段与现值的映射样例：

```c
/* layout_profile.h —— MID 档（现有 416x240 布局的参数化形态，Phase 4 交付物） */
typedef struct {
    int16_t status_h, status_base;      /* UI_STATUS_H=32 / UI_STATUS_BASE=22 */
    int16_t margin_x;                   /* UI_MARGIN_X=16 */
    int16_t vsep_x, mean_x;             /* UI_VSEP_X=248 / UI_MEAN_X=264 → 按宽度比例 */
    int16_t word_base, phon_base;       /* UI_WORD_BASE=100 / UI_PHON_BASE=132 */
    int16_t foot_base, foot_top;        /* UI_FOOT_BASE=224 / UI_FOOT_TOP=206 */
    int16_t mean_top, mean_lh, mean_lines; /* UI_MEAN_TOP=48 / UI_MEAN_LH=26 / UI_MEAN_LINES=6 */
    int16_t root_top, root_lines, root_lh; /* UI_ROOT_TOP=156 / 2 / 20 */
    int     cjk_level;                  /* 档位→字库级：MID=2(24px) SMALL=0(16px) */
} layout_study_t;
const layout_study_t *layout_study(void); /* 内部按 layout_profile_get() 分档 */
```

比例化规则：x 向字段按 `gfx_w` 比例缩放（如 `vsep_x = gfx_w*248/416`），
y 向字段按 `gfx_h` 比例缩放，取整后微调保证 8 对齐约束（无窗口局刷已
解除 8 对齐要求，仅字体基线需稳定）。

**T1.5 兑现状态（2026-09）**：上述愿景已落地为 `src/layout_profile.h/.c`
单表 15 字段（kind/quote_level/reader_level + 几何 12 项：status_h、
margin_x、body_reserve、item_h、hint_h、rv_hint_h、font_lvl_main、
font_px_main、ascii_size_main、info_lh、tight_quote、narrow_tiny），
消费方 main/menu_ui/quiz_ui/chat_ui/review_ui/standby_page/browse_mode/
voice_search/settings_ui 九文件全部查表；字段值 = 迁移前各文件三元宏
取值**原样搬运**（视觉零变化铁律）；TINY 档内 122/128 宽特判档位化为
`narrow_tiny` 字段（get() 运行期填充，width 类判断属档位职责）。字号/
行距类运行时派生宏（依赖用户字号设置者）不表化，保留各文件派生式；
LARGE 档几何字段为预估值，「7.5" 上机校准」。上例 `layout_study_t`
为设计期映射样例，落地形态以 layout_profile.h 为准。

**P2 形态轴兑现（2026-09-05）**：档位表新增运行期字段 `form`
（`LAYOUT_FORM_LANDSCAPE / PORTRAIT / SQUARE`，`get()` 按 gfx 宽高比
填充，表值恒 LANDSCAPE）——起因：MID 档将共存 416x240 横 / 400x300
横 / 240x320 竖（3.1" COG）三形态，单维短边分档无法区分。首个竖屏
MID 消费方待 3.1" 真机校准参数（档位字段已就绪，UI 侧查 form 即可
分支）。测试：test_layout_form_axis 用例覆盖五屏形态。

### 8.2 Phase 4 改动面三分类清单

| 文件 | 纯绝对坐标（重点改） | 宏派生（跟随） | 公式化（已达标） |
|:-----|:---------------------|:---------------|:-----------------|
| main.cpp L127-144 | `UI_VSEP_X=248 / UI_MEAN_X=264 / UI_WORD_BASE=100 / UI_PHON_BASE=132 / UI_FOOT_BASE=224 / UI_FOOT_TOP=206 / UI_MEAN_HINT_BASE=62 / UI_ROOT_TOP=156` 等 12 个 | `UI_MEAN_MAX_W / UI_WORD_MAX_W`（L137-138） | — |
| standby_page.c L91-101 | `SB_QUOTE_X0=112 / SB_QUOTE_Y0=8 / SB_W=192 / SB_H=176 / SB_ATTR_Y0=192 / SB_ATTR_X1=392`（改 `x0=(W-quote_w)/2` 类相对计算） | — | — |
| wifi_config_ui.c L54-79 | `TITLE_H / BOTTOM_LINE_Y=220 / KB_START_Y=74 / KB_KEY_W=36 / kb_w_r2[] / kb_w_r3[] / PWD_SHOW_MAX` 等 15+ 个（隐含 416 宽） | `SCR_W/SCR_H`（L82-83 动态获取 ✅ 范式） | — |
| reader_engine.c L31-38 | L386 `-80` 魔数 | `R_AREA_Y / R_MARGIN_X / R_MAX_W / R_MAX_H` | L436-443 占位页居中 ✅ |

改造顺序（Phase 4）：standby（6 宏，最小）→ main 学习页（12+ 宏，最大）→
wifi_ui（15+ 宏，键盘需按档位缩放或改列表式）→ reader（收尾）。

---

## 九、色彩平面与逻辑调色板

### 9.1 GxEPD2 本地库色彩能力总表（.pio/libdeps/inkword-s3/GxEPD2/src/ 实测）

显示类模板 4 个全部在库：`GxEPD2_BW / GxEPD2_3C / GxEPD2_4C / GxEPD2_7C`——
「显示类 x 驱动类」两轴宏选择与本项目两轴设计同构。模板内部均为分页小缓冲
（如 GxEPD2_3C.h L677 `_black_buffer[] + _color_buffer[]`）不持整帧，与本项目
"自管双帧"架构可无缝配合（epd2 层只消费外部缓冲）。

| 族 | 数量 | 代表型号 | 分辨率 | full 刷新 | busy 超时 | 局刷 |
|:---|:-----|:---------|:-------|:----------|:----------|:-----|
| epd3c/ 三色 | 33 | GxEPD2_270c (IL91874) | 176x264 | 16s | 20s | 无快速局刷 |
| | | GxEPD2_420c (GDEW042Z15/IL0398) | 400x300 | 16s | 20s | 同上 |
| | | GxEPD2_420c_Z21（变体） | 400x300 | 16s | 20s | usePartialUpdateWindow=true |
| | | GxEPD2_750c (UC8179) | 640x384 | 32s | 20s | 无 |
| epd4c/ 四色 | 16 | GDEY029F51H | 168x384 | 25s | 50s | 无 |
| | | GDEY0420F51 | 400x300 | 30s | 50s | 无 |
| | | GxEPD2_437c | 512x368 | 12s | 25s | 无 |
| | | GDEM075F52 | 800x480 | 21s | 50s | 无 |
| epd7c/ 七色 ACeP | 5 | GxEPD2_565c (GDEP0565D90) | 600x448 | 12s | 25s | hasPartialUpdate=false |
| （Spectra 6 六色） | **0** | —— 全库无匹配 | — | — | — | 须自写（沿 GxEPD2_7C 帧编码先例，GxEPD2_7C.h L133 4bpp） |

**关键事实**：3C 面板 `partial_refresh_time == full_refresh_time` 且
`hasFastPartialUpdate` 全为 false——「局刷」实为全屏闪烁刷 → desc 中色彩面板
`partial_enabled` 一律 false（§13.2）。佳显官方产品页亦明示「仅单色
（BW）屏支持局刷，三色屏不支持」（2026-08-22 核实，与库内结论互证）。

**变体警告**：GxEPD2_420c 与 420c_Z21 同尺寸不同 partial window 行为（.h L28
注释 "needs be false to work" vs "works fine"）——同型号变体必须读 .h 注释
再注册 desc，SOP 第 3 步固化（§十六）。

### 9.2 逻辑色与平面位映射

UI 层只见 4 个逻辑色，经 desc.palette 查表展开到平面位：

| 逻辑色 | 用途（黑白优先原则） | 3C (BW+R 平面) | 4C (BW+RY 平面) | 6C | BW 退化 |
|:-------|:---------------------|:---------------|:-----------------|:---|:--------|
| EPD_WHITE | 底色 | 00 | 00 | 白 | 白 |
| EPD_INK | 正文 | BW 面 1 | BW 面 1 | 黑 | 黑 |
| EPD_ACCENT | 强调（待机页出处、音标） | R 面 1 | R 面 1 | 红 | **降级黑** |
| EPD_AUX | 次强调 | R 面 1（同 ACCENT） | Y 面 1 | 黄/蓝/绿 | **降级黑** |

- 画布架构：**每色彩平面一块 GFXcanvas1**（沿用 1bpp 低内存栈）；
- `palette[16]` 每项为平面位掩码（如 3C 的 ACCENT = `0b11`：BW 面置 0 + R 面置 1，
  具体编码按控制器黑白/彩面极性在面板单元真值表定义）；
- BW 退化规则保证单色屏 UI 不空白不乱码；颜色仅作可选增强，**黑白优先设计
  不依赖颜色传达信息**。

### 9.3 写入序列归一

多平面写序（BW 面 + 色面，GxEPD2_3C 的 `writeImage(black, color, ...)` 双缓冲
入口，见 GxEPD2_420c.h L46）封装进面板单元 `ops.write_planes`，上层统一传
逻辑帧数组，不感知控制器写序差异。

### 9.4 绘图 API 色彩参数演进

`epd_gfx_draw_* / cjk_text_draw*` 的 `color` 参数语义从 0/1 扩展为逻辑色枚举
（`EPD_GFX_WHITE=0 / EPD_GFX_BLACK=1` 保持旧值兼容，新增 `EPD_GFX_ACCENT=2 /
EPD_GFX_AUX=3`）；BW 面板快路径在函数入口查退化表后维持原位运算不变。

### 9.5 验收基线

每块色彩屏列一个「平面编码真值表」单元测试用例（逻辑色 → 平面字节样本），
进 `test/` 或固件内自检任务（`inkword-s3-demo` env 无 SD 可测）。

---

## 十、内存预算与 PSRAM 策略

### 10.1 帧内存模型与预算表

单帧 = `panel_w/8 x panel_h x plane_count`；双帧（new + prev）= x2；画布
（1bpp/平面）同帧规模另计。全部面板档位复算如下：

| 面板 | 平面数 | 双帧合计 | +画布 | 归属 |
|:-----|:-------|:---------|:------|:-----|
| 2.7" BW 176x264 | 1 | ~11.6KB | ~5.8KB | 内部 SRAM |
| 3.7" BW 240x416（现役） | 1 | 25KB | 12.5KB | 内部 SRAM（现状：37.4KB 全 SRAM） |
| 2.7" 3C | 2 | ~23KB | ~12KB | 内部 SRAM |
| 4.2" 3C 400x300 | 2 | 60KB | 30KB ≈90KB | 内部 SRAM（偏紧，PSRAM 兜底就绪） |
| 7.5" BW 800x480 | 1 | 96KB | 48KB | 内部 SRAM 临界 |
| 7.5" 3C | 2 | 192KB | 96KB | **PSRAM** |
| 7.5" 档 6C（2~3 平面） | 2~3 | 192~288KB | 同规模 | **PSRAM** |

另：LAN 接收缓冲 `s_frame`（lan_display_server.cpp L53）泛化后按 desc 动态分配，
与主帧同规模（v2 协议下多平面连续布局）。

### 10.2 分配策略

- **内部 SRAM 阈值 ~128KB**：S3 内部 512KB，扣 WiFi/BLE/lwIP/httpd 栈后可用
  约 250-300KB，128KB 阈值留足协议栈余量；
- 超限自动落 PSRAM：DevKitC-1 N16R8 8MB OPI（platformio.ini
  `board_build.arduino.memory_type = qio_opi` + `CONFIG_SPIRAM=y` 已配置，
  当前固件 PSRAM 零使用——首次引入即本方案）；
- **帧缓冲只走 CPU 读写 + SPI 逐行发送，禁用 `MALLOC_CAP_DMA`**（PSRAM DMA
  误配是已知陷阱；转置/差分循环跑 cache 带宽 40-80MB/s，96-288KB 帧处理
  <5ms 可忽略）；
- 推论：ACeP 4bpp 先例下单帧 600x448x4bpp=131KB 已超阈——**6C 双帧必落
  PSRAM**，架构按此预设。

---

## 十一、CJK 字库多字号

### 11.1 现状（三级已就绪，非"单档"）

[cjk_font_data.bin](../InkWord_Firmware/src/cjk_font_data.bin) ~631KB（EMBED_FILES
编入，16MB Flash 富余）已含 **16/20/24px 三级、3,892 字形**（
[cjk_font.h](../InkWord_Firmware/src/cjk_font.h) L16-L20）；CKF1 bin 头布局：
24B 头 + u16 码点表 + n x 32/60/72B 三级位图；
[cjk_text.h](../InkWord_Firmware/src/cjk_text.h) L32-L50 的
`cjk_text_width/draw/draw_wrap` 均已带 `level` 参数；reader_engine 已按级取形
（墨迹盒变宽渲染）。

### 11.2 多屏适配工作（仅 UI 接线）

- SMALL 档位（2.7" 短边 176）：布局参数表选 level 0（16px）；
- LARGE 档位（≥320 短边）：**可选**扩展 32px 级——`tools/gen_cjk_font.swift`
  LEVELS 表加档，体积按 `n x stride x cell` 公式（32px 每字形 256B，分级码点
  仅收阅读高频字以控体积）；bin 头版本 bump CKF1 ver=2，cjk_font.c 按 levels
  字段前向兼容解析；
- 三级字号映射：布局档位 → 字库 level 的映射表进 layout_profile。

---

## 十二、LAN 直传协议 v2

### 12.1 现状锚点（尺寸耦合三层拆解）

[lan_display_server.cpp](../InkWord_Firmware/src/lan_display_server.cpp)：

| 位置 | 内容 | 多屏化处理 |
|:-----|:-----|:-----------|
| L497-500 | C 侧校验 `content_len != EPD_FB_SIZE`（**宏派生**，仅 L500 报错文案含字面 12480） | 改屏后宏自动跟随；v2 按帧头校验 |
| L79, L119-121, L125 | 网页端 CSS `#cv width:240px` / `<canvas width="240" height="416">` / JS `var W=240,H=416`（**字面硬编码**） | 必须走 v2 动态协商 |
| L521-523 | 收帧三联动：`epd_full_refresh + refresh_notify_full_done + ui_force_full_refresh_next` | 语义保持，帧内容变多平面 |

**结论**：C 侧是"宏跟随"（改屏自动适配，代价仅为 4.2" 帧 30KB/平面传输变慢），
网页侧是"字面定死"（换屏即画布错位）——v2 协议的主矛盾在网页侧。

### 12.2 v2 设计

- **新增 `GET /api/device-info`** 返回：
  `{panel_w, panel_h, gfx_w, gfx_h, fb_size, plane_count, palette, proto_ver}`；
  网页据此动态创建画布并按调色板渲染彩色预览；
- **帧格式扩展**：调色板索引位图——BW=1bpp（旧格式原样）/ 3C、4C=2bpp /
  6C=4bpp；v2 帧头 `magic + proto_ver + W/H（大端） + format(1|2|4bpp)`，
  body = `ceil(W*bpp/8) x H`，服务器按 desc 校验；
- **向后兼容**：不带帧头的 12,480B 裸 body 按 v1（竖屏 1bpp）处理；
  v1 客户端发错尺寸得 400 + 文案提示升级；
- `epd_full_refresh` C API **保持竖屏原始帧（多平面顺序）直通语义**，v2
  收帧后按 plane_count 切片调 `ops.write_planes`。

---

## 十三、刷新调度适配

### 13.1 现状（纯阈值计数制）

[refresh_scheduler.c](../InkWord_Firmware/src/refresh_scheduler.c) L42-L54：
每次局刷计数 +1，达阈值（main.cpp 初始化为 8；分页差异化：待机 12 / 学习·
阅读 8）返回 true 由调用方整屏重绘走真全刷；L47-48 注释明示 2026-08-18 起
不再预清屏（黑白深清 2x1.8s 太慢，真全刷已能洗净）；`epd_clear_screen`
黑白交替深清仅保留开机/长按低频路径。

### 13.2 面板参数化与色彩屏降级

- **阈值/passes/busy_timeout 参数化进 desc**（替代已废弃的"先白后画"策略）；
- 7.5" 局刷弱的面板策略降级（全刷为主 + 定时深清）；
- **色彩面板一律无局刷**（3C/4C/6C 面板特性，§9.1 实证：partial==full 且
  无快速局刷）→ 待机页引文自动轮换降级为**按键手动触发**（刷新次数计费
  纪律：一次全刷 16s+ 的屏幕闪烁成本不允许后台静默消耗）；
- 全刷 15~45s 的 UX 应对：刷新中状态提示、禁重复触发（按钮去抖加长）、
  （可选）彩色屏发送前二次确认。

---

## 十四、构建矩阵与 probe 环境

### 14.1 面板维度构建矩阵

[platformio.ini](../InkWord_Firmware/platformio.ini) L49-71 现有
`[env:inkword-s3]` + `[env:inkword-s3-demo]`（demo env 即 env 继承范式参照）。
扩展：

```ini
[env:inkword-s3]                    # 默认：DEPG0370 BW（现役，零改动）
[env:inkword-s3-e042]              # 4.2" 三色（实建 env 名）
build_flags = ${env:inkword-s3.build_flags} -D INKWORD_PANEL_ID=PANEL_E042A13
[env:inkword-s3-270c]               # 2.7" 三色（同构）
[env:inkword-s3-probe]              # 试屏探针（§14.3）
```

NVS 运行期选择：保留 `epd_panel_get_by_id()` 运行期查表接口，量产期以 NVS
键覆盖编译期默认——首批不实现，仅保证接口形状不堵死。

### 14.2 板级轴组合

`INKWORD_BOARD_V14 / INKWORD_BOARD_EVK011` 与 INKWORD_PANEL_ID 正交组合，
二维查表（板宏影响 gpio_config 引脚段与 BS/CS 处理，面板宏影响 desc 选择）。

### 14.3 probe 探针环境（吸收 ink_test 方法论）

新增 `[env:inkword-s3-probe]` + `src/probe_main.cpp`（或 tools/panel_probe）：

- **动机**：ink_test（[Readme.txt](../Info/ink_test/Readme.txt) L30「不知型号
  就只能一个个试」）的"逐个试驱动行"需 Arduino IDE 手工改行重编译——固件
  工程内做成自动循环；
- **行为**：在 INKWORD_PANEL_ID 候选矩阵上循环列面板类（如 420c → 420c_Z21
  → 270c），每档执行 ink_test 同款测试集：helloWorld / 全刷 / 局刷
  （`desc.partial_enabled` 门控）/ 字体 / 位图 / 深睡唤醒；
- **自校验**：串口日志（busy 实测时长 / init 结果）+ 屏显画面人工确认，
  实测数据直接回填 desc 时序字段；
- **首批受益**：4.2" Hink（先 GxEPD2_420c 不中再试 420c_Z21）与 2.7"（先
  GxEPD2_270c）均免改代码快速验证。

### 14.4 诊断编译开关 INKWORD_EPD_DIAG（T1.8 已落地 ✓）

`[env]` 默认 `-D INKWORD_EPD_DIAG=0`（生产态诊断不编入，Flash 实测
−5.3KB）；inkword-s3-demo 与 opm021eb-probe 置 1（bring-up 全量诊断
保留）。诊断输出统一走 `DIAG_LOG()` 宏（epd_bus.h，生产态空展开）；
epd_driver_init 的 BUSY 三态 / RST 脉冲 / BUSY 释放跟踪诊断段整体
`#if INKWORD_EPD_DIAG` 包裹；控制器判族状态读（原 is_ssd16 分支）
下沉为 `desc.ops.diag` 可选回调——族标准实现 bus_diag_uc /
bus_diag_ssd16 由 epd_bus 提供，L3 不再感知面板型号。

---

## 十五、分阶段迁移路径

| Phase | 内容 | 涉及文件 | 验收标准（可测项） |
|:------|:-----|:---------|:-------------------|
| 0 | v1.4 板级适配：BS=-1、板宏拆分 | gpio_config.h、epd_driver.cpp | 现屏在 v1.4 上行为不变 ◐ 座子兼容待上机确认 |
| 1 | 抽 epd_panel.h + panels/depg0370 迁移（纯重构） | 新增 epd_panel.h、panels/；epd_driver.cpp | 真机回归：开机白屏 / 待机引文轮换（SET 手动 + 定时自动）/ 阅读翻页局刷 / 学习页五向键 / LAN 直传 / 深睡唤醒，全刷局刷残影表现零回归 |
| 2 | 帧缓冲动态化 + 转置参数化 | epd_driver.cpp L53-54/L83-119 | 同屏内存水位不变（37.4KB）、局刷时序不回退 |
| 3 | 构建矩阵 | platformio.ini | 双 env 产物均正常烧录运行 |
| 4 | UI 去硬编码（standby → main → wifi_ui → reader） | §8.2 清单 | 同屏视觉零变化：`epd_gfx_read_window` 帧回读 diff 逐页比对 |
| 5 | CJK 16px 档接线 | layout_profile、standby/reader 接 level 0 | 3.7" 上以 16px 排版验证可读性（字库三级已就绪，仅接线） |
| 6 | 色彩基建（逻辑调色板 + 双平面帧缓冲 + LAN 协议 v2）+ 首块三色屏 4.2"  接入 | epd_gfx、panels/panel_e042a13_ssd1619.cpp、lan_display_server.cpp | 三色渲染正确 （平面真值表用例过）/ 无局刷 UX 降级生效（引文手动轮换、全刷时长提示）/ v1 网页 端不炸 |
| 7 | 2.7" 三色接入 | panels/panel_270c.cpp | SMALL 档 + 16px CJK 全页面可用，首批三屏闭环 |
| 8+ | 后续批次（选型开放）：7.5" BW / 四色（epd4c 族直用）/ 六色（自写）；可选 ink_test 所试 2.13" GDEH0213B72 作 BW SMALL 第四屏 | 逐屏注册 | 架构/协议就绪后单屏注册即交付 |

依赖链：0 → 1 → 2 → 3 为硬序；4/5 可与 3 并行（同屏验证不依赖新 env）；
6 依赖 2+4（双平面帧缓冲 + UI 去硬编码）；7 依赖 6（色彩基建）+5（SMALL 档）。

---

## 十六、新屏接入 SOP（8 步 checklist）

1. **查控制器**：FPC 规格书 + ink_test 试屏法（型号不明时逐个试驱动行）；
2. **定 desc**：尺寸 + 色彩 + 平面编码 + busy 实测容限（§5.1 字段逐项填）；
3. **选/写面板类**：先查本地 GxEPD2 epd3c/epd4c/epd7c 是否已有（**同型号
   变体必须读 .h 的 usePartialUpdateWindow/时序注释再定**，420c vs 420c_Z21
   教训）；无则沿 GxEPD2_7C 模式自写；
4. **probe 真机时序验证**：inkword-s3-probe 跑测试集，busy 时长 / 局刷能力
   实测回填 desc；
5. **调色板/平面映射验证**：平面编码真值表单元测试（§9.5）；
6. **布局档位确认**：短边判档，参数表缺项补齐（§8.1）；
7. **字库档位**：SMALL 接 level 0 / LARGE 评估 32px 扩展（§十一）；
8. **LAN 彩色预览验证**：device-info 协商 + 2bpp 帧回传屏显比对（§十二）。

---

## 十七、风险与对策

| 风险 | 对策 |
|:-----|:-----|
| 局刷机制差异（UC8253 双 RAM vs SSD1680 window） | ops 层归一（§6.4），上层只传双帧 |
| 3C/4C 无快速局刷（16~45s 全屏闪刷） | UX 降级矩阵：翻词等高频交互仅 BW 屏；彩色屏静态内容展示；刷新前二次确认（§13.2） |
| 7.5" 刷新时长/残影 | 策略降级：全刷为主 + 定时深清 |
| 内存峰值（大屏多色） | PSRAM 路径预算表 + 禁 DMA cap（§10.2） |
| busy_timeout 面板差异大（20~60s） | desc 强制字段，禁全局默认值；probe 实测回填 |
| 420c/Z21 同型号变体行为相反 | SOP 第 3 步读 .h 注释固化（§9.1） |
| 字库体积膨胀（扩 32px 级） | 分级码点收录 + bin 版本号 bump 前向兼容（§11.2） |
| 网页端兼容性（旧页面访问新固件） | v1 裸 body 兼容窗口 + 400 提示升级（§12.2） |
| 6C 波形/LUT 资料闭源 | 屏厂 demo 逆向对齐（DEPG0370 先例）；Phase 8+ 不阻塞首批 |
| BW 回退时逻辑色渲染一致性 | palette 退化规则 + 真值表用例（§9.2/9.5） |
| 本文档行号漂移 | 三重锚定（文件+宏名+行号），宏名为主键（§一约定） |
| v1.4 座子兼容性 / 新屏 FPC / 420c·270c 命中率 | 统一「待上机确认」标注 + probe 兜底路径 |

---

## 附录 A：demo 原函数名 ↔ 包装方法名映射表

[DEPG0370BBU253F33HP-M7.c](../Info/DEPG0370BBU253F33HP-M7_demo_code/DEPG0370BBU253F33HP-M7.c)
（屏厂 demo）→ [GxEPD2_374_DEPG0370.cpp](../InkWord_Firmware/src/GxEPD2_374_DEPG0370.cpp)
（本项目包装类）→ epd_driver.cpp L422-425（调用点）：

| demo 原函数（.c 行号） | GxEPD2_374_DEPG0370 包装方法 | 用途 |
|:----------------------|:------------------------------|:-----|
| Epaper_Initial_full_mode / Epaper_Initial_partial_mode（L169 起） | initFullDemo / initPartialDemo | 进全刷/局刷模式 |
| EPD_Dis_Part_RAM（L194 起） | demoWriteFull / demoWriteDualNoWindow | 写 COG SRAM（单/双 RAM） |
| Epaper_Update_partial（L254 起） | updateDemoPartial | 无窗口局刷差分 |
| Epaper_READBUS / GPIO 序列 | hwReset（COG 硬复位脉冲） | 复位时序 |

> 新面板接入时同样建议「demo 忠实复刻 → 包装类 → desc.ops」三层落地，
> 屏厂 demo 是时序问题的最终仲裁（DEPG0370 BUSY 恒低问题的解决先例）。

## 附录 B：候选型号矩阵（本地库实测面板清单节选）

| 尺寸 | BW 候选 | 3C 候选 | 4C 候选 | 备注 |
|:-----|:--------|:--------|:--------|:-----|
| 2.13" | GDEH0213B72（ink_test 在试） | GDEH0213Z98 | GDEY0213F51 | BW SMALL 第四屏候选 |
| 2.7" | —— | **GxEPD2_270c（首批）** | —— | IL91874 |
| 2.9" | GDEH029A1/DEPG0290；**WFT0290CZ10 FPC 已实证**（维峰 WF0290T5PCZ10230H = UC8151D，§3.2） | GDEW029Z10 | GDEY029F51H（busy 50s） | |
| 4.2" | GDEW042T2 | **GxEPD2_420c（首批）/ 420c_Z21** | GDEY0420F51（full 30s） | Hink E042A13-A0；官方资料：[佳显 4.2 寸三色产品页](https://www.e-paper-display.cn/products_detail/productId=532.html)，另有支持黑白快刷的 4.2 寸三色新品（good-display.cn/product/378）可作后续局刷化升级候选 |
| 5.79~5.83" | GDEW0583T7 | GDEW0583Z83 | GDEY0579F51（792x272 长条） | |
| 7.5" | GDEW075T7 (800x480) | GDEW075Z09 / GxEPD2_750c (640x384) | GDEM075F52 (800x480, full 21s) | LARGE 档 |
| 5.65" 7C | —— | —— | —— | ACeP：GxEPD2_565c（6C 自写参照） |

**术语表**：COG（Chip On Glass 面板控制器）/ FPC（柔性排线，P=pin 数）/
双 RAM（UC8253 两块全屏 SRAM 差分局刷机制）/ partial window（SSD1680 系
窗口局刷机制）/ 双帧（s_port_new + s_port_prev，局刷差分基准）/ ACeP
（Advanced Color ePaper，七色）/ Spectra 6（六色，无本地库支持）。

---

*本文档由三人视角交叉验证的代码证据驱动成文：架构可维护性 / 性能与内存 /
最小改动风险。行号基线 2026-08-22。*
