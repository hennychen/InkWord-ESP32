# InkWord_Firmware_BigScreen 大屏固件代码全景分析

> 分析基准：2026-09-17 当前工作区代码（约 9,500 行业务源码 + epdiy 组件）。
> 本文为代码结构/架构/实现细节的完整梳理，配套历史过程文档见
> `docs/ES108FC1C1_BIGSCREEN_BRINGUP.md`（bring-up 全程 run 记录）。

---

## 1. 项目定位与硬件链

| 项 | 内容 |
|---|---|
| 面板 | ES108FC1C1-RHY，1920×1080，10.1" 级大屏（VCOM 标签 -2.45V） |
| 主控 | 卖家 ESP32-S3 开发板（16MB Flash + 8MB OPI PSRAM） |
| 驱动链 | 卖家 V7 克隆驱动板，**16bit 并行总线直驱**（非 COG 屏 SPI） |
| 驱动库 | epdiy V7（卖家 epdiy2 魔改版为基线 + 本项目管线修复） |
| 框架 | ESP-IDF 5.5.3（PlatformIO espressif32@6.12.0，espidf 框架） |
| 姊妹项目 | `InkWord_Firmware`（小屏，Arduino + GxEPD2，SPI COG 屏） |

**隔离红线**：大屏项目严禁参考/修改小屏项目代码与配置；GPIO 定义仅存在于本项目
`src/driver/board_config.h`。两个项目物理隔离（独立 platformio.ini / sdkconfig /
构建产物 / 烧录目标），防止驱动参数（LUT、时序、VCOM）错配烧屏。

**项目演进脉络**（从环境命名可见）：Phase 0 骨架（bigscreen-s3）→ 卖家 demo 复刻
A/B 基准（bigscreen-demo）→ 八步 bring-up 探针（bigscreen-probe）→ LAN 图片上传
实验台（bigscreen-lan，run87-110 实证链）→ 学习闭环主应用（bigscreen-app，
小屏功能迁移 Phase A+B，2026-09-16）。

---

## 2. 构建体系

### 2.1 五构建环境（platformio.ini）

| 环境 | 宏 | 用途 | 编入源 |
|---|---|---|---|
| `bigscreen-s3`（默认） | — | 安全初始化 + 全白清屏后挂起（骨架验证） | main.c + driver |
| `bigscreen-demo` | `BIGSCREEN_SELLER_DEMO` | 卖家 ED060KD1 demo 复刻（A/B 已知良好基准） | + test/demo + test_basic + lan |
| `bigscreen-probe` | `BIGSCREEN_PROBE` | 八步 bring-up 序列（每步 8s 观察窗） | 同上 |
| `bigscreen-lan` | `BIGSCREEN_LAN` | WiFi AP + HTTP 图片上传实验台 | 同上 |
| `bigscreen-app` | `BIGSCREEN_APP` | **学习闭环主应用**（词库/FSRS/词卡/阅读器/菜单/LAN 门户） | app/ + gfx/ + 1.7MB 内嵌资产 |

关键机制（pio espidf 实测坑位，platformio.ini 注释留档）：

- `build_flags` 的 `-D` 只进 SCons 编译命令，CMake configure 看不到 → 源列表条件
  编入需双通道：`board_build.cmake_extra_args = -DBIGSCREEN_APP=1` 传 CMake 变量；
- 子环境 `build_flags` 是**整体替换**，须显式 `${env.build_flags}` 继承警告基线；
- 平台版本勘误：espressif32@7.0.1 会强制 IDF 6.0.1（rmt/lcd_periph_signals 已删，
  epdiy V7 不兼容），钉死 6.12.0（IDF 5.5.3）。

### 2.2 分区表（partitions_bigscreen.csv，16MB Flash）

```
nvs      0x9000   0x30000  (192KB，扩容原因：lr_state sparse blob 4000词×22B≈88KB)
phy_init 0x39000  0x1000
factory  0x40000  0x600000 (6MB，含内嵌词库 568KB + CJK 字库 ~631KB + 业务代码)
storage  0x640000 0x9C0000 (≈9.75MB SPIFFS 书库 /storage/books/)
```

### 2.3 sdkconfig.defaults 要点

- PSRAM：OPI 模式 + 80MHz + **64B cache line**（epdiy V7 推荐，否则运行时降半像素时钟）；
- 自定义分区表 + 16MB Flash；
- 抑制 legacy I2C / ADC 弃用警告（epdiy V7 依赖旧 API，Phase 2 再迁新 API）；
- 强制 UART0 控制台（禁 USB Serial/JTAG 防串口冲突）；
- **关闭双核 IDLE 看门狗**：feed 线程满载自旋会饿死 IDLE，渲染挂死由
  lcd_do_update 的 10s 超时诊断兜底。

### 2.4 源编排（src/CMakeLists.txt）

- 基础源（所有环境）：`main.c` + `driver/panel_es108fc.c`；
- 非 APP 环境：+ test_basic / demo_seller / lan_image / waveform_scanq；
- APP 环境：+ app 全家（core/input/ui）+ gfx + `lan/waveform_scanq.c`
  （binfast 波形，flash 增量仅 2KB LUT）+ `app/data/embed_data.S`；
- include 平铺（小屏源码风格 `#include "xxx.h"` 无目录前缀）；
- `.incbin` 资产经 `-Wa,-I,<dir>` 传汇编器搜索路径（gcc -I 不达 as，实测坑）。

---

## 3. 分层架构总览

```
┌────────────────────────────────────────────────────────────────┐
│ src/main.c            入口：宏分派 5 条路径（§4）                │
├────────────────────────────────────────────────────────────────┤
│ src/app/             学习闭环业务层（BIGSCREEN_APP）             │
│  ├ app_main.c        init 链(8步) + 事件循环 + base 页按键编排   │
│  ├ core/             业务核心（小屏近零改动移植 + 精简版）        │
│  │   page_router     两级页面模型（base + 覆盖层栈）             │
│  │   study_mode_machine  学习模式状态机（精简版）                │
│  │   learning_state  每词学习状态（FSRS+收藏+墨封+错词，NVS）     │
│  │   srs_engine      FSRS-4.5 纯算法（与后端同源对拍）           │
│  │   word_parser     JSON 词库解析（rodata 直吃）                │
│  │   cjk_font/text   中文点阵字库 + 混排断行分页                 │
│  │   layout_profile  布局档位（XLARGE 新增档）                   │
│  │   reader_engine   大屏阅读引擎（排版/页表/进度）              │
│  │   chapter_index / bookmark_mgr / storage_manager  阅读器族   │
│  │   epd_geom / quotes_app / settings_keys / app_stubs / ...   │
│  ├ ui/               word_card / menu / settings / standby /    │
│  │                   reader_page / reader_menu / lan_portal     │
│  ├ input/            button_handler（ADC 三键）+ console_cmd     │
│  └ data/             embed_data.S（词库+字库+演示书 内嵌资产）   │
├────────────────────────────────────────────────────────────────┤
│ src/gfx/             epd_gfx：1bpp 画布绘图层（小屏 API 同形）    │
├────────────────────────────────────────────────────────────────┤
│ src/lan/             lan_image（实验台）/ lan_portal（产品化）    │
│                       waveform_scanq（scanq/binfast 波形）       │
├────────────────────────────────────────────────────────────────┤
│ src/test/            test_basic（八步 bring-up）/ demo_seller    │
├────────────────────────────────────────────────────────────────┤
│ src/driver/          panel_es108fc + board_config（GPIO 权威）   │
│ src/config/          display_config（屏参集中管理）              │
│ src/hal/             hal_display.h（Phase 2 抽象接口骨架，未实现）│
├────────────────────────────────────────────────────────────────┤
│ components/epdiy             卖家 epdiy2 库 + InkWord 修复（§6） │
│ components/epdiy_seller_ref  卖家原库只读镜像（A/B 对照基准）     │
└────────────────────────────────────────────────────────────────┘
```

依赖方向严格自上而下；`hal/` 是规划中的显示抽象层（业务禁直调 epdiy），Phase 1
bring-up 期间 test_basic/lan_image 直连 epdiy 属诊断代码豁免。

---

## 4. 入口 main.c：五路宏分派

```
app_main()
 ├ PSRAM 容量报告（帧缓冲 1920×1080@4bpp ≈ 1MB 依赖）
 ├ BIGSCREEN_SELLER_DEMO → demo_seller_task   (16KB栈, prio MAX-1, 无绑核)
 ├ BIGSCREEN_LAN         → lan_image_task     (16KB栈, prio MAX-1)
 ├ BIGSCREEN_PROBE       → bringup_run_all_task(8KB栈, prio MAX-1)
 ├ BIGSCREEN_APP         → app_main_start()   (app_task 16KB栈, prio MAX-2)
 └ 默认                    → panel_es108fc_safe_init() 全白清屏挂起
```

优先级设计依据（真机实证）：epdiy feed 线程满载自旋只做同优先级
`vTaskDelay(0)` 轮转，主控低于它会被永久饿死（静默挂死）；app 主任务取
MAX-2 低一档（不再与 feed 抢屏驱，按键/渲染经 gfx 层单点收敛）。

---

## 5. 驱动层（src/driver + src/config）

### 5.1 board_config.h —— GPIO 权威定义

**[库镜像] EPD 16bit 并行数据线**（epdiy V7 板定义，禁他用）：
D0~D15 = 5, 6, 7, 15, 16, 17, 18, 8, 9, 10, 11, 12, 13, 14, 21, 47

**[库镜像] EPD 控制信号**：CKH=4（水平移位时钟）、CKV=48（垂直时钟）、
STH=41 / LEH=42（方案原文写反已按库源码修正）、STV=45

**[自管] I2C**：SDA=39 / SCL=40，I2C_NUM_0；I2C_INT=38。
⚠ GPIO38 与 SD_CMD 共脚冲突：bring-up/屏驱阶段 I2C_INT 优先，SD 启用前置条件
= PCA9555 中断改轮询并释放 38 脚，禁止双活。

**[卖家板实证] 屏幕电源**：`BS_EPD_POWER_EN = 46`（高=供电）。卖家 epdiy2 魔改版
已注释库内 I2C PMIC 全链路（pca9555/tps 写全注释、set_vcom 空壳、PG 等待注释），
配套 .ino 以 GPIO46 直控电源。

**[待核对] TF 卡**（SPI 模式，未启用）：CLK=2 / CMD=38(冲突) / DAT0=1 / CS=0

**[预留 P3] 音频 ES8311 + NS4150B**：BCLK=22 / LRCK=23 / DSDIN=24 / DSDOUT=25 /
MCLK=3（独立 256×fs 实线，省线派生实测嘶嘶声不可用）。与屏侧 TPS65185=0x48、
PCA9555=0x20 共 I2C_NUM_0 总线（ES8311=0x18/0x19 无冲突）。

**[自绘板实证] 三按键 ADC 分压**：GPIO19（ADC2_CH8）单脚三键，分压网络各键落
不同 ADC 窗口。阈值初值：NONE≥3400 / K1<1200 / K2∈[1200,2400) / K3∈[2400,3400)
——须串口 `a` 命令实测修正。三键映射：键1=NAV_UP、键2=NAV_DOWN、键3=NAV_CENTER。
**ADC2 与 WiFi 硬互斥**：仅 APP 固件使用按键驱动，LAN 会话期软门挂起（§8.4）。

### 5.2 display_config.h —— 屏参集中管理

| 参数 | 值 | 说明 |
|---|---|---|
| PANEL_WIDTH/HEIGHT | 1920×1080 | 面板几何 |
| PANEL_BUS_WIDTH | 16 | V7 板 16bit 并行 |
| PANEL_BUS_SPEED_MHZ | 3 | 保守值。10MHz 下 ISR 消费(6.25μs/行)超双线程喂线能力，首帧 64 行后饿死；升至 5MHz 时帧时间 27→54ms 无副作用（TODO-C 升速需量化余量）。当前宏值 3 |
| PANEL_VCOM_MV | 2450 | 排线二维码标签 -2.45V；卖家板无 PMIC，实际 VCOM 由板载电位器决定，此值仅作范围检查（TODO-B 万用表校准） |
| PANEL_WAVEFORM | epdiy_ED047TC1 | 波形起点（方案原文 ED097TC2 有误，按卖家示例勘误；TODO-A 专用波形到位后替换） |

### 5.3 panel_es108fc.c —— 面板安全初始化与窗口局刷

**safe_init() 安全链**（幂等，可重复调用）：
1. PSRAM ≥2MB 运行时检查（assert 在 release 下会被剔除，故用返回值）；
2. 幂等重入：先 `epd_deinit()`，hl 帧缓冲**只分配一次**（deinit 不释放，
   重复 hl_init 每次耗 ~1MB PSRAM，速率爬升多轮会耗尽）；
3. `ensure_i2c_driver()`：卖家魔改版不装库内 I2C，此处补装（INVALID_STATE 容忍）；
4. GPIO46 先断电复位 20ms（对齐卖家 .ino setup 时序）；
5. `epd_init(&epd_board_v7, &s_display, EPD_LUT_64K)`；
6. VCOM 范围检查 1000~2500mV，越界拒绝上电（防烧屏红线）；
7. **全白基线（run98 定案）**：front=白 / back=黑 显式构造全屏白驱 diff 后
   GC16 刷新；`epd_clear()` 被禁用（缺每帧残留排空、时长十倍漂移、无签名可观测，
   run96/97 实证）。双白空 diff 为 no-op 不扫描（run92 踩坑）；
8. 电源**常驻**（run48/49：频繁断电上电致屏不稳定甚至黑屏；并行总线无 COG
   深睡命令可发）。

**双路径电源**：bring-up 步骤 2 I2C 扫描后 `set_pmic_online()` 钉路径——
PMIC 在线走库 `epd_poweron()`（真 PG 等待）；缺失（本卖家板实测）仅 GPIO46
使能/断电。两条路径 GPIO46 均拉高/低（无害双保险）。

**update_area() 窗口局刷**（run88 P4 路径复活）：卖家 patch 库的
`epd_hl_update_area` 已被魔改（diff_area 强制全屏、pw/pb 强制 false），真窗口
局刷走底层直调：
```
裁剪窗口 → epd_difference_image_cropped(真实差异窗口, 空diff=no-op直接返回)
        → epd_draw_base(MODE_PACKING_1PPB_DIFFERENCE | MODE_DU)   // 5相位, ~554ms 级
        → back 脏行整行 memcpy 同步（对齐库 highlevel.c 语义）
```

---

## 6. epdiy 组件：卖家库 + InkWord 修复差异

`components/epdiy`（构建用）与 `components/epdiy_seller_ref`（卖家原库只读镜像，
A/B 对照基准）的差异即本项目对底层的全部修复。diff 命中 7 个文件：

| 文件 | 修复内容（对应 run 编号） |
|---|---|
| `render_context.h` | NUM_RENDER_THREADS **2→4**（run42：1920 宽 2 线程产能 16% 垫错行，4 线程双核均分供给反超消费；内存代价 68KB 内部 RAM）；新增 `frame_output_done` 逃逸标志（run75） |
| `render.c` | feed 线程栈 **2K→4K**（run74：栈溢出踩坏 DMA 描述符→GDMA 停摆→EOF 冻死随机行）；任务绑核 `i%2`（run42：4 线程时 i=2/3 为非法核号）；优先级降一档（IDF 5.5 断言） |
| `render_lcd.c` | **饥饿垫行**（run34：1920 宽产速不均时 lq 瞬时空立即置 error 会确定性卡死；改读其他线程 lq 垫白行，连续 64 次全空才置 error）；**批边界流残差补偿 z=2**（run86/87：边界重启后行窗从流内残差开始，末批整体左移≈8px，行首补 2 字节黑垫吸收）；帧尾 `frame_output_done=1` 放行忙等 feed 线程（run75）；帧参数诊断日志 |
| `lcd_driver.c` | **dummy_bytes = bus_width/8**（run79：16bit 总线=2，对齐上游 main；卖家旧逻辑只在 8bit 走通）；初批 **vbp=0**（run79，卖家旧值 1）；帧启动自旋锁；run71 诊断计数器（vsync/eof/batches/va 回读/CKV/STV 电平，死锁现场区分用） |
| `render_context.c` | 新帧起点清 `frame_output_done`（与 lq_reset 同语义） |
| `tps65185.c` | 注释留档：本克隆板 I2C 控制链不存在，恢复写入会在 epd_poweron 路径 abort 无限重启（run89 实验后回退） |
| `CMakeLists.txt` | 库级警告压制（-Wno-unused 系 / -Wno-deprecated-declarations）——卖家魔改残留空置符号与 IDF 5.5 GDMA 弃用 API，项目源码仍全量开警告（零警告纪律的库级例外） |

---

## 7. gfx 绘图层（src/gfx/epd_gfx.c，437 行）

小屏 `epd_gfx_*` C API 的 epdiy 版实现——**API 形状借小屏（业务近零改动移植），
屏协议全按 lan_image.c 真机验收路径重写**。

### 7.1 双缓冲管线

```
业务绘图 ──→ PSRAM 1bpp 画布（1920×1080=259,200B，行宽 240B，MSB-first，bit=1 黑）
                │ flush：逐字节查表 s_expand[256]（1字节→uint32 = 8个4bpp像素）
                ▼
           epdiy 4bpp front_fb（1,036,800B；偶x低半字节/奇x高半字节，0=黑 15=白）
                │ epd_hl_update_screen(MODE_GC16)  或  窗口 DU 局刷
                ▼
           ES108FC1C1 面板
```

`s_expand` 展开表用 uint32 一次承载 8 像素（4×画布字节 == fb 全尺寸恰好铺满；
前版 uint16 表只覆盖 fb 前 50%，上下半屏拼接花屏即此口径错误）。

### 7.2 刷新族 API

| API | 波形 | 耗时 | 语义 |
|---|---|---|---|
| `epd_gfx_flush()` | GC16 或 binfast | 3.3s / 0.7s | 全屏展开 + 全刷 |
| `epd_gfx_flush_window(x,y,w,h)` | DU 5 相位 | ~0.5s 级 | x 对齐 8px（uint32 展开粒度）→ 窗口展开 → panel 层真实差异窗口 DU 扫描 |
| `epd_gfx_force_refresh()` | 同 flush | 同 flush | back 置黑构造全屏 diff → 白区全驱驱白（清灰染/残影，不依赖内容变化） |
| `epd_gfx_partial_supported()` | — | — | 恒 true（DU 窗口局刷已实装） |

**灰染纪律（run91，K=8）**：DU 局刷 nop 槽泵电荷随扫描累积，每 8 次局刷自动
插入 `force_refresh` 全屏重置。K 放大须有实测照片证据，不得纸面推断。

**binfast 二值快速波形（2026-09-17，默认开启）**：黑白跃迁各 bn 扫（默认 3+3
≈0.66s，vs GC16 30 相位 3.3s），同色 nop 零注入→翻转少边界扩散小→更锐。实现
约束：`hl->waveform` 不能常驻替换（DU 局刷的 MODE_DU 查找会因 binfast 描述仅含
type=2 失败），仅在 `epd_hl_update_screen` 调用窗口内临时替换、返回即恢复。
bn1/bn2 未真机收敛，串口 `w` 命令一键切 GC16 对照。本画布全部内容为 1bit
（UI/发图均经 1bpp 画布）→ 全场景适用。

### 7.3 文字系统

- ASCII：FreeSans 常规/Bold × 4 档（9/12/18/24pt）+ **Helv 36/48pt 常规/
  Bold 四表**（2026-09-17 UI 重设计增，tools/gen_gfx_font.py 自系统
  Helvetica 生成，dpi=141 官方口径），`font_size` 1~6 六档、`s_fonts[2][6]`
  分派；`draw_text` y=基线语义（与小屏一致）；`set_bold` 运行期切换；
- 中文/音标：`cjk_font` 点阵（**16/20/24/32/40/48px 六级**，2026-09-17
  扩级，bin 3,077,208B）经 `cjk_text` 混排，顶左原点 y 语义，blit 走
  `draw_bitmap`；
- `draw_bitmap/read_window`：Adafruit GFX 行主序 MSB-first 格式（与上传协议、
  cjk 点阵通用格式一致）；Helv 表 bitmapOffset 为**字节偏移**、字形起点
  整字节对齐（drawChar 权威口径，生成器 flush_to_byte 兑现）。

电源策略：画布层不碰电源开关（panel safe_init 后常驻上电；run48/49 禁频繁
断电，per-flush 断电只引入升压时序扰动）。

---

## 8. app 应用层（src/app，BIGSCREEN_APP）

### 8.1 app_main.c —— init 链 + 事件循环

**init 链 8 步**（分段计时，串口可观察各阶段开销）：

```
1 NVS init（异常自动擦除重挂）
2 SPIFFS 书库挂载（storage 分区 → /storage；失败不阻塞，阅读器回退演示书）
3 内嵌词库解析（rodata 直吃无拷贝；词池 PSRAM 预算 WORD_POOL_MAX=3000 ≈3.2MB，
  现状 2407 词；解析期 DOM 峰值 + fb 1MB + 画布 0.26MB < 8MB）
4 learning_state（NVS sparse 恢复：评分/收藏/墨封跨重启）
5 panel_es108fc_safe_init（PSRAM→epd_init→VCOM→GC16 白驱基线）
6 epd_gfx_init（1bpp 画布）
7 study_mode + button_handler + console_cmd（输入双通道）
8 page_router + 首帧词卡（阅读引擎惰性初始化，首进阅读页才建页表）
```

**事件循环**（app_task，16KB 栈，prio MAX-2）：
- `button_wait` 100ms 节拍消费（真实 GPIO / 串口注入同队列）；
- **待机**：5 分钟无操作 → 渲染待机页 → **断屏电**（分钟级低频安全；消除静置期
  VCOM 偏置累积灰染 + 省电）；LAN 会话期豁免（`lan_portal_active()`）；
- 待机唤醒：任意键 → `panel_power_on` + 500ms 升压稳定窗 + 画布同步 +
  `force_refresh` GC16 全驱（电源刚恢复，物理态重置最稳）。

**base 页按键编排（V2.1 交互总表大屏版）**：

| 键 | 短按 | 长按 |
|---|---|---|
| 上 | 释义词内翻页（翻完接上一词） | **全刷清屏**（ghost-clear：render_top + force_refresh） |
| 下 | 释义下翻页 / 下一词 | **切模式** FLASH↔REVIEW |
| 中 | 发音（桩，音频未接） | **菜单**（menu_ui_enter） |
| SET | 遮蔽/揭晓 | **收藏★**（收藏视图内=移出序列；墨封录内=启封移出） |
| RST | 回首词 | **错词本进出**（临时视图内=退出回闪卡；无错词拒绝） |
| 左 | 自评忘了 Q1（FSRS + 错词本连错累计） | — |
| 右 | 自评简单 Q5 + **墨封联动**（toggle 幂等）+ 序列收缩 | — |

### 8.2 core 业务核心（20 文件）

**page_router（119 行，小屏 T1.4 原样移植）**——两级页面模型：
- base 页（词卡）+ 覆盖层栈（menu/settings/reader/lan_portal…）；
- `page_t` 协议：`render / on_button(false=请求退出) / enter / exit / owns_display`；
- 栈串联制：菜单保持入栈、子功能页入栈其上、退出 pop 一层 render_top 自动恢复
  （「从哪进退哪」）；`pop_to_base` 供终结型动作直达 base；
- `display_claim/release/display_busy/top_owns_display`：显示占用真相源收敛于
  路由单点（LAN 自绘/渲染族守卫分工）。

**study_mode_machine（371 行，精简版）**——五视图学习状态机：
- 迁入：FLASH/REVIEW 循环切换、WRONGBOOK/COLLECTION/MASTERED 临时视图
  （learning_state 过滤视图全量复用）、遮蔽/揭晓、last_mode NVS 恢复（白名单
  仅 FLASH/REVIEW）、after_* 收缩钳位家族、seek（跳转即启封语义）；
- 枚举值与小屏 0~10 一一对应（契约稳定）；DICTATION/READER 值位保留但不进
  切换循环（大屏阅读走覆盖层页方案，页游标在 reader_page 而非模式机）；
  CHAT/QUIZ/BROWSE/VOICE/PRON 未迁移；
- 临时视图纪律：不入循环 / 不 NVS 恢复 / 不走 apply_mode。

**learning_state（866 行，全量移植）**——每词学习状态层：
- 数据：FSRS 节点 + 连错计数 + 收藏 + 墨封（mastered，Anki suspend 哲学：
  调度层过滤标志，FSRS/收藏原样保留，启封无损回队；置位同步清连错）；
- 持久化：NVS sparse blob（只存非默认态词，22B/词，LR05 格式；词库规模变化
  整体作废保守策略）；评分/收藏仅置脏，主循环 `maybe_save()` 静默 5s 落盘
  （按键路径零 NVS 阻塞，掉电窗口 ≤5s）；
- 视图族（O(N) 虚游走同构五例）：active（未墨封）/ wrong / collected /
  due（到期且本会话未评）/ mastered；
- 统计：今日首评/今日评分/连续天数（自治钟 = esp_timer 单调秒，无 RTC 域）；
- 上报事件环形队列（24 深，满覆盖最旧）API 随源码保留——大屏暂无
  sync_client 消费方，属后续云端同步接缝。

**srs_engine（134 行，全量移植）**——FSRS-4.5 纯算法：17 参数默认权重，
quality 0~5 映射 Again/Hard/Good/Easy；与后端 `FsrsService.cs` 严格同源，
`tools/fsrs_test_vectors.csv` 双端对拍（S/D 相对 1e-5，间隔整数精确相等）。

**word_parser（223 行）**——JSON 词库解析：`load_mem` 用
`cJSON_ParseWithLength` 直吃固件 rodata（无需 NUL 结尾）；WordEntry 1096B
（text/phonetic/meaning/example/audio/tag + root/inflections/source/grade/
cloud_id + difficulty/id）。

**cjk_font（80 行）+ cjk_text（255 行）**——中文字形与混排：
- 字库 `cjk_font_data.bin`（~631KB 内嵌）：四级 16/20/24/32px、3935 字形
  （GB2312 一级 ∪ 全角标点 ∪ ASCII ∪ IPA 21 字符），码点升序二分查找，
  由 `tools/gen_cjk_font.swift` 生成；
- cjk_text：UTF-8 混排、墨迹盒变宽、CJK 按字 / ASCII 按词断行、量测与分页
  绘制共用同一断行核心（建页与渲染必然一致）。

**layout_profile（148 行）**——运行期按 GFX 短边分档（TINY/SMALL/MID/LARGE/
**XLARGE**）；XLARGE（≥800px，1920×1080）为迁移新增档：几何按大屏专属词卡
布局取值，字库四级上限 32px。含 PPI 自动层（set_dpi 注入后字号双约束自动选级）。

**阅读器族（2026-09 移植适配）**：
- `reader_engine`（623 行）：书加载（SPIFFS 首书→演示书回退，整本入 PSRAM）→
  UTF-8 逐字解码 → 墨迹盒变宽排版 → 行级分页 + 段首孤行保护 → NVS 进度
  （rd_sig/rd_font/rd_lh/rd_page，FNV-1a 书签名 + scope 后缀实现多书进度隔离）；
  字号三档 20/24/32px、行距 1.5/1.6/1.8×，切级按"页首字符偏移"保位重建页表；
  几何常量 RD_STATUS_H=120 / RD_MARGIN_X=80 / RD_BOTTOM_BAR=60；
- `chapter_index`（354 行）：TXT（"第"开头短行）/ MD（# 标题）/ HTML（h1-h6）
  章节检测，兜底 ~2000 字自动分章；map_pages 二分填页码；
- `bookmark_mgr`（135 行）：每书 32 枚书签上限，NVS blob 按书 signature 分键；
- `storage_manager`（101 行）：SPIFFS 书库挂载（失败自动格式化重挂）+ 书架枚举。

**辅助模块**：`epd_geom`（转置/窗口映射/调色板纯函数，host 可测）、
`quotes_app`（待机页《传习录》24 条引文表，App 层内容与 Core 渲染分层）、
`settings_keys.h`（NVS 键单一权威表）、`debug_log.h`（LOG_I/W/E/D 统一宏，
IDF 直译版）、`schedule.h + app_stubs.c`（课程表/SD 字库桩：保持移植源最小
改动，后续阶段整模块替换）。

### 8.3 ui 页面族（7 页面）

**word_card_ui（288 行）——学习主 UI（2026-09-17 词典双栏重排，详见
BIGSCREEN_UI_REDESIGN.md）**：
- 三区范式：顶栏 120 黑底白字（模式名 + 墨封/★徽标 + pos/total）/ 主区
  880 / 底栏 80 静态键位带；
- 左栏 x∈[0,640] 黑底白字：词头 48pt Bold **自适应降档**（48→36→24pt，
  宽限 480px）基线 y=400 → 音标 40px y=480 → 徽标 tag·grade·source y=900；
- 右栏 x∈[720,1840]：释义 48px 断行、行距 76、9 行/页首行 y=218；遮蔽=
  黑块白字提示；
- **刷新三档**：首帧/切模式 → GC16 全刷；翻词/收藏变化（s_last_collected
  跟踪，★ 在顶栏需全屏）→ DU 全屏窗口；同词揭晓/释义翻页 → **DU 右栏窗口**
  flush_window(720,120,1120,880)（左栏词头不动是双栏局刷红利）；
- 释义多页词内翻页（mean_page_step）：上/下短按先词内翻页，边缘才接翻词；
  词/遮蔽态变化自动清零释义页码；空序列自显空态页。

**menu_ui（400 行）——精简菜单（2026-09-17 三区重排 + 行局刷）**：三区
（顶栏 120 黑底「功能菜单」+ 模式徽标 / 主区 880 / 底栏 80 键位带）；组头行
70px + 菜单项 140px（标签 32px 点阵 + 徽标全点阵右对齐），焦点黑底反白条；
三组 7 项 = 1190px > 880 → **s_off 滚动窗口实装**（ensure_visible 贪心）；
光标移动 = 新旧两行重绘 + 列表区窗口 DU（滚动时整区重绘）。暗色页→亮色
词典风为有意变更；徽标动态值（计数/页数/无书）；空态拦截（空收藏/墨封/
错词拒绝进入）；终结型动作（模式切换）直接 exit 回 base，覆盖层型（阅读/LAN）
入栈。

**settings_ui（329 行）——精简设置（2026-09-17 三区重排 + 行局刷）**：4 行
×140px 垂直居中（发音 开/关、字号 标准/大字/特大、音量 0-100 步进 10 + 十格
量程条、关于）；标签与值统一 32px 点阵（FreeSans 退出本页）；值变更/光标
移动 = 行重绘 + 列表区窗口 DU；NVS 惰性读写（set_audio/set_vol/set_font）；
取值 API 供发音门控等任何模块调用。

**standby_page（140 行）——学习统计仪表盘（2026-09-17 升级）**：上带今日
统计「新词 N · 复习 M」+「连续 X 天」40px（y240 细线收边）/ 中央《传习录》
24 条引文按小时轮换（RTC 未同步期用开机小时数取模）48px 楷体居中 + 出处
跟随引文块右下 / 下带 y740 细线：学习进度条（已学 = 词库 − 未学未墨封）+
「已学 N / M 词」+ 墨封·收藏·错词徽标 32px；GC16 全刷（低频页）。

**reader_page（242 行）——阅读页（覆盖层页方案）**：页游标单点持有于本模块
（模式机零改动）；状态栏 120px（书名·章节 32px｜页码 N/M，书签页加 *）/
正文区（reader_engine 渲染，五档 20~48px 默认 40px）/ 中文键位提示栏
80px（对齐三区）；按键：左右上下翻页、中=阅读菜单、SET=书签切换、RST=
回首页，长按中/RST 退出、长按 SET 字号步进；**翻页/书签 = 全屏窗口 DU
局刷**（2026-09-17，原恒全刷约束随词卡 run88 局刷验证解除），进页/字号/
间距切换仍全刷。

**reader_menu（552 行）——阅读菜单（内嵌子视图状态机）**：主菜单 4 项
（书架/目录/书签/阅读设置），子视图不额外 push 覆盖层；书架=演示书+SPIFFS
书库（小屏独立 book_shelf 页收敛为子视图）；跳转/选书/步进后 return false
退出菜单，页游标经 goto/font_step/spacing_step/load_book 协作接口回写。

**lan_portal（837 行）——LAN 图片门户（产品化，§9 详述）**。

### 8.4 input 输入层

**button_handler（自绘板 ADC 分压版）**：
- 50ms 轮询 + 连续 2 采样一致去抖（100ms）；长按 1.5s（30 采样）一次性触发；
- FreeRTOS 队列深 16 满丢最旧；真实扫描与 `button_inject`（串口通道）同队列
  同路径，主循环无差别消费；
- `button_scan_pause/resume`：ADC2/WiFi 硬互斥的会话级解法（软门挂起扫描 +
  resume 含通道重配保险）；
- ADC 不可用自动降级串口命令输入。

**console_cmd（145 行）——开发期主验证通道**：

| 命令 | 功能 |
|---|---|
| `u/d/l/r/c/s/t` + 回车 | 上/下/左/右/中/SET/RST **短按**注入 |
| `U/D/L/R/C/S/T` | 对应键**长按**注入 |
| `a` | ADC 按键实测诊断（连采 5 组 4 采样均值，供定阈值窗口） |
| `w` / `W` | 全刷波形 binfast↔GC16 一键互切 / 状态查询（含 bn 参数） |
| `?` | 重印帮助 |

### 8.5 data 内嵌资产（embed_data.S，.incbin 自包含）

pio espidf 的 EMBED_FILES 有 SCons 路径双前缀 bug → 手工 `.incbin` + 导出与
IDF EMBED 命名规则一致的 `_binary_*` 全局符号（消费方 extern 声明零改动）：

| 资产 | 大小 | 消费方 |
|---|---|---|
| cjk_font_data.bin | ~3.0MB（六级 3,077,208B，2026-09-17 扩级） | cjk_font.c |
| default_words.json | ~568KB（2407 词） | app_main → word_parser_load_mem |
| demo_book.txt | 内置演示书 | reader_engine 回退书源 |

---

## 9. lan 层：图片上传双形态 + 波形引擎

### 9.1 lan_image.c（1329 行，bigscreen-lan 实验台）

run87-110 实证链载体：SoftAP（InkWord-BigScreen / inkword123，192.168.4.1）
+ esp_http_server + DNS 劫持（所有 A 查询应答 AP 网关 → 手机 captive portal
自动弹页）。核心设计：**解码/缩放/灰度/FS 抖动全在浏览器侧**（canvas 等比
contain + Floyd–Steinberg），设备零图像解码依赖，只做解包 + GC16 全刷。

上传协议（与 epd_gfx 打包口径逐位一致）：
- 1bit 裸流 259,200B（MSB-first，bit1=白，8px/字节）；
- 4bpp gray16 裸流 1,036,800B（偶 x 低半字节，15=白）；
- `/wf` 端点：scanq/binfast 波形参数运行时重排（免重烧迭代标定）；
- WiFi 配网页（扫描/选择/密码/异步连接/NVS 凭据 run110）。

### 9.2 lan_portal.c（837 行，APP 固件产品化合并）

与实验台的五项架构差异（文件头留档）：
1. **复用 app 侧管线**：无独立 hl/epd_init——上传流解包直写 epd_gfx 1bpp 画布
   （1bit 流 bit1=白 与 draw_bitmap 天然对齐：铺黑底 + 白像素逐行 blit；
   4bpp 流按 FS 抖动二值化，run100 流式行版）；
2. **ADC2/WiFi 会话互斥**：enter 时 `button_scan_pause`，exit 时 resume+通道重配；
3. **会话生命周期 = 页面生命周期**：enter 起（WiFi+DNS+httpd）/ exit 收
   （httpd_stop→dns_stop→esp_wifi_stop）；netif/event/init 首会话一次建设，
   之后 start/stop 复用；NVS 凭据（`lanwifi` 命名空间）跨会话自动重连 STA；
4. **驱屏纪律（run97）**：一切驱屏收敛于常驻 portal 任务（8KB 栈，
   prio MAX-1 与 feed 同级）；httpd 任务只做网络 I/O + 收包写画布，上传完成
   投递 JOB_SHOW 作业（信号量同步）；信息页重绘同任务串行（busy 期间跳过）；
5. **流式收包零大缓冲**：行对齐分块 recv（1bit 行 240B / 4bpp 行 960B），收满
   一行解一行——不预占 1MB PSRAM（APP 固件稳态 PSRAM ~6.7MB 贴上限）。

HTTP 端点 8 个（max_uri_handlers=16 留余量，注册失败必打 ERROR——run105
槽满仅 warning 教训）：`/`（上传页）、`/upload`、`/status`、`/wifi`、
`/wifi_scan`、`/wifi_connect`、`/wifi_status`、`/wifi_forget`；404 重定向回根。
busy 期间按键全吞（防退出竞态——双任务同时驱屏是灾难场景）；中键短按退出。
`lan_page.h` 为浏览器侧上传页 HTML（run99 页面零改动复用）。

### 9.3 waveform_scanq.c（217 行）——波形引擎

背景（bring-up §15/§16）：LCD 16bit 并行路径**不执行波形 phase_times**
（每相位=一次 1080 行全扫 ≈110ms），ED047TC1 GC16 的 8~20ms 白驱回修相位被
过驱 ~10 倍 → 中间灰阶塌缩为白。以「1 扫描 = 最小时间量子」重排：

- **scanq**（GC16 灰阶标定用）：A 段 nsat 扫全驱黑饱和 + B 段 mmax 扫目标灰阶
  白回修——灰阶由白修扫描数 M=map[to] 唯一决定；
- **binfast**（产品默认，2026-09-17）：二值内容快速全刷，黑白跃迁各 n1/n2 扫
  （默认 3+3），同色/灰阶 nop；
- 结构体直接组装 epdiy 内部 `EpdWaveform`（type=2 命中 MODE_GC16 查找），
  LUT 打包与原波形逐位一致；APP 固件经 epd_gfx 消费（§7.2），scanq 部分被
  gc-sections 裁剪，flash 增量仅 binfast LUT 2KB。

---

## 10. test 层

**test_basic.c（325 行，八步 bring-up，bigscreen-probe）**：

| 步 | 内容 | 通过判据 |
|---|---|---|
| 1 | PSRAM 检测 | ≥2MB |
| 2 | I2C 全总线扫描 | TPS65185=0x48 / PCA9555=0x20 应答情况钉电源路径（本板实测 GPIO46 直控型） |
| 3 | 上电序列 | 双路径电源 + PG 链路 + 温度读取 |
| 4 | 全白清除（epd_clear） | 纯白无残留（注：安全初始化路径已改 GC16 白驱基线，本步保留 bring-up 诊断语义） |
| 5 | 四角像素 + 中心点 | 四点齐、无偏移/镜像（坐标映射与走线完整性） |
| 6 | 16 级灰阶条 | 相邻级可辨无塌缩 |
| 7 | 时序诊断帧 | 64px 棋盘 + 8px 周期标尺带：错缝=行内错位、行错位=批推进错拍、密度突变=DMA 断流起点 |
| 8 | 总线速率爬升 12→15→17MHz | 幂等重 init + 复合验证帧，失败回落保守档 |

步间 8s 观察窗人工确认；独立高优先级任务包装（prio MAX-1，否则饿死挂死）。

**demo_seller.c（423 行，bigscreen-demo）**：卖家 ED060KD1-EpdiyV7 demo 逐行
复刻（ES108FC 定义 5MHz / ED047TC1 / builtin 波形），与本项目修复管线做 A/B
对照的"已知良好基准"。刻意差异仅 4 处（屏定义对象、电源宏、末尾不深睡、字体
extern 引用）。含 run91 局刷保养（每 8 次局刷插 GC16 重置）。

---

## 11. tools 脚本（项目根 InkWord_Firmware_BigScreen/tools/）

| 脚本 | 功能 |
|---|---|
| decode_waveform.py | 离线解码 epdiy 波形头，核对 (to,from) 各相位 2bit 驱动码（0=nop/1=黑/2=白/3=浮空） |
| gen_gray_ramp.py | 生成 16 级灰阶标板裸流（gray16 + 1bit FS 抖动两种），curl 上传 /upload 验证灰阶塌缩签名 |
| sim_scanq.py | scanq 波形离线模拟/自检：LUT 往返校验 + 剂量表与原 GC16 意图剂量对照 + 单调性检查 |
| gen_cjk_font.swift | CJK 六级点阵 bin 生成器（小屏同源改造：LEVELS 扩 40/48px，产 cjk_font_data.bin + quotes_app.*；2026-09-17 UI 重设计） |
| gen_gfx_font.py | 系统 Helvetica.ttc → Adafruit GFX 格式 .h（freetype-py，dpi=141 官方口径，--check 校准；Helv 36/48pt 四表） |
| chuanxilu_quotes.txt | 引文表源（gen_cjk_font.swift 字符集输入） |

---

## 12. 任务与优先级模型汇总

| 任务 | 栈 | 优先级 | 环境 | 说明 |
|---|---|---|---|---|
| epd_prep ×4（epdiy feed） | 4KB | MAX-1，绑核 i%2 | 全部 | LUT 喂线，满载自旋 vTaskDelay(0) 轮转 |
| demo_seller / lan_image / bringup | 16/16/8KB | MAX-1 | demo/lan/probe | 与 feed 同级防饿死 |
| app_main（app_task） | 16KB | MAX-2 | app | 主控低一档；驱屏经 gfx 单点收敛 |
| console | 4KB | 3 | app | 串口命令注入 |
| lan_portal（常驻） | 8KB | MAX-1 | app | 显示作业执行体 + 信息页异步重绘 |
| dns 劫持 / ADC 扫描 / httpd | — | 系统 | 相应 | — |

配套全局决策：关闭双核 IDLE WDT（feed 满载饿死 IDLE 是 epdiy 设计预期形态）；
驱屏挂死由 lcd_do_update 10s 超时诊断兜底。

---

## 13. 关键工程纪律清单（代码中留档的实证结论）

1. **电源常驻**（run48/49）：禁频繁断电上电；仅待机（分钟级）与深睡前断电；
   唤醒后 500ms 升压稳定窗 + GC16 全驱重置。
2. **epd_clear 禁用**（run96/97）：一切清屏走 GC16 diff（run98 白驱基线）；
   双白空 diff 是 no-op 不扫描（run92）——清残影必须 back 置黑构造真实 diff。
3. **灰染治理**（run91）：DU 局刷每 K=8 次插全屏 GC16 重置；K 值放大须实测证据。
4. **单任务驱屏 + 优先级纪律**（run97/75）：驱屏任务与 feed 同级；帧输出完成
   置 frame_output_done 放行忙等；低优先级主控必饿死。
5. **ADC2/WiFi 互斥**：GPIO19 按键扫描与 WiFi 会话互斥，lan_portal enter/exit
   pause/resume 成对。
6. **零警告构建**：项目源全量 -Wall -Wextra，库级压制需登记在库 CMakeLists。
7. **防误烧红线**：本固件仅面向大屏新板，禁止烧录至小屏设备。
8. **波形迭代通道**：产品固件 binfast 参数经实验台 /wf 端点真机收敛后回填；
   串口 `w` 命令保留 GC16 一键回退对照。
9. **局部刷新真窗口语义**：卖家 patch 库 hl 局刷已魔改退化全屏，局刷必须走
   panel 层 difference_image_cropped + epd_draw_base 直调路径。

---

## 14. 遗留 TODO 与未迁移功能

**硬件/波形标定类**：
- TODO-A：获取 ES108FC1C1-RHY 专用波形替换 ED047TC1 通用波形；
- TODO-B：VCOM 板载电位器万用表实测校准（软件值仅记录）；
- TODO-C：总线速率升速前量化生产余量（当前产品路径 3MHz，demo 验证 5MHz）；
- binfast bn1/bn2（默认 3+3）尚未真机收敛（不足签名：黑不黑/白不白）。

**功能未迁移（模式机值位保留）**：DICTATION（听写）、CHAT（AI 对话）、QUIZ
（测验）、BROWSE（目录浏览）、VOICE（语音查词）、PRON（跟读）；音频子系统
（ES8311 引脚已规划 P3）；SD 卡（引脚冲突待解）；课程表；云端同步（learning_state
事件队列已保留，无消费方）；settings 完整版（震动/单词大小/测验/方向/面板/快捷键）。

**架构演进项**：hal_display 抽象层落地（Phase 2，业务禁直调 epdiy 的红线由其
承接）；epdiy 弃用 API 迁移（gdma 系，随上游 master 统一升级处理）；大字号
点阵资产扩级（单词当前 FreeSans 24pt 为上限）。
