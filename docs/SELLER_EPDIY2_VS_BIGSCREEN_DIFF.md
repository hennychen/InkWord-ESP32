# 卖家 ED060KD1-EpdiyV7 资料与 InkWord_Firmware_BigScreen 显示差异对照

> 状态：**对照分析定稿**（2026-09-16，基于卖家 epdiy2 原库逐文件 diff + 真机 bring-up run34~run109 实证）
> 适用：拿到卖家新资料 / 更新 epdiy 库 / 接入卖家系列新屏时，先读本文避免重走弯路
> 姊妹文档：`docs/ES108FC1C1_BIGSCREEN_BRINGUP.md`（管线定稿五件套详解，本文不重复展开）
> 卖家资料位置：`Info/ED060KD1-EpdiyV7示例和操作说明/`（Arduino 示例 + epdiy2.zip 库 + 操作截图）

---

## 0. 结论速览

两套代码面向**同一块硬件**（卖家 ESP32-S3 板 + V7 克隆驱动板），卖家资料证明"板能亮"，
当前项目在其上补齐"16bit 大屏 + IDF 5.5 下稳定、几何正确、灰阶可控"的全部工程化差异。
差异集中在四层：

| 层 | 卖家方案 | 当前项目方案 | 遗留风险 |
|---|---|---|---|
| 电源链 | 宏覆盖 `digitalWrite(46)` 直控，隐式绕过库 | `BS_EPD_POWER_EN` 权威定义 + raw GPIO 钉定 | 调库 `epd_poweron()` 必炸（§3） |
| LCD 引擎时序 | 8bit 屏时代老驱动 | 5 处时序修复（§4） | 回退任一项 = 黑屏/整屏偏移 |
| 渲染产能 | 2 线程 + 2K 栈 | 4 线程 + 4K 栈 + 队列治理（§5） | 1920 宽下死锁/错行 |
| 波形 | builtin ED047TC1 | 扫描量子化自研波形（§6） | 灰阶塌缩为白 |

---

## 1. 对象界定

| | 卖家资料 | 当前项目 |
|---|---|---|
| 工程 | Arduino IDE（`.ino` 包装 + `main.c`），库为 `epdiy2.zip`（epdiy 2.0.0 魔改版） | PlatformIO `espressif32@6.12.0` + **ESP-IDF 5.5.3** 原生 |
| 板 | 同一块：卖家 ESP32-S3 + V7 克隆驱动板 | 同左（引脚系统化于 `src/driver/board_config.h`） |
| 屏 | 示例内联定义 4 块屏，`epd_init` 实际传 **ES120**（需用户自改） | ES108FC1C1-RHY 1920×1080（`src/config/display_config.h`） |
| 库基线 | `Info/.../操作说明/epdiy2/` | `components/epdiy`（= 卖家库 + 7 文件修复）；`components/epdiy_seller_ref` 为卖家原库只读快照（diff 对照用） |

卖家 `main.c` 内联屏定义（切换屏 = 改 `epd_init` 第二参数）：

| 屏 | 分辨率 | bus_width | bus_speed | 波形 |
|---|---|---|---|---|
| ES080FC | 1800×600 | 16 | 17 | ED097TC2 |
| ES108FC | 1920×1080 | 16 | 17 | ED047TC1 |
| ES120 | 2560×1600 | 16 | 12 | ED047TC1 |
| **ED060KD1** | 1448×1072 | **8** | 20 | ED060SCT |

注意：ED060KD1 未入库（`displays.c` 无此条目），仅示例临时定义；卖家 `displays.c` 内置表
本身也被改过（ED060XC3 注释 1024→1448、ED097TC2 注释 1200→1448），对应操作截图
"5.屏幕自定义.png" 的用法——自定义屏走示例内联定义即可，不必改库。

---

## 2. 屏幕描述参数差异（直接影响时序）

| 参数 | ED060KD1 | ES108FC（卖家） | 当前项目 ES108FC1C1 | 差异说明 |
|---|---|---|---|---|
| 分辨率 | 1448×1072 | 1920×1080 | 1920×1080 | 同屏同参 |
| **bus_width** | **8** | 16 | 16 | ED060KD1 是 8bit 总线，其 20MHz 配置**不可照搬**到 16bit 屏 |
| **bus_speed** | 20 | 17 | **3**（demo 环境 5） | 10MHz 下 ISR 消费速率（6.25µs/物理行）远超喂线能力，首帧 64 行后饿死（EPD_DRAW_EMPTY_LINE_QUEUE，run34 实证）。升速见 bring-up 文档 TODO-C |
| 波形 | ED060SCT | ED047TC1 | ED047TC1（起步，TODO-A 换专用波形） | 与卖家对 ES108FC 的选择一致（display_config.h 勘误记录） |
| **VCOM** | — | `epd_set_vcom(1560)` | 2450mV（面板排线标签 -2.45V） | **数字不同但语义相同**：卖家库 `set_vcom` 为空壳（只存变量不落硬件），实际 VCOM 均由板载电位器决定。1560 仅为沿袭库内默认值 1600 附近 |

---

## 3. 电源管理链差异（结构性差异，红线所在）

### 卖家方案：宏覆盖 + 完全绕过库电源管理

- `main.c`：`#define epd_poweron() digitalWrite(46,1)` —— 用宏直接**替换**库函数，
  GPIO46 直控升压使能；`.ino` setup 先 `digitalWrite(46,0)` 断电复位。
- 卖家库 `epd_board_v7.c` 内部把 I2C PMIC 全链注释：i2c 安装、pca9555 写入、
  tps 写入、`set_vcom` 全空，**但残留 PG 轮询循环**（`pca9555_read_input` /
  `tps_read_register`）。正因如此 demo 必须用宏覆盖——若直接调库
  `epd_poweron()`，会走进残留轮询在无 I2C 控制链的克隆板上挂死。
- `lcd_driver.c` 将 GPIO46 配置为 DEBUG_PIN 输出（`epd_lcd_init` 内）。

### 当前项目：显式双路径 + 钉定 raw GPIO

- `src/driver/panel_es108fc.c`：`panel_power_on/off()` = GPIO46 直控 +
  可选库路径（`s_pmic_online` 开关），I2C 驱动补装（I2C_NUM_0, 39/40）供残留轮询用。
- **run89 真机教训（红线）**：本克隆板 I2C 控制链不存在，调库 `epd_poweron()` 会
  `ESP_ERR_INVALID_STATE` abort **无限重启**。LAN/生产路径全部钉死 raw GPIO46，
  **禁止 `epd_poweron()`**（`src/lan/lan_image.c` 设计决策 4）。
- GPIO46 = `BS_EPD_POWER_EN`（高=开），定义于 `board_config.h`，禁止挪作他用。

### 防再犯

- 任何新代码路径刷屏，电源只允许 `gpio_set_level(BS_EPD_POWER_EN, x)`；
  上电后留 ≥10ms 电源稳定窗再启动刷新。
- 若未来换用带真 PMIC 的板（I2C 扫描到 0x48/0x20 且可写），才考虑恢复库电源链，
  并以 `panel_es108fc_set_pmic_online(true)` 切换，两路径不可混用。

---

## 4. LCD 时序引擎差异（当前项目 7 文件修复中的 lcd_driver.c 部分）

卖家库 `output_lcd/lcd_driver.c` 是 8bit 屏时代（老 IDF）驱动，16bit 总线 + IDF 5.5
组合下有 5 处时序缺陷，当前项目已修复（定稿五件套详见 bring-up 文档 §0/§2）：

| # | 卖家库行为 | 当前项目修复 | 屏显后果（不修的话） |
|---|---|---|---|
| 1 | `dummy_bytes` 按 `bus_width==8` 判定（16bit 判定为 0 走 8bit 老路） | 机制保留但 `epd_lcd_init` 钉定 **0**（run85；IDF 5.5.3 外围已在行首插空字，再插整帧右移） | 整帧水平偏移 |
| 2 | 帧启动序：STV→CKV→1µs→引擎 | CKV 先行→STV 隔一行周期拉起→**自旋锁内**启引擎（run79 对齐上游 main） | 相位漂移 |
| 3 | 初批 `vbp=1` | `vbp=0`（run79） | 首批起始行错位 |
| 4 | `auto_next_frame=true` | **false**（run84：batch0 结束瞬间引擎自动开跑 batch1，vsync ISR 有 1-3 行服务延迟） | 末批整体下移 2-5 行 |
| 5 | 末批 `vfp=10` | **1**（run83：末批下移 ≈10 行与 vfp=10 精确吻合） | 末批整体下移 10 行 |

另增诊断钩子（`epd_lcd_dbg_counters/va/va2/pins`）：死锁现场区分
「LCD 引擎未跑 / EOF 停 / vsync 停」，排障时直接调用。

### 防再犯

- **禁止用卖家 epdiy2 原库直接替换 `components/epdiy`**——7 文件修复会全部丢失。
  更新库必须三方 diff（卖家新版 / epdiy_seller_ref / components/epdiy）逐项重放修复。
- 换 IDF 大版本前先查 `lcd_ll_*` / `lcd_periph_signals` API 存活性（IDF 6.0 已移除后者）。

---

## 5. 渲染产能差异（1920 宽暴露的瓶颈）

| 文件 | 卖家库 | 当前项目 | 实证 |
|---|---|---|---|
| `render_context.h` | `NUM_RENDER_THREADS=2` | **4** + 绑核 `i%2` | run42：2 线程产能缺口 16%（prepared=905<consumed=1080）→ 垫错行 |
| `render_context.c/.h` | — | `frame_output_done` 逃逸标志 | run75：feed 线程尾行满队列永久忙等 25s 误判 |
| `render_lcd.c` | 空队列立即置 error+黑行 | 跨线程借行垫白 + 连续 64 次全空才 error | run34：瞬空即 error = 确定性卡死 |
| `render_lcd.c` | — | 帧起点排空残留行 + frame_done 信号量（run72）；**z=2 字批边界残差补偿** | run71/72：上帧残留致 start_frame 永不触发；run86/87：末批整体左移 16px |
| `render_lcd.c` | notify 对核二连发（2 线程时代写法） | 4 线程全量分发 | run42：线程 2/3 收不到 notify、每帧必超时 |
| `render.c` | feed 线程栈 2K、优先级恰越界 | 栈 **4K**（run74）、优先级降一档 | 2K 栈溢出踩坏 DMA 描述符 → GDMA 停摆；IDF 5.5 espidf release 断言活 |
| `render.c` | 绑核 `i` | `i%2` | 4 线程下 i=2/3 为非法核号（S3 仅核 0/1） |

### 防再犯

- 主控任务优先级必须与 feed 线程同级（`configMAX_PRIORITIES-1`），否则 feed 忙等
  轮转点让不出核、帧完成后主控饿死静默挂死（main.c 已注释）。
- `epd_hl_init` 帧缓冲只分配一次（~1MB PSRAM），`epd_deinit` 不释放，重复 init
  会耗尽 PSRAM（panel_es108fc.c 已幂等处理）。

---

## 6. 波形与刷新策略差异

- **卖家**：`EPD_BUILTIN_WAVEFORM`（ED047TC1）+ MODE_GL16/GC16/DU，纯演示。
- **当前项目**：LCD 16bit 并行路径**不执行波形 phase_times**（每相位 = 一次 1080 行
  全扫 ≈110ms，ED047TC1 的 8-20ms 白驱回修相位被过驱 ~10 倍）→ GC16 中间灰阶
  塌缩为白（bring-up 文档 §15）。对策（`src/lan/waveform_scanq.h`）：
  - **run101 扫描量子化 GC16**：1 扫描 = 最小时间量子，A 段驱黑饱和 + B 段白回修，
    灰阶由白回修扫描数 M=map[to] 唯一决定；
  - **run109 binfast 二值快速波形**：黑白跃迁各 n1/n2 扫（默认 3+3），服务 1bit 内容；
  - `/wf` HTTP 端点运行时重排参数，免重烧真机标定；
  - **run91 局刷对策**：连续 nop 全扫会灰染未更新区，每 8 次局刷插一次全屏 GC16
    重置归零累积。

### 防再犯

- 1bit 内容走 binfast / 页面默认 FS 抖动（run100 起设备侧二值化兜底）；灰阶标定走 scanq。
- **禁用 `epd_clear()`** 作基线清屏：帧未等完屏停在驱黑阶段（bring-up 文档 §13.2），
  用 `epd_hl_set_all_white()` + GC16 更新替代。

---

## 7. 显示内容管线差异

| | 卖家 demo | 当前项目 LAN 链路（`src/lan/lan_image.c`） |
|---|---|---|
| 内容源 | 编译期内置图片/字体（zebra/board/beach、FiraSans） | SoftAP + 浏览器上传，设备零图像解码依赖 |
| 处理 | 无 | 浏览器 canvas 等比 contain（白底）+ 灰度 + Floyd–Steinberg 抖动 → 1920×1080 1bit 裸流（259200B）POST `/upload` |
| 设备侧 | `epd_hl_update_screen` | 解包入 fb → 全屏 GC16；更新在 httpd 任务内同步阻塞（GC16 ~3.3s），`s_busy` 期间 503 拒新请求 |
| 旋转 | `EPD_ROT_LANDSCAPE` | 同，**原生横屏不走旋转路径**（旋转路径在本 patched 管线未验证，不引入新变量） |

---

## 8. 构建环境差异

- 卖家：Arduino IDE + `psramInit()`；断言/栈检查弱（NDEBUG）。
- 当前项目：espidf release 构建断言仍活——卖家库 feed 线程优先级恰好等于
  `configMAX_PRIORITIES` 上限，Arduino 不炸、espidf 必炸（已降一档，上游 master 同款）。
- **平台钉死**：`platformio.ini` 用 PIO 官方 `espressif32@6.12.0`（IDF 5.5.3）。
  **不可用 pioarduino 7.0.1**（强制 IDF 6.0.1，`rmt_periph_signals`/`lcd_periph_signals`
  等寄存器描述已移除，适配等同重写寄存器级时序，烧屏风险）。
- 4 构建环境隔离：`bigscreen-s3`（默认安全骨架）/ `bigscreen-demo`（卖家流程复刻
  A/B 基准，bus_speed=5）/ `bigscreen-probe`（八步 bring-up）/ `bigscreen-lan`（WiFi 图片）。
- A/B 对照方法：`bigscreen-demo` = 卖家 epdiy2 原库语义（`src/test/demo/demo_seller.c`
  逐行复刻卖家 main.c，仅 4 处刻意差异：目标屏 ES120→ES108FC、电源宏等价替换、
  深睡改循环、字体 extern）；修复版组件输出与之对照定位回归。

---

## 9. 防再犯纪律汇总（一页清单）

1. **电源**：只允许 raw GPIO46（`BS_EPD_POWER_EN`）；禁止 `epd_poweron()/epd_poweroff()`
   （无 I2C 控制链，run89 abort 实证）；上电留 10ms 稳定窗。
2. **库更新**：禁止整目录覆盖 `components/epdiy`；必须三方 diff 重放 7 文件修复
   （`epdiy_seller_ref` 为基线快照）。修复文件清单：
   `board/tps65185.c`（仅注释）、`output_common/render_context.{c,h}`、
   `output_lcd/lcd_driver.{c,h}`、`output_lcd/render_lcd.c`、`render.c`。
3. **屏幕参数**：新屏先核对 bus_width（8/16 不可混用参数体系）；bus_speed 从 3MHz
   保守起步，升速前量化喂线余量（TODO-C）；VCOM 由电位器决定，代码值仅记录。
4. **刷新语义**：基线清屏用 `set_all_white`+GC16，禁 `epd_clear()`；1bit 走 binfast，
   灰阶走 scanq；局刷每 8 次插全刷重置。
5. **任务拓扑**：刷屏主控与 feed 线程同级优先级；`epd_hl_init` 只调一次。
6. **平台**：IDF 钉 5.5.3；升 IDF 前先查 `lcd_ll_*` API 存活性。

---

## 附：核心文件索引

| 内容 | 路径 |
|---|---|
| 卖家 Arduino 示例 | `Info/ED060KD1-EpdiyV7示例和操作说明/ED060KD1-EpdiyV7/{ED060KD1-EpdiyV7.ino,main.c}` |
| 卖家 epdiy2 原库 | `Info/ED060KD1-EpdiyV7示例和操作说明/操作说明/epdiy2/` |
| 卖家库只读快照（diff 基线） | `InkWord_Firmware_BigScreen/components/epdiy_seller_ref/` |
| 修复版库（生产用） | `InkWord_Firmware_BigScreen/components/epdiy/` |
| 屏/总线/电源参数 | `InkWord_Firmware_BigScreen/src/config/display_config.h` |
| 板级 GPIO 权威定义 | `InkWord_Firmware_BigScreen/src/driver/board_config.h` |
| 面板安全初始化（双路径电源） | `InkWord_Firmware_BigScreen/src/driver/panel_es108fc.c` |
| 卖家 demo 复刻（A/B 基准） | `InkWord_Firmware_BigScreen/src/test/demo/demo_seller.c` |
| 自研波形（scanq/binfast） | `InkWord_Firmware_BigScreen/src/lan/waveform_scanq.{c,h}` |
| LAN 图片链路 | `InkWord_Firmware_BigScreen/src/lan/lan_image.c` |
| 管线定稿详解 | `docs/ES108FC1C1_BIGSCREEN_BRINGUP.md` |
