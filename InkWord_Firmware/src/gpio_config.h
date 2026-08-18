/**
 * @file gpio_config.h
 * @brief EVK011 转接板引脚映射定义
 *
 * 硬件：ESP32-S3 + EVK011 升压转接板 + DEPG0370BBU253F33HP-M7 3.7" 墨水屏
 * 接口：4 线 SPI（BS1=LOW）
 *
 * 升压架构（2026-08 原理图重建结论）：
 *   EVK011 板上分立 boost（Q1 SI1308EDL + L1 47uH + MBR0503）由屏幕 COG
 *   从 FPC pin2(GDR) 自主驱动 —— MCU 不输出 GDR/RESE 信号，
 *   唯一电源职责是向 J2-16 (EPAPER_VCI) 供 3.3V。
 *
 * J2 排针对应：SCK=pin3, SDO=pin5, D/C#=pin7, RES=pin8, BUSY=pin9,
 *             BS=pin10, CS=pin6, VCI=pin16(3.3V 供电)
 */
#ifndef INKWORD_GPIO_CONFIG_H
#define INKWORD_GPIO_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * EVK011 墨水屏 EPD 引脚（4 线 SPI 模式）
 * BS1=LOW 选 4 线 SPI（DEPG0370 规格 Note5-5）
 * 注：无 GDR/RESE 定义 —— 升压由屏幕 COG 自主驱动（见文件头）
 *
 * 省线方案（BS，唯一可去线）：本项目只用 4 线 SPI，BS 永远为 L。
 * 在转接板侧把 J2-10 短接 GND（就近接 J2-1）即可去掉这根线，
 * 并把 EPD_BS_PIN 改为 -1（epd_driver_init 已做条件编译保护）；
 * 释放出的 GPIO11 可改作他用（如状态灯，解决与 KEY_E 的 14 脚冲突）
 * ============================================================ */
#define EPD_BS_PIN          (11)    /**< Boot Select：11=J2-10 接 GPIO11，固件驱动 LOW 选 4 线 SPI；-1=已去线（板侧短接 GND） */
#define EPD_SCK_PIN         (7)     /**< SPI 时钟 SCK (J2 pin3) */
#define EPD_MOSI_PIN        (8)     /**< SPI 数据 SDO/MOSI (J2 pin5) */
#define EPD_DC_PIN          (9)     /**< 数据/命令选择 (J2 pin7) */
#define EPD_CS_PIN          (10)    /**< 片选 (J2 pin6) */
#define EPD_BUSY_PIN        (12)    /**< 忙信号输入 (J2 pin9) - LOW=忙 */
#define EPD_RESET_PIN       (13)    /**< 硬复位 (J2 pin8) */

/* ============================================================
 * I2S 音频输出 -> MAX98357A 功放
 * 标准飞利浦 I2S 模式
 * ============================================================ */
#define I2S_BCK_PIN         (4)     /**< I2S BCLK  (位时钟) */
#define I2S_WS_PIN          (5)     /**< I2S LRCK  (字选择/左右声道) */
#define I2S_DATA_OUT_PIN    (6)     /**< I2S DOUT  (数据输出) */
#define I2S_SAMPLE_RATE     (44100) /**< 默认采样率 */
#define I2S_SAMPLE_BITS     (16)    /**< 每采样位数 */

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
