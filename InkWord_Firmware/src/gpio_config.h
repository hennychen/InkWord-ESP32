/**
 * @file gpio_config.h
 * @brief 板级引脚映射定义（板级轴：EVK011-C 现役 / v1.4 通用板）
 *
 * 硬件：ESP32-S3 + 转接板 + DEPG0370BBU253F33HP-M7 3.7" 墨水屏
 * 接口：4 线 SPI（BS1=LOW）
 *
 * 升压架构（2026-08 原理图重建结论，两板均无 MCU 信号职责）：
 *   - EVK011 板上分立 boost（Q1 SI1308EDL + L1 47uH + MBR0503）由屏幕 COG
 *     从 FPC pin2(GDR) 自主驱动，MCU 唯一电源职责是供 VCI 3.3V；
 *   - v1.4 通用板板载自主升压（解耦 COG 时序，上电即工作），同样
 *     不输出任何 GDR/RESE 信号。
 *
 * EVK011 J2 排针对应：SCK=pin3, SDO=pin5, D/C#=pin7, RES=pin8, BUSY=pin9,
 *             BS=pin10, CS=pin6, VCI=pin16(3.3V 供电)
 */
#ifndef INKWORD_GPIO_CONFIG_H
#define INKWORD_GPIO_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 板级轴选择（PANEL_COMPAT_DESIGN.md §七，Phase 0）
 * 两板固件可感知差异仅 BS 一项：EVK011 需 GPIO 驱动 LOW 选 4 线 SPI，
 * v1.4 板上硬接固定 4 线（无此线）；其余 EPD 信号两板引脚相同。
 * v1.4 其余差异（24/26/34P 三合一座子、双 CS 预留、0.47R/3R 可选
 * 采样电阻、板载自主升压）均为硬件属性，固件无感。
 *
 * 切换方式（二选一，优先级：构建期 > 文件内）：
 *   1. 构建期 -D INKWORD_BOARD_V14=1（Phase 3 构建矩阵 env 落地）；
 *   2. 上机验证临时取消下行注释（提交前必须还原为 EVK011）。
 * 未定义任何板宏时默认 EVK011（现役，保证既有 env 零改动）。
 * ============================================================ */
/* #define INKWORD_BOARD_V14 1 */ /* ← v1.4 上机验证时临时启用 */
#if defined(INKWORD_BOARD_V14)
#define INKWORD_BOARD_NAME   "v1.4"
#elif defined(INKWORD_BOARD_EVK011)
#define INKWORD_BOARD_NAME   "EVK011-C"
#else
#define INKWORD_BOARD_EVK011 1     /* 默认板：EVK011-C */
#define INKWORD_BOARD_NAME   "EVK011-C"
#endif

/* ============================================================
 * 墨水屏 EPD 引脚（4 线 SPI 模式，两板共用段）
 * BS1=LOW 选 4 线 SPI（DEPG0370 规格 Note5-5）
 * 注：无 GDR/RESE 定义 —— 升压两板均自主驱动（见文件头）
 * ============================================================ */
#if defined(INKWORD_BOARD_V14)
/* v1.4 通用板：BS 板上硬接 4 线 SPI（该线不存在，-1 语义即“已去线”，
 * epd_driver_init 条件编译跳过驱动）；CS2 为双芯片预留，单芯片场景
 * 悬空（无 GPIO 分配，固件不定义） */
#define EPD_BS_PIN          (-1)
#else
/* BS 省线方案已实施（2026-08-26，ES8311 DOUT 接管 GPIO11）：
 * 转接板侧 J2-10 已短接 GND（就近接 J2-1），本项目只用 4 线 SPI；
 * 释放出的 GPIO11 已定档 ES8311 DOUT（录音数据输入，见录音段） */
#define EPD_BS_PIN          (-1)    /**< Boot Select：11=J2-10 接 GPIO11，固件驱动 LOW 选 4 线 SPI；-1=已去线（板侧短接 GND / v1.4 板上硬接） */
#endif /* INKWORD_BOARD_V14 */
#define EPD_SCK_PIN         (7)     /**< SPI 时钟 SCK (J2 pin3) */
#define EPD_MOSI_PIN        (8)     /**< SPI 数据 SDO/MOSI (J2 pin5) */
#define EPD_DC_PIN          (9)     /**< 数据/命令选择 (J2 pin7) */
#define EPD_CS_PIN          (10)    /**< 片选 (J2 pin6) */
#define EPD_BUSY_PIN        (12)    /**< 忙信号输入 (J2 pin9) - LOW=忙 */
#define EPD_RESET_PIN       (13)    /**< 硬复位 (J2 pin8) */

/* ============================================================
 * I2S 音频（ES8311+NS4150B CODEC 模块，2026-08-24 取代 MAX98357A）
 * 播放/录音共用 I2S0 四线（模块排针 SCLK/LRCK/DIN/DOUT）；
 * codec 寄存器经 I2C 配置（见 ES8311 段）；NS4150B CTRL 板载
 * R10 上拉常开，无 MCU 控制线（关功放需挪 R10→R11 焊盘）
 * ============================================================ */
#define I2S_BCK_PIN         (4)     /**< I2S BCLK/SCLK (位时钟) → 模块 SCLK */
#define I2S_WS_PIN          (5)     /**< I2S LRCK  (字选择/左右声道) → 模块 LRCK */
#define I2S_DATA_OUT_PIN    (6)     /**< I2S DOUT (数据输出) → 模块 DIN（codec DAC 侧） */
#define I2S_SAMPLE_RATE     (44100) /**< 默认采样率 */
#define I2S_SAMPLE_BITS     (16)    /**< 每采样位数 */

/* ============================================================
 * ES8311 codec（I2C 寄存器配置 + 录音数据线，2026-08-24）
 * 模块：ES8311+NS4150B CODEC（板载模拟麦 + FPC 外接麦 + 3W 功放，
 * 5V 供电；无 5V 可接 3V3 功率稍小）。38/39 自「I2C 预留」正式定档。
 * I2C 地址：7bit 0x18（CE 脚低电平接法，原理图默认；若实测 NACK
 * 可能焊选 0x19——es8311_probe 双地址自适应）。
 * MCLK 主流拓扑（2026-08-27 定档）：xiaozhi-esp32 57 块 ES8311
 * 量产板 56 块用 MCLK 实线（256×fs）；GPIO45 尝试全静音系当时
 * 接线未知错误环境下测的无效样本。legacy 驱动 mck_io_num=0
 * 时 GPIO0 始终在输出 256×fs MCLK——ES8311_MCLK_PIN=0 后
 * codec 切 MCLK 脚源（REG01 bit7=0），系数表直接命中。
 * GPIO0 为 BOOT strapping 脚：运行期作 MCLK 输出安全，勿按 BOOT 键。
 * ============================================================ */
#define ES8311_I2C_NUM      (0)     /**< I2C 控制器（本项目唯一 I2C 主设备） */
#define ES8311_I2C_SDA_PIN  (38)    /**< → 模块 SDA（板载 2.2k 上拉） */
#define ES8311_I2C_SCL_PIN  (39)    /**< → 模块 SCL */
#define ES8311_I2C_FREQ_HZ  (100000)
#define ES8311_I2C_ADDR     (0x18)  /**< 7bit（CE=GND）；探测自适应 0x19 */
#define ES8311_MCLK_PIN     (0)     /**< 0=GPIO0 输出 256×fs（主流拓扑）；-1=SCLK 派生（实测嘶嘶不可用） */

/* ============================================================
 * 录音数据线（ES8311 ADC → MCU；原 INMP441 方案 2026-08-24 废弃）
 * 全双工共享时钟：SCLK/WS 与播放共 GPIO4/5，仅数据线区分方向；
 * DOUT 首选 GPIO11，前置条件：BS 省线（J2-10 板侧短接 GND、
 * EPD_BS_PIN 改 -1 重烧）；未省 BS 线时备选 GPIO38/39 已被 I2C
 * 占用（ES8311 方案下无备选，必须省 BS 线）。
 * ⚠ AI_SPEECH_ASSESSMENT §3.1 早期建议的 GPIO7 已被 EPD_SCK_PIN
 * 占用（WIRING 全 GPIO 冲突校验勘误），不可用。
 * ============================================================ */
#define I2S_DATA_IN_PIN     (11)    /**< ES8311 DOUT(ASDOUT) → MCU 输入 */

/* ============================================================
 * 五向导航按键（无源开关，2026-08 取代 6 独立按键方案）
 * 上拉输入，COM 接 GND，按下接地（低电平有效）
 * 选脚原则：避开 strapping（GPIO0/3），五脚全落 RTC 域，
 *          支持 ext1 深睡唤醒；模块丝印变体 MID/OK 同义
 * SET/RST 为模块上两个额外侧键（与五向共用 COM，同为无源触点，
 * PDF 明示无固定功能由程序自定义）；实际接线（2026-08-18 确认）：
 * SET→GPIO42，RST→GPIO40，振动马达预留从 40 改至 41，
 * 保住 38/39（I2C 电量计/RTC 预留）
 * ============================================================ */
#define NAV_UP_PIN          (1)     /**< 上：模块丝印 UP */
#define NAV_DOWN_PIN        (2)     /**< 下：模块丝印 DOWN */
#define NAV_LEFT_PIN        (14)    /**< 左：模块丝印 LEFT */
#define NAV_RIGHT_PIN       (15)    /**< 右：模块丝印 RIGHT */
#define NAV_CENTER_PIN      (21)    /**< 中：模块丝印 CENTER/MID，兼深睡唤醒 */
#define NAV_SET_PIN         (42)    /**< SET 侧键：确认/翻义（待机页=轮换引文；
                                        长按=局刷波形参数 A/B 切换） */
#define NAV_RST_PIN         (40)    /**< RST 侧键：回到第一条；
                                        长按=局刷/全刷策略切换（残影定位工具） */

#define BUTTON_DEBOUNCE_MS  (50)    /**< 去抖时间 */
#define BUTTON_LONG_PRESS_MS (1500) /**< 长按判定阈值 */
#define BUTTON_SCAN_MS      (20)    /**< 扫描周期 */

/* ============================================================
 * SD 卡（SPI 模式）- 独立于 EPD SPI
 * ============================================================ */
#define SD_MOSI_PIN         (17)
#define SD_MISO_PIN         (16)
#define SD_SCLK_PIN         (18)
#define SD_CS_PIN           (47)
#define SD_MOUNT_POINT      "/sdcard"
#define SD_MAX_FREQ_KHZ     (20000)

/* ============================================================
 * 震动马达（PRD 5.4 触觉反馈，2026-08-20 接入）
 * 直流震动马达模块 + 外接 MOS 管驱动，GPIO 开关量（无调速）；
 * 原预留 40，2026-08-18 RST 侧键接线后让位定档 41
 * （保住 38/39 I2C 预留，见按键区注释）。
 * 活动电平：常见模块高有效；低有效模块（板载上拉+NMOS）
 * 在 haptic.c 改 HAPTIC_ACTIVE_HIGH 为 0
 * ============================================================ */
#define HAPTIC_PIN          (41)    /**< 震动马达使能（MOS 管） */

/* ============================================================
 * LED / 其他
 * ============================================================ */
/* 注意：LED_STATUS_PIN(14) 与 NAV_LEFT_PIN 重叠 —— 禁止使用，
 * 仅历史兼容保留定义；状态灯应使用 GPIO48 板载 WS2812 */
#define LED_STATUS_PIN      (14)

/* 挂载点与音频目录 */
#define AUDIO_DIR           "/sdcard/audio"

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_GPIO_CONFIG_H */
