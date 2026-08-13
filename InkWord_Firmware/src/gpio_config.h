/**
 * @file gpio_config.h
 * @brief 全局引脚映射定义 (Task F-02)
 *
 * 集中定义 ESP32-S3 上所有外设的 GPIO 引脚分配，避免冲突。
 * 覆盖：EPD 墨水屏、I2S 音频(MAX98357)、按键、SD 卡。
 *
 * 硬件方案约定（参考 EPDiy V7 配线 + InkWord 转接板）：
 *  - EPDiy 内部已固定占用一组引脚（见 epd Board 描述），此处仅声明应用层关注者。
 *  - 音频走标准 I2S，按键走独立 GPIO，SD 卡走 SPI。
 */
#ifndef INKWORD_GPIO_CONFIG_H
#define INKWORD_GPIO_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 墨水屏 EPD 引脚（与 EPDiy V7 默认一致，作为参考记录）
 * EPDiy 库内部使用 PCH_xxx / CONTROL_xxx，应用层一般不直接操作。
 * ============================================================ */
#define EPD_BUSY_PIN        (GPIO_NUM_48)   /**< EPD BUSY 状态 */
#define EPD_RESET_PIN       (GPIO_NUM_8)    /**< EPD 硬复位 */

/* ============================================================
 * I2S 音频输出 -> MAX98357A 功放
 * 标准飞利浦 I2S 模式
 * ============================================================ */
#define I2S_BCK_PIN         (GPIO_NUM_4)    /**< I2S BCLK  (位时钟) */
#define I2S_WS_PIN          (GPIO_NUM_5)    /**< I2S LRCK  (字选择/左右声道) */
#define I2S_DATA_OUT_PIN    (GPIO_NUM_6)    /**< I2S DOUT  (数据输出) */
#define I2S_PORT            (I2S_NUM_0)     /**< 使用的 I2S 端口 */
#define I2S_SAMPLE_RATE     (44100)         /**< 默认采样率 */
#define I2S_SAMPLE_BITS     (16)            /**< 每采样位数 */

/* ============================================================
 * 按键矩阵 / 独立按键
 * 上拉输入，按下接地（低电平有效）
 * ============================================================ */
#define BUTTON_A_PIN        (GPIO_NUM_0)    /**< KEY_A：上 / 确认 */
#define BUTTON_B_PIN        (GPIO_NUM_1)    /**< KEY_B：下 / 确认 */
#define BUTTON_C_PIN        (GPIO_NUM_2)    /**< KEY_C：输入 / 长按进入菜单 */
#define BUTTON_D_PIN        (GPIO_NUM_3)    /**< KEY_D：模式切换 / 删除 / 长按返回 */
#define BUTTON_E_PIN        (GPIO_NUM_14)   /**< KEY_E：左（Wi-Fi 配置方向键） */
#define BUTTON_F_PIN        (GPIO_NUM_15)   /**< KEY_F：右（Wi-Fi 配置方向键） */

#define BUTTON_DEBOUNCE_MS  (50)            /**< 去抖时间 */
#define BUTTON_LONG_PRESS_MS (1500)         /**< 长按判定阈值 */
#define BUTTON_SCAN_MS      (20)            /**< 扫描周期 */

/* ============================================================
 * SD 卡（SPI 模式）
 * ============================================================ */
#define SD_SPI_HOST         (SPI2_HOST)
#define SD_MOSI_PIN         (GPIO_NUM_11)
#define SD_MISO_PIN         (GPIO_NUM_13)
#define SD_SCLK_PIN         (GPIO_NUM_12)
#define SD_CS_PIN           (GPIO_NUM_10)
#define SD_MOUNT_POINT      "/sdcard"       /**< 文件系统挂载点 */
#define SD_MAX_FREQ_KHZ     (20000)         /**< SPI 时钟上限 */

/* ============================================================
 * LED / 其他
 * ============================================================ */
#define LED_STATUS_PIN      (GPIO_NUM_2)    /**< 板载状态指示灯（低电平点亮） */

/* 挂载点与音频目录 */
#define AUDIO_DIR           "/sdcard/audio"

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_GPIO_CONFIG_H */
