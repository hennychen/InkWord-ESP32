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
#define EPD_BS_PIN          (-1)    /**< Boot Select：-1=已去线（J2-10 板侧短接 GND）；>0 时固件驱动 LOW 选 4 线 SPI */
#define EPD_SCK_PIN         (7)     /**< SPI 时钟 (J2 pin3) */
#define EPD_MOSI_PIN        (8)     /**< SPI 数据 MOSI (J2 pin5) */
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
 * 独立按键
 * 上拉输入，按下接地（低电平有效）
 * ============================================================ */
#define BUTTON_A_PIN        (0)     /**< KEY_A：上 / 确认 */
#define BUTTON_B_PIN        (1)     /**< KEY_B：下 */
#define BUTTON_C_PIN        (2)     /**< KEY_C：发音 / 长按进入 Wi-Fi 配置 */
#define BUTTON_D_PIN        (3)     /**< KEY_D：模式切换 / 长按清残影 */
#define BUTTON_E_PIN        (14)    /**< KEY_E：左（Wi-Fi 配置方向键） */
#define BUTTON_F_PIN        (15)    /**< KEY_F：右（Wi-Fi 配置方向键） */

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
/* 注意：LED_STATUS_PIN(14) 与 BUTTON_E_PIN 重叠，KEY_E 已启用 —— 禁止使用，
 * 仅历史兼容保留定义；状态灯如需启用应另选空闲 GPIO */
#define LED_STATUS_PIN      (14)

/* 挂载点与音频目录 */
#define AUDIO_DIR           "/sdcard/audio"

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_GPIO_CONFIG_H */
