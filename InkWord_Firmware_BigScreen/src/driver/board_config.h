#pragma once
// ============================================================
// board_config.h — 大屏板 GPIO 权威定义（仅本项目可见）
// 方案 §3.1；与 InkWord_Firmware 的 GPIO 定义零交叉（物理隔离红线）。
//
// 引脚来源与可信度：
//   [库镜像] EPD/I2C 引脚 = epdiy 2.0.0 官方 V7 板定义（勿手改库）
//            来源 components/epdiy/src/board/epd_board_v7.c；
//            方案 §3.1 原文 STH=42/LE=41 与库相反，已按库源码修正
//            为 STH=41/LEH=42（STH 水平起始脉冲、LEH 锁存）。
//   [待核对] SD/按键为卖家板扩展（方案原文），未见原理图实测，
//            bring-up 时以 I2C 扫描/点屏结果为准修正。
//
// 分区规则：
//   EPD_*  — epdiy 库内部驱动（信息镜像，便于排障对照），禁止他用；
//   BS_*   — 本项目自管外设（I2C/SD/按键），本项目内权威。
// ============================================================

// ---------- [库镜像] EPD 16bit 并行数据线（禁止用于其他用途） ----------
#define EPD_PIN_D0   5   // 库 D0
#define EPD_PIN_D1   6   // 库 D1
#define EPD_PIN_D2   7   // 库 D2
#define EPD_PIN_D3   15  // 库 D3
#define EPD_PIN_D4   16  // 库 D4
#define EPD_PIN_D5   17  // 库 D5
#define EPD_PIN_D6   18  // 库 D6
#define EPD_PIN_D7   8   // 库 D7
#define EPD_PIN_D8   9   // 库 D8
#define EPD_PIN_D9   10  // 库 D9
#define EPD_PIN_D10  11  // 库 D10
#define EPD_PIN_D11  12  // 库 D11
#define EPD_PIN_D12  13  // 库 D12
#define EPD_PIN_D13  14  // 库 D13
#define EPD_PIN_D14  21  // 库 D14
#define EPD_PIN_D15  47  // 库 D15

// ---------- [库镜像] EPD 控制信号（禁止用于其他用途） ----------
#define EPD_PIN_CKH  4   // 水平移位时钟
#define EPD_PIN_CKV  48  // 垂直时钟
#define EPD_PIN_STH  41  // 水平起始脉冲（方案原文误作 42，已修正）
#define EPD_PIN_LEH  42  // 锁存使能（方案原文误作 41，已修正）
#define EPD_PIN_STV  45  // 垂直起始脉冲

// ---------- [库镜像] I2C：TPS65185 PMIC + PCA9555 IO 扩展 ----------
// 地址：TPS65185=0x48、PCA9555=0x20（bring-up 步骤 2 扫描验证）
#define BS_I2C_SDA   39
#define BS_I2C_SCL   40
#define BS_I2C_PORT  0    // I2C_NUM_0（库 EPDIY_I2C_PORT 同端口）
// GPIO38 冲突记录（方案 §3.1 自知项，分时复用策略）：
//   I2C_INT(38) 与 SD_CMD(38) 共脚。bring-up 及屏驱动阶段 I2C_INT 优先
//   （PMIC 中断是屏命脉，SD 不启用）；SD 启用前置条件 = PCA9555 中断
//   改轮询并释放 38 脚，届时在此补充复用切换函数，禁止双活。
#define BS_I2C_INT   38   // TPS65185 中断（输入，当前占用，禁止 SD 复用）

// ---------- [卖家板] 屏幕电源使能（epdiy2 魔改版实证，2026-09-12） ----------
// 卖家 epdiy2 的 V7 板驱已注释 I2C PMIC 全链路（pca9555/tps 写全注释、
// set_vcom 空壳、PG 等待循环注释），配套 .ino 以 GPIO46 直控电源使能
// （pinMode(46,OUTPUT) + digitalWrite(46,1)=上电）；lcd_driver.c 亦将
// 46 配置为输出（DEBUG_PIN）。高置信度：46=电源使能，高电平=供电。
// bring-up 步骤 2/3 将实测 PMIC 在线性，双路径电源见 panel_es108fc.c。
#define BS_EPD_POWER_EN 46   // 屏幕电源使能（高=开），禁止挪作他用

// ---------- [待核对] TF 卡（SPI 模式，bring-up 阶段不启用） ----------
#define BS_SD_CLK    2
#define BS_SD_CMD    38   // ⚠ 与 BS_I2C_INT 共脚，见上方冲突记录
#define BS_SD_DAT0   1
#define BS_SD_CS     0

// ---------- [预留] 音频模块 ES8311 + NS4150B（P3，引脚已规划） ----------
// 方案：沿用小屏板已验证组合（gpio_config.h 2026-08-27 定档）——MCLK
// 必须独立实线输出 256×fs（省线 SCLK 派生模式小屏实测嘶嘶声不可用；
// xiaozhi-esp32 56/57 块量产板同用 MCLK 实线主流拓扑）。GPIO22-25 为
// 全板唯一连续空闲四连号（epdiy 库零引用、非 strapping、非模组
// flash/PSRAM 区 26-37），物理相邻走线集中，降低 SCLK/LRCK 接反
// 风险（小屏板实测教训：接反→DAC 帧错位持续气流声）。
// I2C 复用 I2C_NUM_0（BS_I2C_SDA/SCL=39/40），ES8311 地址 0x18（CE=GND，
// NACK 自适应 0x19）与屏侧 TPS65185=0x48、PCA9555=0x20 无冲突——
// 三方共线：audio 驱动 init 勿重复装 i2c driver 同端口（epdiy 已装）。
// MCK=GPIO3（strapping JTAG 源选择脚，默认上拉启动安全；运行期输出
// 无冲突——模块 MCK 为高阻输入不会拉低它，复位重采样亦安全）。
// NS4150B 使能板载 R10 上拉常开（小屏同款模块），无 MCU 控制线；
// 如需软控挪 R10→R11 焊盘后用富余脚。模块 5V 供电（无 5V 可 3V3
// 功率稍小，小屏实测）。
#define BS_I2S_BCLK   22   // ES8311 SCLK/BCLK
#define BS_I2S_LRCK   23   // ES8311 LRCK/WS
#define BS_I2S_DSDIN  24   // MCU DOUT → codec DSDIN（播放）
#define BS_I2S_DSDOUT 25   // codec DSDOUT → MCU DIN（录音）
#define BS_I2S_MCLK   3    // 独立 256×fs（省线派生实测嘶嘶不可用）
// 备选（弃 SD 三脚 0/1/2 后）：MCK 可挪 GPIO0（BOOT 脚运行期输出小屏
// 实证安全）或 1/2，富余脚作 NS4150B 软使能；GPIO43/44（UART0 日志）
// 不动。
// [2026-09-16 勘误] 原注「GPIO19/20=USB D+/D- 不动」系卖家开发板假设；
// 实板为自绘 PCB，GPIO19 已定为按键 ADC 检测脚（卖家告知），USB
// 走线以自绘原理图为准，旧假设作废。

// ---------- [自绘板实证] 三按键 ADC 检测（2026-09-16 卖家告知） ----------
// 自绘 PCB 三按键，分压网络汇总到 GPIO19（ESP32-S3 = ADC2_CH8）
// 单脚多键：不同按键串不同分压电阻，ADC 原始值落在不同窗口。
// 电气持征（典型）：无键=上拉满量程（~4095）；按键各自拉到中低段。
// 窗口分界为占位初值，必须经串口 'a' 诊断命令实测后修正——
// 实测方法：烧录后串口输入 a，分别按下三键读原始值，按段间中线填宏。
// ADC2 与 WiFi 互斥（记忆实证）：仅 BIGSCREEN_APP 使用本驱动，LAN
// 固件不编入 button_handler，无冲突。
#define BS_BTN_ADC_GPIO   19   // ADC2_CH8（自绘板按键分压汇总脚）
#define BS_BTN_ADC_TH_NONE 3400  // ≥此值=无键按下（实测：丝印01≈2731，无键=4095，中线3400）
#define BS_BTN_ADC_TH_1_2 1460  // <此值=键1；[TH_1_2, TH_2_3)=键2（实测：丝印03≈957，丝印02≈1962，中线1460）
#define BS_BTN_ADC_TH_2_3 2350  // [TH_2_3, TH_NONE)=键3（实测：丝印02≈1962，丝印01≈2731，中线2350）
// 三物理键→导航键映射（可配；长按语义由 app_main 编排层定义）
#define BS_BTN_KEY1  NAV_UP      // 键1（低段）：短=上翻/上一词，长=全刷清屏
#define BS_BTN_KEY2  NAV_DOWN    // 键2（中段）：短=下翻/下一词，长=切模式
#define BS_BTN_KEY3  NAV_CENTER  // 键3（高段）：短=确认（发音桩），长=菜单桩
// 遗留：BS_NAV_* 七键 GPIO 直连方案作废（原 -1 待定表删除）；
// 若后续自绘板加独立 GPIO 键，恢复直连分支再配。
