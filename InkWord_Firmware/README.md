# InkWord Firmware

基于 ESP32-S3 的墨水屏英语单词学习终端固件，运行于 Arduino 框架 (espressif32@7.0.1) + GxEPD2 驱动栈。

## 硬件方案（2026-08 实测版）

| 组件 | 型号 / 规格 | 接口 |
|------|-------------|------|
| 主控 | ESP32-S3-DevKitC-1 (16MB Flash, 8MB PSRAM) | — |
| 屏幕 | DKE DEPG0370 3.7" 240×416 BW（UC8253）/ Hink E042A13-A0 4.2" 400×300 三色（SSD1619）/ GDEW027C44 同族 2.7" 264×176 三色（IL91874，24Pin，均已真机验证）/ WEIFENG WF0270 2.7" 264×176 三色（SSD1680，22Pin，待到货验证） | 4 线 SPI，多屏切换见下文「多屏切换」节 |
| 驱动板 | **EVK011-C**（现役）/ **v1.4 通用驱动板**（多屏兼容，见 §1.1） | 见 §1.2 接线图 |
| 音频 | ES8311+NS4150B CODEC（取代 MAX98357A+INMP441）✅ 播放已验收 2026-08-27 | I2C(38/39) + I2S(0/4/5/6)，MCLK=GPIO0 必接（11 录音待 BS 省线）；板载模拟麦+FPC外接麦+3W功放 |
| 存储 | MicroSD 卡 | SPI + FAT（未接线） |
| 按键 | 五向导航开关（无源，上/下/左/右/中 + SET/RST 侧键） | GPIO 独立输入（已接入，2026-08 取代 6 键） |

> 详细接线图见 [`../docs/WIRING_DIAGRAM.md`](../docs/WIRING_DIAGRAM.md)。

### 1.1 驱动板选型：EVK011-C vs v1.4

为兼容多种型号/尺寸屏幕，项目引入 **v1.4 通用驱动板** 作为多屏硬件基座，
EVK011-C 保留为 DEPG0370 对照验证板。

| 维度 | EVK011-C（现役） | v1.4 通用驱动板（多屏） |
|:---|:---|:---|
| **FPC 尺寸** | 仅 24P | **24P / 26P / 34P 三合一**（24P=3.7", 26P=2.13"/2.9", 34P=7.5"/10.3"） |
| **升压方式** | 依赖 COG GDR 输出驱动板上分立 boost | **板上自主升压**（自带振荡器，上电即工作，与 COG 型号解耦） |
| **BS 引脚** | 有（需 MCU 驱动或板侧短接 GND） | **无（板上硬接 4 线 SPI）**，省一根线 |
| **片选** | 单 CS | **CS1 + CS2**（预留双色屏/双驱动大屏） |
| **采样电阻** | 固定 | R8(0.47Ω 大屏) / R9(3Ω 小屏) 可选 |
| **适用场景** | DEPG0370 单屏开发验证 | 多型号/多尺寸屏幕兼容 |

> **迁移成本**：EVK011-C → v1.4 接线 9 线→8 线（去掉 BS），
> `gpio_config.h` 中 `EPD_BS_PIN` 改 -1，其余引脚号可保持不变。
> 升压时序简化：不再关心 COG 何时开始升压，上电即有 ±15V。

### 1.2 ESP32-S3 接线图

#### 方案 A：EVK011-C 转接板（现役，9 线）

```
  ESP32-S3           EVK011-C 转接板              DEPG0370 屏幕
  ┌────────┐         ┌───────────────┐         ┌────────────
  │        │         │           J1  ├─FPC 24p─┤ COG UC8253 │
  │ GPIO7  ├─SCK────►│ J2-3 SCK      │         │ 240x416    │
  │ GPIO8  ├─SDO────►│ J2-5 SDO      │         │ GDR→板上Q1 │
  │ GPIO9  ├─D/C────►│ J2-7 D/C#     │         │ 自主升压    │
  │ GPIO10 ├─CS─────►│ J2-6 CS       │         │ VGH +15V   │
  │ GPIO11 ├─BS─────►│ J2-10 BS      │         │ VGL -15V   │
  │ GPIO13 ├─RST────►│ J2-8 RES      │         │ VCOM       │
  │ GPIO12 │◄─BUSY───│ J2-9 BUSY     │         │            │
  │ 3V3    ├─VCI────►│ J2-16 EPAPER_VCI（唯一供电脚！）│   │
  │ GND    ├────────►│ J2-1 GND      │         │            │
  ────────┘         └───────────────         └────────────┘
```

| ESP32-S3 | EVK011 J2 | 信号 | 说明 |
|:---:|:---:|:---:|:---|
| GPIO7 | J2-3 | SCK | SPI 时钟 |
| GPIO8 | J2-5 | SDO | SPI 数据（MCU 侧 MOSI，丝印 SDO） |
| GPIO9 | J2-7 | D/C# | 命令/数据 |
| GPIO10 | J2-6 | CS | 片选 |
| GPIO11 | J2-10 | BS | 接口模式：固件驱动 LOW=4 线 SPI（⚠️ 绝不可悬空） |
| GPIO13 | J2-8 | RES | 硬复位（深睡唯一唤醒途径） |
| GPIO12 | J2-9 | BUSY | 忙信号（LOW=忙） |
| 3V3 | J2-16 | VCI | 屏幕唯一供电 3.3V |
| GND | J2-1 | GND | 共地（必须接！） |

> **升压说明**：EVK011 板上分立 boost 由屏幕 COG 从 FPC pin2(GDR) 自主驱动，
> MCU 仅向 J2-16 供 3.3V，无 GDR/RESE 信号。
> **BS 铁律**：J2-10 只允许两种状态——接 GPIO11 或板侧短接 GND，
> 绝不可悬空（漂高会进 3 线 SPI 模式，屏显条状）。

#### 方案 B：v1.4 通用驱动板（多屏兼容，8 线）

```
  ESP32-S3           v1.4 驱动板                 墨水屏（FPC 24/26/34P）
  ┌────────┐         ┌───────────────┐         ┌────────────
  │        │         │  板上自主升压   │         │            │
  │ GPIO7  ├─SCK────►│ SCK           │         │ 240x416    │
  │ GPIO8  ├─SDA────►│ SDA           │         │ (尺寸随 FPC)│
  │ GPIO9  ├─DC─────►│ DC            │         │            │
  │ GPIO10 ├─CS1────►│ CS1           │         │            │
  │ GPIO13 ├─RST────►│ RST           │         │            │
  │ GPIO12 │◄─BUSY───│ BUSY          │         │            │
  │ 3V3    ├────────►│ 3V3（板卡供电） │         │            │
  │ GND    ├────────►│ GND           │         │            │
  ────────┘         └───────────────         └────────────┘
                       │ CS2（悬空）   │  ← 双芯片屏用，单芯片屏不接
                       │ 采样电阻 R9   │  ← 小屏(2.13"~3.7")焊 3Ω
```

| ESP32-S3 | v1.4 排针 | 信号 | 说明 | 与 EVK011 对比 |
|:---:|:---:|:---:|:---|:---|
| GPIO7 | SCK | SPI 时钟 | 同 |
| GPIO8 | SDA | SPI 数据 | 同功能，命名不同（EVK011 叫 SDO） |
| GPIO9 | DC | 命令/数据 | 同（EVK011 叫 D/C#） |
| GPIO10 | CS1 | 片选 1 | 同（EVK011 叫 CS） |
| GPIO13 | RST | 硬复位 | 同（EVK011 叫 RES） |
| GPIO12 | BUSY | 忙信号 | 同 |
| 3V3 | 3V3 | 板卡供电 | 同（EVK011 叫 VCI） |
| GND | GND | 共地 | 同 |
| **—** | **BS** | — | **v1.4 无 BS 引脚**（板上硬接 4 线 SPI），省一根线 |
| — | CS2 | 片选 2 | **v1.4 新增**，双芯片屏用，DEPG0370 悬空即可 |

> **v1.4 关键差异**：
> 1. **无 BS 引脚**：板上硬接 4 线 SPI，`EPD_BS_PIN` 改 -1，GPIO11 释放给 ES8311 DOUT；
> 2. **自主升压**：板上自带振荡器，上电 3V3 即开始升压，不再依赖 COG GDR 输出，
>    升压与 COG 型号解耦，换屏不用重新调时序；
> 3. **三合一 FPC**：24P/26P/34P 排线均可接入，一块板通吃主流尺寸；
> 4. **采样电阻**：默认 R8(0.47Ω) 适配大屏，小屏(≤3.7")可改焊 R9(3Ω) 降低升压电流。
>
> **上板验证**：接 3V3 后测 FPC 座 PREVGH/PREVGL 测试点，
> PREVGH 应 >10V，PREVGL 应 < -5V（无需插屏即可验证升压电路）。

### 1.3 ES8311+NS4150B CODEC 音频模块接线（2026-08-24，取代 MAX98357A+INMP441）—— ✅ 播放链路验收通过（2026-08-27）

> 板载 ES8311 codec（I2C 寄存器配置）+ NS4150B 3W 模拟功放一体模块。
> 播放：DAC→NS4150B→喇叭（2026-08-27 人声验收通过）；录音：板载/外接模拟麦→ADC→DOUT（待 bring-up）。
> **MCLK 实线（2026-08-27 定稿，必接）**：GPIO0→MCK，REG01 bit7=0 选 MCLK 脚源，
> 与 xiaozhi-esp32 56/57 块量产板同款主流拓扑；省线方案实测不可用（见下方警示）。
> NS4150B CTRL 板载 R10 上拉常开，无 MCU 控制线。
> 供电：5V 优先（无 5V 可接 3V3，功率稍小）。
> 固件驱动：[`src/es8311.c`](src/es8311.c)（寄存器序列对齐 esp_codec_dev 1.5.6，双地址自适应）。

```
  ESP32-S3              ES8311+NS4150B CODEC           喇叭
  ┌────────┐           ┌──────────────┐          ┌───────────┐
  │        │     黄线   │ SDA          │          │ 4Ω 3W     │
  │ GPIO38 ├──────────►│              │          │           │
  │        │     白线   │ SCL          │          │           │
  │ GPIO39 ├──────────►│              │          │           │
  │        │     橙线   │ MCLK (MCK)   │          │           │
  │ GPIO0  ├──────────►│ 256×fs       │          │           │
  │        │     黄线   │ SCLK (BCLK)  │          │           │
  │ GPIO4  ├──────────►│              │          │           │
  │        │     绿线   │ LRCK (WS)    │          │           │
  │ GPIO5  ├──────────►│              │          │           │
  │        │     蓝线   │ DIN          │          │           │
  │ GPIO6  ├──────────►│              │    +──►│  +        │
  │        │     紫线   │ DOUT (ASDOUT)│    └──►│  -        │
  │ GPIO11 │◄──────────┤              │          │           │
  │        │     红线   │ 5V           │          │           │
  │ 5V     ├──────────►│              │          └───────────┘
  │        │     黑线   │ GND          │
  │ GND    ├──────────►│              │
  └────────┘           └──────────────┘
```

| ESP32-S3 引脚 | 模块信号 | 总线 | 说明 |
|:---:|:---:|:---:|:---|
| GPIO38 | SDA | I2C | I2C 数据（板载 2.2k 上拉） |
| GPIO39 | SCL | I2C | I2C 时钟（地址 0x18，CE=GND） |
| **GPIO0** | **MCLK (MCK)** | I2S | **256×fs 主时钟（2026-08-27 定稿，必接）**；BOOT strapping 脚，运行期输出安全、勿按 BOOT 键 |
| GPIO4 | SCLK (BCLK) | I2S | 位时钟（播放/录音共享） |
| GPIO5 | LRCK (WS) | I2S | 字选择 |
| GPIO6 | DIN | I2S | MCU→codec DAC（播放方向） |
| GPIO11* | DOUT (ASDOUT) | I2S | codec ADC→MCU（录音方向）；*前置 BS 省线 |
| 5V | 5V | 电源 | 模块供电（无 5V 可接 3V3） |
| GND | GND | 电源 | 共地 |

> **I2C 线长**：SDA/SCL ≤5cm（说明书要求）。
> **采样率**：播放 44.1kHz/16bit；录音 16kHz/32bit 槽（16bit 有效数据居高位，固件 `>>16` 截取）。
> **PGA 增益**：默认档 3（18dB），可调 0~7（0/6/12/18/24/30/36/42dB）。
> **⚠️ MCLK 必接（2026-08-27 bring-up 定稿）**：GPIO0→MCK 实线，勿再尝试省线——
> 曾按 esp-adf LyraT-Mini 方案省去 MCLK（REG01 bit7=1 SCLK 派生），实测嘶嘶/无声四日
> 排障；根因为驱动初版 REG00=0x00 挂起态（非省线本身），但定稿仍采用 MCLK 实线主流
> 拓扑。完整排障档案见 [`../docs/WIRING_DIAGRAM.md`](../docs/WIRING_DIAGRAM.md) §2.2。
> 若未来重评省线方案，必须在 REG00=0x80 正常态下重测。
> **DOUT 前置条件**：BS 省线后 GPIO11 释放（`EPD_BS_PIN=-1`），详见 §1.2 铁律。
> 详细接线图见 [`../docs/WIRING_DIAGRAM.md`](../docs/WIRING_DIAGRAM.md) §2.2/§2.7。
>
> **播放固件要点（2026-08-27 读音链路验收）**：
> - **采样率帧级动态切换**：MP3 按 helix 解码帧实际 samprate（8k~48k）重配
>   I2S+ES8311 时钟系数（有道源 48k / gstatic 44.1k 混库实测正常）；WAV 按头字段。
> - **I2S 恒 stereo L=R**：`i2s_configure_std` 全路径 channels=2（mono 样本复制
>   双槽）；ONLY_LEFT 单槽帧会致变调+杂音（两坑档案：曾漏改 `audio_set_sample_rate`
>   路径、`i2s_write_mono` 曾截断丢 55% 样本，均 2026-08-27 真机修复）。
> - **音量 0~100 步进 10**：驱动内 `s_volume` 唯一真相源（`dac_start` 起播回写，
>   默认 75=0xBF=0dB）；NVS `set_vol` 持久化，三入口：菜单 [系统]「音量」页
>   （上/下 ±10 即时生效+中键试听）、设置页第 10 行（中键 +10 循环；2026-08-27
>   字体三项扩展后音量行号 8→10）、RST 短按直达设置页。

---

## 引脚映射

完整定义见 [`src/gpio_config.h`](src/gpio_config.h)。

| 外设 | 引脚 | 说明 |
|------|------|------|
| **墨水屏** (EVK011 J2, 9 线 / v1.4 排针, 8 线) | | 4 线 SPI |
| — SCK | GPIO7 | SPI 时钟（两板通用） |
| — SDA/SDO | GPIO8 | SPI 数据（MCU 侧 MOSI） |
| — D/C# / DC | GPIO9 | 命令/数据 |
| — CS / CS1 | GPIO10 | 片选 |
| — BS | GPIO11 | **仅 EVK011**：接口模式选择，固件驱动 LOW=4 线 SPI（v1.4 无此脚，改 -1） |
| — RST / RES | GPIO13 | 硬复位（深睡唯一唤醒途径） |
| — BUSY | GPIO12 | 忙信号（LOW=忙） |
| — VCI/GND | 3V3 / GND | 供电与共地（必须！） |
| — CS2 | — | **仅 v1.4**：双芯片屏片选 2，DEPG0370 悬空 |
| **I2S 音频**（ES8311 CODEC，播放/录音共享） | | 标准飞利浦 I2S |
| — BCLK/SCLK | GPIO4 | 位时钟（播放/录音共享） |
| — LRCK/WS | GPIO5 | 字选择 |
| — DOUT→DIN | GPIO6 | 数据输出（MCU→codec DAC，播放方向） |
| — DOUT(ASDOUT) | GPIO11 | 数据输入（codec ADC→MCU，录音方向；BS 省线后释放该脚） |
| **I2C 控制**（ES8311 寄存器配置） | | |
| — SDA | GPIO38 | I2C 数据（板载 2.2k 上拉） |
| — SCL | GPIO39 | I2C 时钟（地址 0x18） |
| **五向导航开关** | | 无源开关，上拉输入，COM 接地，五向全 RTC 域可深睡 |
| — UP | GPIO1 | 上一条 / 长按清残影全刷 |
| — DOWN | GPIO2 | 下一条 / 长按切换学习模式 |
| — LEFT | GPIO14 | 预留（配置页光标左移/返回）/ 长按 AP 门户、密码快删 |
| — RIGHT | GPIO15 | 预留（配置页光标右移）/ 长按 LAN 接收页 |
| — CENTER | GPIO21 | 发音（配置页确认/输入；待机页拉天气；功能菜单确认）/ 长按进入功能菜单（2026-08-23 起替代 Wi-Fi 配置直达；配网页内长按=返回列表） |
| — SET | GPIO42 | 遮蔽/揭晓释义（待机页：轮换下一条引文）/ 长按收藏/取消当前词（收藏视图内取消后移出序列） |
| — RST | GPIO40 | 直达设置页（2026-08-27，原「回第一条」退役）/ 长按临时视图进出（错词本/收藏浏览） |
| **SD 卡** (SPI3_HOST) | | 独立于 EPD 的 SPI 总线 |
| — MOSI | GPIO17 | |
| — MISO | GPIO16 | |
| — SCLK | GPIO18 | |
| — CS | GPIO47 | |

> ⚠️ SD 卡引脚勿与墨水屏混淆：GPIO10/12/13 全部为 EPD 占用
> (CS/BUSY/RST)，SD 必须用 GPIO16/17/18/47。
> v1.4 方案下 GPIO11 释放（BS 硬接），已定档 ES8311 DOUT（录音数据输入）。

## 多屏切换与新屏适配（2026-08-22）

多屏兼容架构 = **面板注册表**（静态数组，`src/epd_panel.c`）+ **构建矩阵**（platformio env，编译期选默认面板）。同一份代码支持多块屏，接线完全相同（24P FPC 标准 9 线，见 §1.2），**换屏只换固件，硬件零改动**。

### 已支持面板

| 面板 | 注册名 | 规格 | 控制器 | 驱动单元 | 刷新特性 | 状态 |
|------|--------|------|--------|----------|----------|------|
| DKE DEPG0370 | `depg0370_uc8253` | 3.7" 240×416 黑白 | UC8253 | [`panels/panel_depg0370_uc8253.cpp`](src/panels/panel_depg0370_uc8253.cpp)（ops 经 [`panels/uc8253_ops.h`](src/panels/uc8253_ops.h) 族宏） | 全刷 ~1.5s / 局刷 ~0.4s | ✅ 在用 |
| Hink E042A13-A0 三色 | `e042a13_ssd1619` | 4.2" 400×300 黑白红 | SSD1619 | [`panels/panel_e042a13_ssd1619.cpp`](src/panels/panel_e042a13_ssd1619.cpp) | 全刷 ~14.6s（三色物理下限，无局刷） | ✅ 真机验证 |
| WEIFENG WF0270 | `wf0270_ssd1680` | 2.7" 264×176 黑白红（COG 竖屏 176×264 + rotation=1） | SSD1680 | [`panels/panel_wf0270_ssd1680.cpp`](src/panels/panel_wf0270_ssd1680.cpp) | 全刷 ~15s（同族估计，实测后回填），无局刷 | 🧪 待到货验证（22Pin） |
| GDEW027C44 同族 | `gdew027c44_il91874` | 2.7" 264×176 黑白红（COG 竖屏 176×264 + rotation=1） | IL91874/EK79652 | [`panels/panel_gdew027c44_il91874.cpp`](src/panels/panel_gdew027c44_il91874.cpp) | 快刷 ~4.4s（E4 LUT 压缩，实测 4361ms）+ 每 8 次插 1 次官方深刷 ~14.7s 抗残影，无局刷；RAM 须逐字节独立 CS 事务（家族铁律） | ✅ 真机验证 |
| Hink E042A13-A0 黑白版 | `e042a13bw_ssd1619` | 4.2" 400×300 黑白（LAYOUT_MID，UI 零改动即适配） | SSD1619 | [`panels/panel_e042a13bw.cpp`](src/panels/panel_e042a13bw.cpp) | 全刷 ~2.9s / 局刷 ~1.3s（两段式 + DMA，0x22/0xF7 实证） | ✅ 2026-08-30 真机验证 |
| WFT0290CZ10 | `wft0290_bw` | 2.9" 128×296 黑白竖屏（电子价签源，LAYOUT_TINY） | UC8151D（三轮实锤，GDEW029T5D 同族） | [`panels/panel_wft0290.cpp`](src/panels/panel_wft0290.cpp) | 全刷 3.4s / REG LUT 局刷 0.9s 实测；TRES 修复 | ✅ 真机验证 |
| OPM021EB | `opm021eb_bw` | 2.13" 122×250 黑白竖屏（电子标签，LAYOUT_TINY） | UC8151D（SSD1680 证伪改判，BUSY/0x2F 实锤） | [`panels/panel_opm021eb.cpp`](src/panels/panel_opm021eb.cpp) | 全刷 3.2s / 打断法快刷 0.65s（K_ABORT_MS=250 + 双写） | ✅ 真机验证 |
| 3.1" 320×240 | `panel_310_uc8253` | 3.1" 320×240 黑白（COG 横屏 + rotation=1 → UI 240×320 竖屏，MID 档首个竖屏形态，消费 `form` 形态轴） | UC8253 | [`panels/panel_310_uc8253.cpp`](src/panels/panel_310_uc8253.cpp)（同族宏） | 规格全刷 3s / 局刷 0.5s（待实测回填） | 🧪 骨架（PSR 方向待真机标定，2026-09-05） |
| HINK-E0213A31 | `hink_e0213a31_bw` | 2.13" 122×250 黑白竖屏（COG 128×250 原生 16B/行，右缘 6px 无绑定 UI 避让，LAYOUT_TINY；GxEPD2 B74 同规格） | SSD1680 | [`panels/panel_hink_e0213a31.cpp`](src/panels/panel_hink_e0213a31.cpp) | 快速全刷 0xD7 实测 1911ms（标准 3460ms 省 45%；OTP 无快刷局刷波形，全/局刷统一 0xD7） | 🧪 bring-up 进行中（P0 探针实测回填，2026-09-05） |

> **骨架/在途面板说明**（2026-09-05 更新）：10 屏注册表中 8 屏已真机
> 验证；3.1" 屏（PSR 待标定）与 HINK-E0213A31（bring-up 进行中）
> 几何/档位/UI 适配已就绪，序列按 §十六 SOP 回填 desc 即完成。WF0270
> （22Pin）屏在途。TINY 屏配网走 AP 门户（手机浏览器连 InkWord 热点），
> 屏上键盘不适用。新屏接入后开机即受 `epd_panel_desc_check()` 契约
> 校验保护（全注册表自检，违规 LOG_W / 选中屏 fail-fast）。

### 切换屏幕（一条命令）

```bash
cd InkWord_Firmware
# 切到 3.7" 黑白屏（默认 env）
~/.platformio/penv/bin/pio run -e inkword-s3 -t upload --upload-port /dev/cu.usbserial-0001
# 切到 4.2" 三色屏
~/.platformio/penv/bin/pio run -e inkword-s3-e042 -t upload --upload-port /dev/cu.usbserial-0001
# 切到 2.7" 三色屏（IL91874，24Pin，真机在用）
~/.platformio/penv/bin/pio run -e inkword-s3-gdew027c44 -t upload --upload-port /dev/cu.usbserial-0001
# 切到 2.7" 三色屏（WF0270，22Pin FPC 需转接板，待到货）
~/.platformio/penv/bin/pio run -e inkword-s3-wf0270 -t upload --upload-port /dev/cu.usbserial-0001
# 切到 4.2" 黑白屏（全刷 2.9s，比三色快 5 倍）
~/.platformio/penv/bin/pio run -e inkword-s3-e042bw -t upload --upload-port /dev/cu.usbserial-0001
# 切到 2.13" OPM021EB（TINY 档，打断法快刷）
~/.platformio/penv/bin/pio run -e inkword-s3-opm021eb -t upload --upload-port /dev/cu.usbserial-0001
```

VSCode + PlatformIO 用户：底部状态栏环境切换器选 `inkword-s3` / `inkword-s3-e042` / `inkword-s3-gdew027c44` / `inkword-s3-wf0270` 后点 Upload 等效。

**切换后自检**（串口 115200）：
- 启动日志出现 `EPD driver initialized: panel 'depg0370_uc8253' ...` 或 `panel 'e042a13_ssd1619' 400x300 dual-plane color` / `panel 'gdew027c44_il91874' 176x264 rot=1 dual-plane color` = 面板识别正确；
- 三色屏全刷时长：2.7" 快刷 ~4.4s（每 9 次含 1 次深刷 ~14.7s，日志 `FAST/DEEP LUT` 可辨）；4.2" ~14.6s（三色物理下限）；待机页文字应正常显示。

**注意事项**：
- 三色屏 UX 降级自动生效：无快速局刷（所有局刷请求自动降级全刷 ~14.6s）、待机引文自动轮换停用（SET 手动翻页保留）——固件按 desc 字段自动路由，无需手动配置；
- 两屏刷新速度差异显著属物理常态（红色粒子多相位翻转耗时），非故障；
- 烧错 env 不会损坏硬件（命令集不匹配的屏只是无显示），重烧正确 env 即可。

### 适配新屏幕（SOP）

架构设计见 [`../docs/PANEL_COMPAT_DESIGN.md`](../docs/PANEL_COMPAT_DESIGN.md)（注册表 §5.3 / bring-up SOP §十六）。三步注册：

1. **建面板单元** `src/panels/panel_<型号>.cpp`：定义 `const epd_panel_desc_t`（几何/色彩/时序/调色板/ops 函数表），驱动序列一比一移植官方 demo 或规格书（GxEPD2 无对应类时手写 SPI 序列，参见 4.2" 单元）；
2. **注册** [`src/epd_panel.c`](src/epd_panel.c)：extern 声明 + `s_registry[]` 追加一行；
3. **加 env** [`platformio.ini`](platformio.ini)：复制 `inkword-s3-e042` 段，build_flags 注入 `'-D EPD_PANEL_DEFAULT_ID="新注册名"'`（epd_panel.h 已无宏链无需改动，2026-09-03 裁剪；烧统一固件时也可免 env，经设置页 NVS 选屏运行期覆盖）。

**bring-up 铁律**（4.2" 屏实战沉淀，全部真机实证）：
- **BUSY 极性先核对**：UC8253/UC8xxx 系 LOW=忙，SSD16xx 系 HIGH=忙——判反极性会把「空闲正常态」误读为「无响应/卡死」，`desc.busy_level` 必须首验；
- **版本读验通路**：SSD16xx 读 0x2F（需 50ms 延时）→ 0x01；UC8xxx 读 0x71 → 0x02；读到预期值 = SPI 双向闭环 + COG 在场铁证；
- **ESP32-S3 `SPI.writeBytes` 批量写 RAM 静默不落地**：面板单元批量写一律用事务内 `SPI.transfer` 逐字节连发（参见 `epd_write_buf`）；
- **快刷能力由 OTP 波形库决定而非命令集**：同族控制器移植快刷序列前先跑 BUSY profile 定性（亚秒级 BUSY 释放 = 空转铁证）；
- 屏显恒定纯色 ≠ 接触不良：优先怀疑 RAM 写入未生效（先驱动内构造测试图验证写入路径，再查上层渲染管线）。

---

## 软件架构

固件采用模块化设计，每个模块对应一个 Task (F-xx)，源码位于 [`src/`](src/)。

### 模块总览

| 模块 | 文件 | 职责 |
|------|------|------|
| **主入口** | [`main.cpp`](src/main.cpp) | 设备装配（分阶段 setup）、主 loop、base 页渲染与按键编排、deck_flow_switch（巨石拆分后 601 行；渲染族迁 word_card_ui、云端编排迁 sync_session） |
| **学习词卡 UI** | [`word_card_ui`](src/word_card_ui.h) | 单词卡片/qa/poem 版式、释义分页、状态栏、pron 屏显（自 main 拆出，黄金帧基线不变） |
| **页面路由** | [`page_router`](src/page_router.h) | 页面栈 push/pop/按键分发 + 显示通道注册制（display_claim/busy，前台占用单真相源） |
| **日志** | [`debug_log`](src/debug_log.h) | 统一 LOG_I / LOG_W / LOG_E / LOG_D 宏封装 |
| **屏幕驱动** | [`epd_driver`](src/epd_driver.h) + [`epd_panel`](src/epd_panel.h) + `src/panels/*` | 多屏注册表架构：面板单元自包含驱动序列（ops 函数表），L3 渲染层（canvas 转置/双平面展开/局刷调度）面板无关；epd_gfx_* C 接口；双坐标体系（面板物理坐标 / GFX 层坐标，生效旋转派生——面板默认 gfx_rotation 可经 epd_set_rotation 运行期覆盖，设置页「屏幕方向」横/竖屏即改即生效，2026-08-26） |
| **音频播放** | [`audio_player`](src/audio_player.h) + `src/mp3/`（libhelix 内嵌） | I2S + ES8311 CODEC DAC, WAV/MP3 异步任务队列播放（提交即返、重按打断重播；P0A）；MP3 帧级采样率动态切换 8k~48k（2026-08-27），音量 0~100 经 es8311 数字音量（见 §1.3） |
| **音频同步** | [`audio_sync`](src/audio_sync.h) | 云端词条音频补齐：`{cloud_id}.mp3` 缺失串行下载（tmp+rename 防半文件），功能菜单入口 + 「缺 N/总 M」徽标（P0C） |
| **麦克风录音** | [`mic_recorder`](src/mic_recorder.h) | ES8311 ADC 全双工录音（板载/FPC 模拟麦→DOUT=GPIO11；录音期 TX 持续写静音零样本）；3s 跟读/10s 对话双档（send_now 说完即发、尾静音提前断），PSRAM 缓冲录完即释、就地组 WAV 头（P1/P2B） |
| **AI 对话** | [`chat_mode`](src/chat_mode.h) | MODE_CHAT 五态状态机（录音→上传→读流→句级流水线播放；常驻任务+触发位），录音复用 mic_recorder、播放走 audio_play_file 零新路径，三色屏降级纯语音+震动；P0-1 流式：NDJSON 增量读 + barge-in 3s 拾起（abort 截后端生成）+ replay 重播 + 老后端首行探测回退（docs/AI_CHAT_MODE.md §2b/§4） |
| **按键** | [`button_handler`](src/button_handler.h) | 五向导航开关轮询去抖, 区分短按 / 长按 (1.5s) |
| **存储** | [`storage_manager`](src/storage_manager.h) | SD 卡 SPI 挂载至 `/sdcard`, 文件读写 |
| **刷新调度** | [`refresh_scheduler`](src/refresh_scheduler.h) | 局刷计数, 达阈值例行全刷（阈值 = desc.partial_count_full_refresh × 页面系数：学习 100% / 待机 150% / 配网 125%，T1.7 系数化档位表） |
| **词库** | [`word_parser`](src/word_parser.h) | 解析 `words.json` 至 PSRAM 词池（4000 词；raw 按文件实际大小分配——2026-08-27 修复：曾固定预分配 2MB 致 SD 词库路径峰值超 8MB PSRAM、cJSON 中途 malloc 失败静默回退内嵌库，修复后 boot 提速 5.6×）；出厂内嵌兜底词库约 2400 条（`src/default_words.json` embed，无 SD 卡开箱即用，SD 卡 `words.json` 优先；生成链 [`tools/default_vocab`](../tools/default_vocab/README.md)） |
| **SRS 引擎** | [`srs_engine`](src/srs_engine.h) | FSRS-4.5 间隔重复算法（M4 路径 A 2026-08-22；纯算法，与后端 FsrsService 对拍，`pio test -e native-test`） |
| **学习状态** | [`learning_state`](src/learning_state.h) | 每词 FSRS stability/difficulty/连错/收藏；LR03 sparse NVS + 脏标记延迟落盘（旧 LR02 升级自动作废）；到期词视图（due_count/due_at，会话 done bitmap 去重）；今日统计（新学/复习次数/连续天数，NVS lr_stats UTC+8 跨日结算，2026-08-24） |
| **模式状态机** | [`study_mode_machine`](src/study_mode_machine.h) | 闪卡 / 听写 / 复习 / 阅读四模式切换（复习序列=FSRS 到期词，自评即出队）+ 听-跟一体流（云端词播完自动进跟读评测，P1）+ AI 对话临时视图 MODE_CHAT 进出（P2B） |
| **CJK 字库/文本** | [`cjk_font`](src/cjk_font.h) + [`cjk_text`](src/cjk_text.h) | 四级点阵字库 bin（16/20/24/32px，3935 字，1.13MB 嵌入；32px 级供 7.5"+ LARGE 档大屏）+ UTF-8 混排绘制层（词卡释义/tag、阅读器、待机页共用；CJK 按字断行 / ASCII 按词断，墨迹盒变宽渲染；断行量测/分页绘制 API 与绘制同源，词卡释义分页基建） |
| **Wi-Fi 联网** | [`wifi_manager`](src/wifi_manager.h) | 网络栈/STA 连接、NVS 凭据持久化、SoftAP、AP 扫描、快速+慢速断线重连、异步连接 |
| **HTTP 同步** | [`sync_client`](src/sync_client.h) + [`sync_session`](src/sync_session.h) | sync_client=纯 HTTP 客户端（增量词库拉取/回传/心跳/天气拉取附带校时）；sync_session=云端编排层（凭据装载/MAC 幂等注册/401 换钥自愈/静默心跳会话/后台任务，自 main 拆出） |
| **OTA** | [`ota_manager`](src/ota_manager.h) | 双分区升级: 下载 / 校验 / 切换 / 回滚 |
| **Wi-Fi 配置 UI** | [`wifi_config_ui`](src/wifi_config_ui.h) | 扫描列表 + QWERTY 软键盘配网向导（经功能菜单进入；独立任务+队列） |
| **快捷菜单** | [`menu_ui`](src/menu_ui.h) | 长按中键进入的功能菜单分组 12 行（2026-08-24：[学习] 收藏列表/模式选择/AI 对话、[同步] 音频同步/Wi-Fi 配网/AP 门户/LAN 接收页、[系统] 设备信息/按键说明；组头小字不可选中光标跳过）（三段式反选列表+徽标，几何按 layout_profile 档位运行期派生；回调内同步绘制，无任务无队列；设计见 [`docs/MENU_DESIGN.md`](../docs/MENU_DESIGN.md)） |
| **LAN 直传/配网门户** | [`lan_display_server`](src/lan_display_server.h) + [`lan_pages`](src/lan_pages.h) | 设备端 HTTP 服务器 + mDNS + SoftAP captive portal + DNS 劫持；内嵌网页资产（发送页/配网页）外移 lan_pages.h |
| **待机页** | [`standby_page`](src/standby_page.h) | 无词库时的《传习录》引文整页（引文独占：居中楷体 Bold 24px 点阵每 5 分钟轮换 + 右下角出处；HTTP Date+后端双校时、NVS 天气缓存；轮换默认局刷 + 差分/计数智能分流全刷防残影；深睡时钟 checkpoint/restore RTC 差分交接） |
| **电源管理** | [`power_manager`](src/power_manager.h) | SoC 深睡 + 定时唤醒（P5）：无操作 10 分钟入睡全流程、唤醒原因分流、静默心跳会话入口（见下文「电源管理」节） |

### 启动流程

```
setup() (Arduino)
  │
  ├─ 1. 日志 + NVS 初始化
  ├─ 1.5 电源分流 (P5): TIMER 唤醒 → 静默心跳会话 (校时/上报/OTA 后回睡, 不返回);
  │     中键唤醒 → 时钟 RTC 差分恢复 + 幻影按键吞除武装 (见「电源管理」节)
  ├─ 2. SD 卡挂载 + 屏幕初始化 (含 NVS 屏幕方向恢复, 幂等) + 音频 + 按键
  ├─ 3. 刷新调度器（局刷阈值 = desc.partial_count × 页面系数）
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

四种模式循环切换 (D 键切换)，模式持久化到 NVS:

| 模式 | 说明 |
|------|------|
| **闪卡** (FLASH) | 看词猜义，C 键发音 |
| **听写** (DICTATION) | 听音拼写 |
| **复习** (REVIEW) | FSRS 到期词紧凑词表（左词右义，会话去重）→中键词卡详情→左右自评即出队（2026-08-24 两态化） |
| **阅读** (READER) | SD 卡 books 目录 TXT 阅读（16/20/24/32px 四级字号，进度记忆） |

> 中键发音（P0C）：`w->audio` 人工命名词库优先，否则 `{cloud_id}.mp3`
> 云端约定（`audio_sync` 菜单同步）；缺文件短震、不回退测试音；云端词
> 播完自动进跟读评测（P1 听-跟一体流，三态屏 + 震动映射）。
> **默认词库读音（2026-08-27）**：英文 2134 条真人 MP3 已回填 `audio` 字段
> （`{slug}.mp3`，dictionaryapi.dev 优先/有道兜底，32k mono；生成链
> [`tools/default_vocab/fetch_audio.py`](../tools/default_vocab/README.md)，
> 拷贝至 SD 卡 `/sdcard/audio/` 即用）。
> **AI 对话**为第五临时视图（MODE_CHAT，P2B）：功能菜单进入，
> RST/长按中退出回闪卡，不入 D 键轮换（[`docs/AI_CHAT_MODE.md`](../docs/AI_CHAT_MODE.md)）。

释义/标签支持中文（cjk_text 16px 点阵混排：CJK 按字断行、ASCII 按词
断、超宽自动换行；真实词库释义为中文，FreeSans 仅 ASCII 不可用）；
demo 构建内置中文释义词可直接上机验证。

**释义按屏排版 + 分页 + 单列重设计（2026-08-23）**：学习页全档位
统一单列上下结构（当日两轮真机反馈迭代定稿：双栏右栏仅 136px ≈ 7
字/行阅读体验差 → 退役）——头部单词全宽大字（自适应降级）+ 音标行
（同日 IPA 修复：字库收录 IPA 21 字符 + 诗词作者名/中点，词池原始
音标 16px 点阵直渲，取代 ASCII 近似转换）+
收藏星标；正文流 = 释义+词根全宽分页（20px 字 21 字/行，行数按屏
高派生：416x240 = 4 行、400x300 = 6、264x176 = 2 行 16px）；底部
标签行全宽。超出一屏自动分页不再截断，多页时首行右缘页码指示
（如 `2/3`，正文缩窄 30px 量测绘制同宽重建页数）；上下键先词内翻
释义页、到首/尾页边界再翻词，换词/翻义/切模式页游标自动归零。
量测（`cjk_text_wrap_lines`）与分页绘制（`cjk_text_draw_wrap_page`）
共用同一断行核心，页数与渲染行严格一致（与阅读器建页同策略）。
同日修复两处存量缺陷（host 真实字库复现定位）：cjk_text ink_span
哨兵误抄致 advance 恒 3px（分页失效根因）、wrap_walk 分页绘制用
绝对行 y 致第 2 页起压底标签（页内相对 y 修正）。

**音标行 IPA 点阵渲染（2026-08-23，08-24 记号补全）**：FreeSans 字符
范围仅 0x20-0x7E，真机 IPA 音标（ə ˈ ɪ ʃ…）被逐字符静默跳过——
曾以 `phonetic_ascii` 转 ASCII 近似（ə→e 发音错位）救急，同日彻底
修复：`gen_cjk_font.swift` 字符集新增 IPA 21 字符（实测 19 + 备用
ː ɒ）+ `src/default_words.json` phonetic 列全量收集（诗词词条作者名/
中点）；PingFang 缺 IPA 全套且 CTLine 级联不可控（ˈ 渲染空白），
生成器按字符分派 STHeitiSC-Medium（真字重，21 字符全覆盖，浓度
与主链 Semibold 同级）渲染点阵；固件侧 `main.cpp` 音标行改
`cjk_text_draw` 16px 整行点阵（收藏星标同步点阵化），删除
`phonetic_ascii`；布局宏 `UI_PHON_BASE`（FreeSans 基线）→
`UI_PHON_TOP`（点阵顶左），三档 `UI_BODY_TOP` 数值不变
（104/100/74，正文区零位移）；字库 3892 → 3935 字形（+7KB），
未收录字符（如云端新字符）画 cell 空心框兜底。
08-24 真机复验补全（报障 /səˈsaɪəti/ 显为 sə saɪəti）：① ˈ ˌ
在 16px 级（12pt）字体渲染 1px 细竖笔低于二值化阈值被整体丢弃，
渲染成半角空格；生成器新增 synthModifier 合成位图，四记号直接
按级合成（ˈ 顶部竖笔 / ˌ 底部竖笔 / ː 中部双点 / · 中心方点，
笔宽 cell/8 与主链 Bold 同源），替换发生在 per-glyph 降级后、
进库前，不引入贴边；② 词典惯例斜杠包裹：词库 phonetic 为裸 IPA，
显示层条件补 `/ /`（自带 / 或 [ ] 的云端/SD 词库不双包）；
③ demo 词库换真 IPA（ˌserənˈdɪpəti 等，覆盖 ˈ ˌ ː ɪ θ）。

### FSRS-4.5 间隔重复算法（M4 路径 A，2026-08-22）

```
回忆质量 q (0~5) → rating 四档（0-1 遗忘 / 2 困难 / 3-4 良好 / 5 简单）
S/D 双记忆指标更新（open-spaced-repetition FSRS-4.5 默认 17 参数）
目标留存 0.90 → interval = round(S)
双端同源：与后端 FsrsService 共享 tools/gen_fsrs_vectors.py
对拍向量（12 序列 76 向量；S/D 相对容差 1e-5，间隔 uint16_t clamp）
```

### 词库扩容与内存布局（2026-08-20）

词库从 DRAM 静态 64 词扩容至 4000 词，三层内存全部迁 PSRAM
（`MALLOC_CAP_SPIRAM`，与阅读器书缓冲共享 8MB Octal）：

| 层 | 位置 | 容量 | 说明 |
|------|------|------|------|
| 词池 `s_word_pool` | PSRAM 堆 | 4000 × ~816B ≈ 3.3MB | setup 内按 MAX_WORDS 逐半降级分配 |
| JSON 解析缓冲 | PSRAM 堆 | 2MB | word_parser 显式 PSRAM，防配置漂移 |
| 学习状态数组 `s_state` | PSRAM 堆 | 4000 × 24B ≈ 96KB | 惰性初始化，未学词零成本 |

学习状态持久化改 **LR03 sparse 格式**（M4 FSRS 切换 2026-08-22，旧
LR02/SM-2 状态 magic 不匹配自动作废重建）：NVS blob 只存非默认态词
（学过/连错>0/已收藏，每词 21B，上限 300 活跃词 ≈ 6.3KB，配 nvs
24KB 分区；词库规模变化整体作废）。保存时机：评分/收藏仅置脏标记，
主循环 `learning_state_maybe_save()` 静默 5s 后落盘——按键路径零 NVS
阻塞，掉电窗口 ≤5s（与上报队列不持久化策略一致）。

编译实测（inkword-s3）：Flash 54.0%，DRAM 31.1%（词池迁出后静态
内存大幅下降）。

### 局部刷新方案（2026-08-20 定稿）

**无窗口整屏双 RAM 差分 + 单段直接差分**，真机验证无残影、引文切换
≈0.5s。适用于所有局刷场景：待机页引文轮换已按此实施；学习页翻词走
同一无窗口路径（`epd_gfx_flush_window` 默认双刷，速度不敏感取最稳）；
后续新增局刷场景一律按本方案实施。

**指令序列**（单次局刷完整会话，入口 `epd_gfx_flush_window_passes`）：

```
hwReset()                      硬复位 COG —— 安全：双平面全量重写，
                               不依赖 COG 内部缓存存活
initPartialDemo()              partial 波形初始化（PSR=0xD3,0x0d /
                               CDI=0x17 / E0=0x02 / E5=100，demo 原始值）
demoWriteDualNoWindow(prev,new) 整屏写双平面：0x10 旧帧 + 0x13 新帧
                               —— 不发 0x91/0x90 窗口指令
                               （SPI 20MHz，双平面 24KB ≈ 10ms）
updateDemoPartial(passes)      0x04 上电 → 0x12(+0x00 哑字节)×passes
                               → BUSY 387ms/pass → 0x02(+0x00) Power Off
```

**核心规则：**

| # | 规则 | 依据 |
|---|------|------|
| 1 | 禁用 partial window（0x91/0x90） | 窗口模式三组参数实测均不能干净刷白（0x1f 留浅影 / 0x0d 无深睡旧字不消失 / +深睡仍遮盖），与 GxEPD2 "多数 UC 面板禁用 partial window" 结论一致 |
| 2 | 前帧影子与屏幕真实内容严格一致；0x10 必须每次显式重写 | 驱动自持 `s_port_prev` 每次刷新后同步；0x10 差分基准失真即花屏/残迹。2026-08-21 实验证伪“单平面写”：0x12 后 COG 不自动 new→old，只写 0x13 → 连续局刷出现上上帧陈旧像素 |
| 3 | 单段直接差分：新帧一次写入（同 pass 允许混合方向翻转） | 黑→白单刷即净已真机验证，混合差分旧顾虑仅窗口弱波形下成立 |
| 4 | 波形耗时按次计费：387ms/pass，与面积/内容/传输频率无关 | 实测 diff 1190~4439px 恒 387ms；逐行/拆区拆刷只会更慢（每拆一次多付一轮 387ms+会话），提速唯一杠杆是减少刷新次数；SPI 提频仅省传输时间（见下节实验 A） |
| 5 | 电源终态 0x02 Power Off，不 Deep Sleep | 深睡是窗口时代补偿（+400ms），已移除 |
| 6 | 低频保养：连续 N 次局刷后例行真全刷（黑白闪烁属正常） | 学习页 N=8（`refresh_gfx_before_partial()`）；待机页 N=12（自动轮换 ≈1 小时一次） |
| 7 | 大面积变化分流全刷 | 待机页：引文带差分 > 带面积 25% 直接全刷 |
| 8 | 遗留窗口直通 API 已删（refresh_submit / epd_partial_refresh / Rect，2026-08-20） | 走 drawImagePart 窗口路径且不维护 s_port_prev，误用即违反规则 2 导致花屏；后续新增局刷一律走 epd_gfx_flush_window* |

**回退阶梯**（再现残影时逐级退，各级真机耗时）：

```
单段直接差分 387ms（现行，真机验证无残影）
  → 两段式单刷 774ms（先整带刷白再绘字，亦真机验证无残影）
  → 两段式白段双刷 1161ms（窗口时代验证版）
  → 真全刷 1796ms
```

### 速度极限与实验记录（2026-08-21 定案）

**当前 ≈0.5s 的耗时分解**：波形执行 387ms（82%，OTP 固化无 LUT 入口）+
会话开销 ~65ms（13%，复位/上电/断电等待）+ 帧传输 ~10ms（5%，SPI
20MHz）。理论地板 ≈0.42s（波形 + 最小会话），当前已达 85%；波形期间
像素持续可见翻转，感知延迟好于标称。**定案：不再对本面板做软件提速**。

**微优化实验记录**（一成一败一弃，均真机验证）：

| 实验 | 收益 | 结果 | 处置 |
|------|------|------|------|
| A：SPI 10→20MHz | -12ms | ✅ 无花屏错帧 | **保留**（回退 = 构造参数改回 10MHz） |
| B：单平面写（只写 0x13，赌 0x12 后 COG 自动 new→old） | -12ms | ❌ 残迹证伪（连续局刷出现上上帧陈旧像素） | **已回退**，结论进规则 2；demo 双写行为即正确做法 |
| C：去 hwReset（跨会话保持寄存器） | -40ms | 未试 | **放弃**：失败延迟显现（渐变型波形质量劣化，需浸泡测试）、demo 每次刷新都完整复位（行为反证）、破坏无状态架构 |

**经典优化组合判定**（PSRAM 缓存 / DMA 双缓冲 / XOR 差分 / 自定义波形）：
前三项全部作用于数据通路，而数据通路仅占 5%（Amdahl）；帧缓冲 25KB
应留内部 SRAM；无窗口线性流协议不支持 MCU 侧差分压缩。第四项（自定义
波形）是 387ms 地板的唯一真解，但 UC8253 无 LUT 寄存器。**该组合的正确
归宿是换 SSD1680 系屏（有 0x32 LUT，fast partial 波形 100~200ms）后的
下一代路线图，届时四项全部有效**。进 0.4s 以下只能换硬件。

**演进全景**（残影攻坚 + 提速两阶段）：

| 阶段 | 波形 | 切换体感 | 备注 |
|------|------|----------|------|
| 窗口模式双刷+深睡 | 1161ms + 400ms | ~1.7s | 且残影（窗口模式不可靠） |
| 无窗口 + 两段双刷 | 1161ms | ~1.3s | 残影根治（架构切换） |
| 方案 A 两段单刷 | 774ms | ~0.9s | 黑→白单刷即净验证 |
| 方案 B 单段差分 | 387ms | ~0.5s | 定稿 |
| +SPI 20MHz | 387ms | ≈0.49s | 实验 A 保留，最终形态 |

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

**《传习录》引文（点阵字库渲染；字库已升级四级 3935 字 + 32px 级，见上文）**：

- 24 条经典选句每 5 分钟轮换一条（知行合一、四句教、岩中花树等），一轮 2 小时
- 引文与字形由 [`tools/gen_cjk_font.swift`](tools/gen_cjk_font.swift) 生成
  （P3 重写：macOS CoreText 渲染，字体 Kaiti SC 优先（Bold 变体，
  不覆盖时回退 Songti SC/Heiti 等首个全字符集家族），四级 16/20/24/32px
  （16/20px 走 PingFang Bold 黑体链、24/32px 走楷体链），字符集=
  GB2312 一级 ∪ 全角标点 ∪ ASCII ∪ 引文 = 3935 字，点阵 bin 1.13MB 经
  board_build.embed_files 嵌入；两遍法实测墨迹盒自适应：Pass1 测极值
  Pass2 居中，验收硬指标：全部字形完整 + 四边 edge-touch=0），
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

**刷新策略（引文轮换按上文「局部刷新方案（2026-08-20 定稿）」实施）：**

- 引文下标 = (epoch / `STANDBY_QUOTE_INTERVAL_S`（默认 300s，可用
  `-DSTANDBY_QUOTE_INTERVAL_S=600` 等覆盖）+ SET 手动偏移) % 24，
  无状态派生，重启/校时自然对齐同一窗口
- 轮换走单段直接差分局刷（方案与规则见上文定稿节）：画布绘完整
  新帧（旧字位白 + 新字位黑）后一次局刷，波形 387ms、切换 ≈0.5s，
  真机验证无残影
- 智能分流：读回新帧与屏幕影子逐像素差分，变化像素 > 带面积 25%
  （`STANDBY_PARTIAL_MAX_DIFF_PX` 可覆盖）→ 全刷；否则局刷引文带
  [112,8,192x176)
- 低频保养 `STANDBY_PARTIAL_MAX_N=12`：连续 12 次局刷例行真全刷
- 首绘/校时跳变/配网与 LAN 页退出恢复均走全刷重建基准
- 清屏采用黑白交替深清（`epd_clear_screen`）：先全黑全刷再回白，
  洗掉长时间驻留的陈年黑迹（仅白帧全刷翻转不彻底会留浅影）
- 长按上手动清残影（黑白交替深清 + 整页重绘，与学习页同语义）

天气图标（8 个 40×40 单色位图，~1.6KB flash）与图标绘制当前未上屏，
由 [`tools/gen_weather_icons.py`](tools/gen_weather_icons.py) 生成可随时复用。

### 电源管理（P5，2026-08-21）

[`power_manager`](src/power_manager.h)：SoC 深睡 + 定时唤醒，兑现 PRD §8.1
待机功耗指标的架构前提。**双模式语义**（解决“<5µA 待机”与“待机页 5 分钟
引文轮换”的互斥）：

| 模式 | 行为 |
|------|------|
| 交互模式（现状） | loop 1s tick、引文 5 分钟轮换、后台 10 分钟心跳照常 |
| 深睡模式（新增） | 屏驻留末帧（墨水屏双稳态零功耗）；时钟/引文冻结；仅中键或 RTC TIMER 可唤醒；唤醒 = 重启走 setup 分流 |

**入睡流程**（`power_enter_sleep`，loop 周期检查超时触发，不返回）：

```
无操作 PM_SLEEP_TIMEOUT_MIN（10 分钟）且无禁睡条件
（配网 UI / LAN 接收页 / AP 门户激活时不睡）
  → standby 自治钟基准对 checkpoint 入 NVS（slp_ep0/slp_rtc0）
  → learning_state 强制落盘 → 音频/马达收口
  → epd_deep_sleep 锁 COG 电荷（画面驻留）→ wifi_radio_off
  → EPD CS gpio_hold 防总线悬浮 → ext1 中键 + RTC TIMER 挂唤醒源
  → esp_deep_sleep_start
```

**唤醒源与恢复**：

- **中键 ext1**（GPIO21，RTC 域低电平）：正常启动路径 + 20ms 确认震动
  + 自治钟 RTC 慢钟差分恢复（`epoch = 入睡基准 + (time(NULL) - rtc0)`，
  不碰已损坏的系统时钟绝对值路径；小时级睡眠误差分钟级，联网后 HTTP
  Date 校准兜底）。唤醒键幻影按键事件（按住唤醒时扫描任务零状态起步
  误报）在 on_button 吞除首个中键事件；
- **RTC TIMER**（`PM_HEARTBEAT_PERIOD_S`，2h）：**静默心跳会话**（setup
  最早期分流，不返回）：屏/SD/音频/学习状态全不初始化 → Wi-Fi 快连
  （10s 超时失败静默回睡，不重试不闪屏）→ HTTP Date 校时 → 注册/
  上报 flush/心跳/OTA 检查 → 回睡。云端设备列表保活 + 错词上报低延迟
  + OTA 低延迟，全程不碰屏；
- **冷启动/复位**：正常启动，时钟维持未同步留白等 HTTP 校准。

**关键实现约束**（详见 `power_manager.c` 头注释）：

- 本构建链（Arduino core 2.0.17 / IDF 4.4.7）S3 走传统 ext1
  （`SOC_PM_SUPPORT_EXT_WAKEUP=1`）；IDF 5.x 起 S3 ext1 被移除，P6 框架
  迁移时须换 `esp_deep_sleep_enable_gpio_wakeup` +
  `ESP_SLEEP_WAKEUP_GPIO` 判别；
- 交互期局刷后不 COG 深睡是实测定稿（缩短切换耗时）；但 SoC 深睡前
  必须补 `epd_deep_sleep()`（UC8253 波形参数依赖电荷锁定），唤醒后
  GxEPD2 首刷自动硬复位恢复 COG；
- `gpio_hold` 跨深睡存活：`power_init()`（setup 最早）负责释放锁存的
  EPD 引脚，否则 SPI 无法重新接管 CS。

**宏配置**（`power_manager.h`，build_flags 可覆盖）：

| 宏 | 默认 | 说明 |
|----|------|------|
| `PM_SLEEP_TIMEOUT_MIN` | 10 | 无操作自动入睡阈值（分钟）；上机验证可临时 -DPM_SLEEP_TIMEOUT_MIN=1 |
| `PM_HEARTBEAT_PERIOD_S` | 7200 | TIMER 心跳唤醒周期（秒）；验证可临时改 60s |
| `PM_WAKE_WIFI_TIMEOUT_S` | 10 | 静默会话 Wi-Fi 快连超时（秒，失败静默回睡） |

**上机验证清单**（与 P3 阅读模式合并一次上机会）：

- [ ] 10 分钟无操作入睡（串口日志逐步确认 checkpoint/落盘/收口/COG/射频/锁存/挂源全链）
- [ ] 睡后按中键唤醒：页面恢复 + 20ms 震动确认 + 无幻影误触发（发音/配网）
- [ ] TIMER 心跳（临时宏改 1 分钟）：静默会话校时/上报/心跳/回睡，屏全程不变；路由器离线时 10s 静默回睡
- [ ] 唤醒后待机页时钟误差 ≤ 分钟级（RTC 差分恢复）+ 联网后 HTTP Date 校准生效
- [ ] 局刷 → 入睡 → 唤醒 → 局刷/全刷链路正常（COG 深睡兼容性）
- [ ] USB 电流表实测深睡底电流（区分 SoC 级 vs 板级，记录数据供硬件断电域决策）

### Wi-Fi 配置 UI

设备提供**三种配网/换网方式**，任选其一：

| 方式 | 入口 | 适用场景 |
|------|------|----------|
| **AP 配网门户**（推荐） | 开机无凭据自动开启；或长按左键 | 首次配网 / 换网，手机自动弹页体验最佳 |
| **网页配网** | 发送页右上角“Wi-Fi 设置” | 已联网状态下直接换网，无需重启 |
| **软键盘配网** | 功能菜单（长按中键进入）→「Wi-Fi 配网」项 | 无第二台设备时的屏上向导（见下文） |

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

**软键盘配网 UI（功能菜单 → Wi-Fi 配网）：**

设备首次使用或需要更换网络时，通过屏幕引导完成 Wi-Fi 配网。

**进入方式：**
- 运行中长按中键进入功能菜单，选「Wi-Fi 配网」项（2026-08-23 前为长按中键直达；开机无凭据时已改为自动开启 AP 配网门户）

**交互流程（五向导航键）：**

```
列表页 (扫描附近 AP)
  ├─ 上/下: 选择网络
  ├─ 中:   确认 → 进入密码输入页（无网络时重新扫描）
  ├─ 左:   退出配置（长按中同效）
  │
  └─→ 密码页 (QWERTY 软键盘，横屏 416px 宽重排放大)
       ├─ 上/下: 键盘上下移动
       ├─ 左/右: 键盘左右移动
       ├─ 中:   输入字符 / 触发"OK"连接
       ├─ 长按左: 删除一个字符 / 长按中: 返回列表
       │
       └─→ 连接 → 结果页 → 自动返回学习模式
```

**软键盘三态：** 小写字母 / 大写字母 (Shift) / 数字符号 (123)

**刷新策略（2026-08-21 修版）：** 页面切换/首帧整页全刷；光标移动、
输入、删除只重绘内容区（标题栏以下）走无窗口局刷单 pass，
经 `refresh_scheduler` 计数（阈值 10 次）自动转全刷保养 ——
按键不再整屏黑白闪烁。旧版键盘按 240px 竖屏设计导致第四行与
底栏文字重叠，已按横屏 416x240 重排（字母键 36x32 居中）。

**线程模型：** 独立 `wifi_ui_task` 处理所有 UI 逻辑 (扫描/连接/刷新)，按键事件通过 FreeRTOS 队列非阻塞转发，不影响 20ms 周期的按键去抖扫描。

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
| **STA 同网直传** | 长按右键 | `http://<设备IP>/` 或 `http://inkword.local/` | 手机与设备同一路由器（正常家庭网络） |
| **AP 直连** | 长按左键 | 手机连 `InkWord-Setup` 热点 → `http://192.168.4.1/` | 路由器开了 AP 隔离 / 无路由器环境，完全不依赖路由器 |
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
  → 黑白：Floyd-Steinberg 抖动
  → 彩色（三色面板，2026-08-22）：{黑,白,红}
    RGB 最近色量化 + FS 误差扩散（WYSIWYG
    预览，文本可选红字）
  → 打包面板物理 1bpp 帧 (BW 面板 12480B /
    三色双平面 30000B, MSB first, bit=1 白/红)
  → POST /api/display    ──────→  双长度校验 → 整帧直刷 epd_full_refresh()
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
| POST | `/api/display` | 整帧位图：`epd_fb_size()` 单平面（v1，多平面面板红补零）或 `epd_fb_total()` 双平面（v2 彩色：[0]=BW bit=1 白 + [1]=红 bit=1 红）；3.7" 屏两者相等 = 12480 |
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
| 中 | 立即拉取天气 | 进入功能菜单（收藏/模式/配网/AP/LAN/信息） |
| 左 | 忽略 | 进入 AP 直连/配网门户 |
| 右 | 忽略 | 进入 LAN 接收页 |
| SET | 轮换下一条引文 | 忽略 |
| RST | 忽略 | 忽略 |

> 无词库无可翻内容，短按忽略以节省刷新次数；长按语义与学习页一致，
> 任意键退出配网/LAN 页后待机页自动整页重绘。

### 学习模式 (默认)

| 键 | 短按 | 长按 |
|----|------|------|
| 上 | 上一条（释义多页时先翻上一释义页；复习列表态=选择上一词） | 清残影全刷 |
| 下 | 下一条（释义多页时先翻下一释义页；复习列表态=选择下一词） | 切换学习模式 |
| 中 | 发音（缺音频短震；云端词播完自动进跟读；复习列表态=进词卡详情） | 进入功能菜单（收藏列表/模式选择/AI 对话/音频同步/Wi-Fi 配网/AP 门户/LAN 接收页/设备信息/按键说明，分组 [学习]/[同步]/[系统]） |
| 左 | 自评「忘记」Q1（连错+1，>0 入错词本；复习=自评出队） | 进入 AP 直连/配网门户（手机连 InkWord-Setup 热点直传） |
| 右 | 自评「简单」Q5（连错清零，错词本内移出；复习=自评出队） | 进入 LAN 接收页（同网浏览器直传） |
| SET | 遮蔽/揭晓（闪卡自测，再按切换；复习列表态忽略） | 收藏/取消当前词（已收藏词音标行右缘显 `*`；收藏视图内取消后移出序列、清空自动退回闪卡） |
| RST | 直达设置页（2026-08-27，原「回第一条」退役；复习列表态同） | 临时视图进出（错词本：连错>0 过滤，答对移出/清空退回；收藏浏览：退出回闪卡；AI 对话：退出回闪卡） |

> 遮蔽态（2026-08-24 重设计，中文提示取代英文）：闪卡族居中大问号「？」+
> 「[SET] 揭晓」；听写模式藏词与音标、画首字母+拼写空格线（听音忆拼，
> 提示「中键重播 · SET 揭晓」）；翻词/切模式后自动回全显。
> 复习词表底部提示行：「中 详情 · 左/右 自评出队 · RST 设置」；
> 详情态自评后回列表，序列清空显空态页（今日无到期词）。
> 释义分页（2026-08-23）：行数按屏高派生，超出一屏自动分页（多页时
> 右下角页码指示）；上下键先词内翻页、到边界再翻词，换词/翻义/切
> 模式页游标自动归零；SMALL 档（2.7"）为单列版式，词根随释义分页。

> 接收页 / portal 激活期间，**任意按键**退出并回到学习界面（portal 模式同时关热点回 STA）。
> 接收页与配网期间学习页渲染自动屏蔽，直刷后会强制下次全刷，无残影/花屏风险。

### 功能菜单（2026-08-23；2026-08-24 分组化）

长按中键（学习页/待机页）进入 `menu_ui`，分组 12 行 = 3 组头 + 9 项（组头
16px 小字不可选中，光标循环跳过）：**[学习]** 收藏列表（徽标=收藏数，空收藏确认长震不进入；进入与错词本同构的收藏浏览临时视图）、模式选择（二级
4 项列表，光标预定位当前模式，确认与长按下循环切换终态一致）、AI 对话
（进 MODE_CHAT 临时视图，联网 + device key 前置校验，docs/AI_CHAT_MODE.md）；
**[同步]** 音频同步（徽标「缺 N/总 M」，后台串行下载云端词条 MP3）、Wi-Fi
配网、AP 配网门户、LAN 接收页；**[系统]** 设备信息（分页：学习概况 词库/
收藏·错词/今日进度/连续学习天数 + 设备信息 版本/运行时长/IP/PSRAM，上/下
翻页）、按键说明（六组分页键位速查：学习页/复习词表/收藏·错词视图/
AI 对话/待机页/菜单内，含词卡 `*` 收藏标记含义）。

| 键 | 主菜单 | 模式列表/信息页/按键说明页 |
|----|--------|--------------------------|
| 上/下 | 移动选择（循环滚动，组头跳过，局刷） | 模式列表：移动选择；信息页：翻页（循环）；按键说明页：翻页（循环） |
| 中 | 确认/进入 | 模式列表：确认切换；信息页：下一页；按键说明页：下一页 |
| SET | 退出菜单（恢复原页面） | 返回上级 |
| RST | 任意层级直接退出回学习页/待机页 | 同左 |
| 长按 | 全部忽略（防误触） | 同左 |

> 菜单激活期间独占七键（路由插入在唤醒吞除之后、配网 UI 之前）；启动子功能
> （配网/AP/LAN/收藏）遵循「先 exit 后 enter」纪律；待机页引文轮换与渲染
> 在菜单激活期间自动让位（三条件接管早退）。几何按 layout_profile 档位
> 运行期派生：MID 4 行/SMALL 3 行/TINY 8~9 行（省提示栏与 CJK 徽标）。

### 设置页（字体三项 2026-08-27）

RST 短按直达（学习页/复习词表态同），`settings_ui` 覆盖层 10 行（menu_ui
范式）：每日新词量、发音、震动、**字号**、**单词大小**、**粗细**、测验
快答、考试倒计时、屏幕方向、音量。上/下 选择、中 切换当前项（即改即存）、
SET/RST 退出全刷回原页面、长按忽略。屏高容不下全表时自动滚动窗口（选中
行驱动 + 右上角 n/10 位置指示；wft0290 自第 10 行起由全显转滚动，机制与
MID 同路径）。

字体三项（P1a/P1b/P2）：

| 行项 | 取值循环 | 行为说明 |
|------|----------|----------|
| **字号** | 标准→大字→特大 | 正文（释义/词根/译文）px 档：MID+ 20/24/24、TINY/SMALL 16/20/20——**TINY/SMALL 特大档钳位等价大字**（窄屏 24px 每行仅 3~4 字）；阅读器无书内记忆时默认级按档 +1 起步（同钳位，书内字号记忆优先） |
| **单词大小** | 大→中→小 | 词条单词（学习页头部/测验题干）起步 24/18/14pt，超宽自动降级不变；默认「大」=历史行为 |
| **粗细** | 标准⇄加粗 | **仅英文字符**（FreeSans 路径：单词/状态栏数字/菜单 ASCII；CJK 释义点阵与 IPA 音标不受影响）；切换即时生效（量测/绘制同表一致）；中间 14pt 档加粗时降用 12pt 表（GFX 库无 14pt Bold） |

三项缺省零档（标准/大/标准）出厂视觉零变化；NVS 键 `set_font`/`set_word`/
`set_bold`（旧固件回滚读新值自然降级：特大→大字、新键不识别按缺省）。

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
│   ├── main.cpp                # 设备装配+主 loop+base 页+deck_flow_switch（巨石拆分后 601 行）
│   ├── word_card_ui.{cpp,h}    # 学习词卡/qa/poem 版式+释义分页+状态栏（自 main 拆出）
│   ├── debug_log.{c,h}         # 日志封装
│   ├── gpio_config.h           # 全局引脚映射
│   ├── page_router.{c,h}       # 页面栈路由（push/pop/dispatch + display 注册制）
│   ├── epd_driver.{cpp,h}      # L3 墨水屏驱动适配层 (epd_gfx_* C API)
│   ├── epd_panel.{c,h}         # L2 面板描述符注册表（NVS 运行期选屏 / env 编译期默认）
│   ├── panels/                 # L0/L1 面板单元（7 屏：epd_bus 族原语+电源序列+各屏序列）
│   ├── epd_geom.{c,h}          # 窗口转置/矩形钳位纯函数（native-test）
│   ├── layout_profile.{c,h}    # L4 布局档位层（短边分档 TINY~LARGE，native-test）
│   ├── GxEPD2_374_DEPG0370.{cpp,h} # GxEPD2 面板类 (DEPG0370 专用初始化序列)
│   ├── Fonts/                  # 字体 (fontconvert 生成)
│   ├── probe/                  # 屏驱探针归档（主构建排除，probe env 显式纳入）
│   ├── mp3/                    # 提示音 MP3 资源
│   ├── settings_keys.h         # NVS 键权威表（单一登记处）
│   ├── audio_player.{c,h}      # I2S 音频（ES8311 CODEC DAC，异步任务队列）
│   ├── audio_sync.{c,h}        # 云端词条音频补齐
│   ├── es8311.{c,h}            # ES8311 CODEC 驱动（I2C 寄存器，移植自 esp-adf）
│   ├── i2c_bus.{c,h}           # I2C 总线共享
│   ├── mic_recorder.{c,h}      # ES8311 ADC 录音
│   ├── chat_mode.{c,h}         # AI 对话状态机
│   ├── chat_ui.{c,h}           # AI 对话页 UI
│   ├── voice_search.{c,h}      # 语音搜词
│   ├── button_handler.{c,h}    # 按键扫描去抖
│   ├── haptic.{c,h} / ui_sfx.{c,h} # 震动/提示音反馈
│   ├── storage_manager.{c,h}   # SD 卡存储
│   ├── word_loader.{c,h}       # 词库装载
│   ├── word_parser.{c,h}       # 词库解析（native-test）
│   ├── catalog_index.{c,h}     # 目录索引（native-test）
│   ├── cjk_font.{c,h} / cjk_text.{c,h} / cjk_font_sd.{c,h} # CJK 字库/文本测量/SD 级联
│   ├── srs_engine.{c,h}        # FSRS-4.5 算法（M4，与后端对拍）
│   ├── study_mode_machine.{c,h}# 学习模式状态机
│   ├── deck_manager.{c,h}      # 词库组管理
│   ├── learning_state.{c,h}    # 学习进度持久化
│   ├── daily_plan.{c,h}        # 每日组配额（native-test）
│   ├── quiz_session.{c,h} / quiz_ui.{c,h}  # 测验会话/UI（session 为 native-test）
│   ├── card_layout.{c,h}       # 词卡版式纯函数（native-test）
│   ├── browse_mode.{c,h}       # 目录浏览模式
│   ├── review_ui.{c,h}         # 复习词表 UI
│   ├── reader_engine.{c,h}     # 阅读引擎
│   ├── menu_ui.{c,h}           # 快捷菜单（长按中键入口）
│   ├── menu_icons.h            # 菜单图标位图
│   ├── settings_ui.{c,h}       # 设置页（含运行期选屏/字号）
│   ├── standby_page.{c,h}      # 待机页（引文+天气+农历）
│   ├── lunar_calendar.{c,h}    # 农历查表（swift NSCalendar 离线生成）
│   ├── weather_icons.h         # 40×40 天气图标位图（脚本生成）
│   ├── power_manager.{c,h}     # 电源/睡眠管理
│   ├── max17048.{c,h}          # 电量计驱动
│   ├── wifi_manager.{c,h}      # Wi-Fi 联网
│   ├── wifi_config_ui.{c,h}    # Wi-Fi 配置 UI（软键盘）
│   ├── ble_provision.{cpp,h}   # BLE 配网
│   ├── lan_display_server.{cpp,h} # LAN 直传/配网门户（HTTP+mDNS+DNS 劫持；网页资产外移后 1212 行）
│   ├── lan_pages.h             # LAN 内嵌网页资产（发送页/配网页 HTML）
│   ├── lan_proto.{c,h}         # LAN 直传协议 v2 帧分类（native-test）
│   ├── sync_client.{c,h}       # 纯 HTTP 同步客户端（含天气拉取/校时）
│   ├── sync_session.{cpp,h}    # 云端同步编排层（自 main 拆出：注册/换钥/心跳/后台任务）
│   ├── ota_manager.{c,h}       # OTA 升级
│   ├── selftest_frame.{c,h} / selftest_diff.{c,h} / selftest_golden.h # 黄金帧自检（diff 为 native-test）
│   ├── refresh_scheduler.{c,h} # 刷新调度
│   ├── toolchain_stubs.c       # 工具链桩
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

音频文件放置于 `/sdcard/audio/` 目录：词条音频人工命名（words.json `audio` 字段）
优先，云端同步为 `{cloud_id}.mp3`（`audio_sync` 自动补齐）；AI 对话回复临时文件
`chat_tmp.mp3` 播完即删。

## 配置

关键配置位于 [`platformio.ini`](platformio.ini)（板型 esp32-s3-devkitc-1、
环境 inkword-s3）与 [`sdkconfig.inkword-s3`](sdkconfig.inkword-s3)。
屏幕初始化序列在 [`GxEPD2_374_DEPG0370.cpp`](src/GxEPD2_374_DEPG0370.cpp)
（PSR=0xD3 实测标定值、全刷 CDI=0x97、局刷 CDI=0x17，对照官方 demo 修正；SPI 20MHz
见构造参数，刷新方案详见「局部刷新方案」节）。

**驱动板配置**：
- EVK011-C：`EPD_BS_PIN=11`（固件驱动 LOW=4 线 SPI）
- v1.4：`EPD_BS_PIN=-1`（板上硬接 4 线 SPI，GPIO11 释放）

日志级别可通过 `menuconfig` → Component config → Log output 调整，或运行时调用 `log_set_global_level()`。

## License

Proprietary — LexInk
