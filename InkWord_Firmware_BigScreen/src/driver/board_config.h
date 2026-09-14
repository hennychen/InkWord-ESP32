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
// 方案：沿用现有小屏板已验证的 ES8311+NS4150B 组合，无 MCLK 省线模式
// （SCLK 派生时钟）。GPIO22-25 为全板唯一连续空闲四连号（epdiy 库零
// 引用、非 strapping、非模组 flash/PSRAM 区 26-37），物理相邻走线集中，
// 降低 SCLK/LRCK 接反风险（小屏板实测教训）。
// I2C 复用 I2C_NUM_0（BS_I2C_SDA/SCL=39/40），ES8311 地址 0x18 与屏侧
// 0x48/0x20 无冲突；NS4150B 使能默认上拉（模块板），如需软控用 GPIO3。
#define BS_I2S_BCLK   22   // ES8311 SCLK/BCLK
#define BS_I2S_LRCK   23   // ES8311 LRCK/WS
#define BS_I2S_DSDIN  24   // MCU DOUT → codec DSDIN（播放）
#define BS_I2S_DSDOUT 25   // codec DSDOUT → MCU DIN（录音）
// 备用条件空闲脚：GPIO3（strapping JTAG 源，启动后可作 PA 使能）、
// GPIO19/20（USB 占用则不可用）、GPIO43/44（默认 UART0 日志，慎占）

// ---------- [待定] 五向按键（大屏板按键方案未定） ----------
// TODO(Phase 3 P0): 原理图确认后补充按键 GPIO 定义（沿用五向导航交互）
