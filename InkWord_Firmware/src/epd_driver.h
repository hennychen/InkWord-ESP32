/**
 * @file epd_driver.h
 * @brief 墨水屏驱动封装 — DEPG0370BBU253F33HP-M7 3.7" 直驱 SPI
 *
 * 硬件：ESP32-S3 + EVK011 升压转接板 + DEPG0370 3.7" 墨水屏
 * 接口：4 线 SPI（BS1=LOW），驱动核心为 GxEPD2，本文件为 C API 薄适配层
 * 控制器：屏载 UC8253 类 COG（升压由 COG 经 GDR 自主驱动板上分立 boost，
 *         MCU 唯一电源职责是向 J2-16 (EPAPER_VCI) 供 3.3V，无 GDR/RESE 信号）
 *
 * 分辨率：面板物理 240 x 416；GFX 显示层默认横屏 416 x 240（rotation=1）
 * 帧缓冲：12,480 字节 (1bpp, 竖屏格式，bit=1 白)
 */
#ifndef INKWORD_EPD_DRIVER_H
#define INKWORD_EPD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 双坐标体系：
 *   - 面板物理/底层直通：竖屏 240x416（epd_full_refresh / epd_clear_screen）
 *   - GFX 显示层：横屏 416x240，rotation=1（epd_gfx_* 系列，UI 主路径） */
#define EPD_WIDTH       (240)
#define EPD_HEIGHT      (416)
#define EPD_GFX_WIDTH   (416)   /* 显示坐标宽（横屏） */
#define EPD_GFX_HEIGHT  (240)   /* 显示坐标高（横屏） */
#define EPD_FB_SIZE     (EPD_WIDTH / 8 * EPD_HEIGHT)  /* 12,480 字节，竖屏格式（30 字节/行 x 416 行） */

/**
 * @brief 初始化墨水屏硬件：GPIO / SPI / 升压使能。
 * @return 0 成功；非 0 失败。
 */
int epd_driver_init(void);

/**
 * @brief 保留兼容的空操作：COG 收到 0x04 后自主升压，无需 MCU 干预。
 */
void epd_power_on(void);

/**
 * @brief 发送 0x02 让 COG 关闭高压 rails（VCI 3.3V 保持供电）。
 */
void epd_power_off(void);

/**
 * @brief 全屏清白（黑白交替深清：先全黑全刷再回白，洗掉陈年残影；
 *        开机白屏 / 清残影 / 局刷阈值等低频路径使用，多一次全刷）。
 */
void epd_clear_screen(void);

/**
 * @brief 全屏刷新（全刷模式，竖屏 240x416）。
 * @param data 竖屏格式 1bpp 位图（EPD_FB_SIZE 字节，行宽 30，
 *             bit=1 为白 0x00=黑，与 demo/COG SRAM 语义一致）。
 *             NULL 时等同于 epd_clear_screen()。
 */
void epd_full_refresh(const uint8_t *data);

/**
 * @brief 深度休眠（0x07/0xA5）。下次刷新前 GxEPD2 自动硬件复位并
 *        重新初始化 COG，无需重新调用 epd_driver_init()。
 */
void epd_deep_sleep(void);

/**
 * @brief 读取屏幕厂商 ID（桩实现）。
 */
uint16_t epd_get_manufacturer(char *manufacturer, size_t len);

#ifdef __cplusplus
}
#endif

/* ============================================================
 * C-callable GFX 包装函数（供 .c 文件使用）
 * 基于内部 1bpp 帧缓冲的简易绘图 API
 * ============================================================ */
#ifdef __cplusplus
extern "C" {
#endif

/** @brief 获取帧缓冲宽度 */
int epd_gfx_width(void);
/** @brief 获取帧缓冲高度 */
int epd_gfx_height(void);
/** @brief 填充整个屏幕 (0=白, 1=黑) */
void epd_gfx_fill_screen(uint16_t color);
/** @brief 填充矩形 */
void epd_gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
/** @brief 画矩形边框 */
void epd_gfx_draw_rect(int x, int y, int w, int h, uint16_t color);
/** @brief 画水平线 */
void epd_gfx_draw_hline(int x, int y, int w, uint16_t color);
/** @brief 画垂直线 */
void epd_gfx_draw_vline(int x, int y, int h, uint16_t color);
/** @brief 画文本（font_size: 1=小9pt, 2=中14pt默认, 3=大18pt, 4=特大24pt；y 为基线） */
void epd_gfx_draw_text(int x, int y, const char *text, uint16_t color, int font_size);
/** @brief 测量文本宽高 */
void epd_gfx_text_bounds(const char *text, int font_size, int *out_w, int *out_h);
/** @brief 绘制单色位图（Adafruit GFX 行主序 MSB-first，bit=1 画 color，0 保持背景；
 *         即 image2cpp "horizontal, MSB first" 导出格式，尺寸建议 8 对齐） */
void epd_gfx_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits, uint16_t color);
/** @brief 读回画布窗口位图（与 draw_bitmap 同格式：行主序 MSB-first，
 *         bit=1=画布置位=黑），供上层新旧帧差分统计（智能局刷/全刷分流）；
 *         out 容量需 >= ceil(w/8)*h 字节 */
void epd_gfx_read_window(int x, int y, int w, int h, uint8_t *out);
/** @brief 将帧缓冲推送到屏幕（全刷） */
void epd_gfx_flush(void);
/** @brief 将指定区域推送到屏幕（局刷，默认双刷 2x0x12 减浅影） */
void epd_gfx_flush_window(int x, int y, int w, int h);
/** @brief 同上，可指定同会话 0x12 次数（passes=1 单刷最快，2 双刷减浅影）。
 *         无窗口双 RAM 波形强，两段式刷新两个方向均单刷（passes=1）
 *         即可洗净（2026-08-20 真机验证）；双刷留作浅影回退手段 */
void epd_gfx_flush_window_passes(int x, int y, int w, int h, int passes);

#ifdef __cplusplus
}
#endif

/* GFX 颜色常量 */
#define EPD_GFX_BLACK  1
#define EPD_GFX_WHITE  0

#endif /* INKWORD_EPD_DRIVER_H */
