/**
 * @file epd_driver.h
 * @brief 墨水屏驱动封装 (Task F-05 ~ F-09)
 *
 * 基于 EPDiy V7 库，封装屏幕初始化/版本检测、全刷、局刷与深度休眠。
 * 屏幕型号：ED097TC2（9.7 寸，1200x825，横屏使用）。
 */
#ifndef INKWORD_EPD_DRIVER_H
#define INKWORD_EPD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕几何参数（ED097TC2 横屏） */
#define EPD_WIDTH       (1200)
#define EPD_HEIGHT      (825)

/**
 * @brief 初始化墨水屏与 EPDiy 底层。(F-05/F-06)
 * @return 0 成功；非 0 表示失败码。
 */
int epd_driver_init(void);

/**
 * @brief 上电（开启负高压生成电路）。刷新前需先上电。
 */
void epd_power_on(void);

/**
 * @brief 下电（关闭高压电路，降低静态功耗）。
 */
void epd_power_off(void);

/**
 * @brief 读取屏幕厂商 ID / 版本。(F-06)
 * @param[out] manufacturer 厂商字符串缓冲（至少 32 字节）。
 * @return 0xFFFF 表示读取失败；否则返回有效 ID。
 */
uint16_t epd_get_manufacturer(char *manufacturer, size_t len);

/**
 * @brief 全屏刷新（Full Refresh）。(F-07)
 *        清除残影，耗时较长（约 1~2 秒），适合 Logo/切换大画面。
 * @param framebuffer 全屏像素缓冲（EPD_WIDTH/8 * EPD_HEIGHT 字节，1=黑）。
 */
void epd_full_refresh(const uint8_t *framebuffer);

/**
 * @brief 全屏清白。
 */
void epd_clear_screen(void);

/**
 * @brief 局部刷新（Partial Refresh）。(F-08)
 *        仅更新指定矩形区域，速度快（<1 秒），适合翻单词。
 * @param x,y,w,h 目标矩形（像素）。
 * @param data    该区域的像素缓冲，行字节对齐 = w/8 向上取整。
 */
void epd_partial_refresh(int x, int y, int w, int h, const uint8_t *data);

/**
 * @brief 进入深度休眠，功耗降至极低。(F-09)
 *        唤醒需重新调用 epd_driver_init()。
 */
void epd_deep_sleep(void);

/**
 * @brief 获取内部帧缓冲指针（供 EPDiy highlevel API 绘图）。
 *        布局：1bit/pixel，1=黑 0=白，共 EPD_WIDTH/8*EPD_HEIGHT 字节。
 */
uint8_t *epd_get_framebuffer(void);

/* framebuffer 像素颜色宏（用于 EPDiy 绘图函数的 color 参数）
 * EPDiy 4bpp 约定：高位有效，0x00=黑, 0xF0=白, 0x80=灰
 * 参考: https://epdiy.readthedocs.io/en/latest/api.html#colors */
#define EPD_DRAW_BLACK  (0x00)   /**< 黑色 */
#define EPD_DRAW_WHITE  (0xF0)   /**< 白色 */
#define EPD_DRAW_GRAY   (0x80)   /**< 灰色 */

/* 字体属性颜色宏（同上约定，用于 EpdFontProperties 的 fg_color / bg_color） */
#define EPD_FONT_FG_BLACK   (0x00)   /**< 黑色文字 */
#define EPD_FONT_FG_WHITE   (0xF0)   /**< 白色文字（反白底） */

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_EPD_DRIVER_H */
