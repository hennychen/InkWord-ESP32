#pragma once
// ============================================================
// gfxfont.h — Adafruit GFX 经典字体类型（自 Adafruit_GFX.h 提取，
// BSD/MIT 许可数据表配套的最小类型定义；ESP-IDF 无 Arduino 头）
//
// 位图格式：字形位流连续打包（bitmapOffset 起始，逐行逐位 MSB-first，
// 位计数跨行累计不重置——即整字形连续位流，非行字节对齐）。
// drawChar 语义（Adafruit GFX 标准）：光标 y 为文本基线，字形上左角
// = (x + xOffset, y + yOffset)。
// ============================================================
#include <stdint.h>

// ESP32/IDF：flash 与 RAM 统一编址，PROGMEM 为空语义（对齐 Arduino 移植）
#ifndef PROGMEM
#define PROGMEM
#endif

/// 字形描述（一个 ASCII 字符）
typedef struct {
    uint16_t bitmapOffset; /**< 位图数据在 GFXfont->bitmap 中的起始偏移 */
    uint8_t width;         /**< 位图宽（像素） */
    uint8_t height;        /**< 位图高（像素） */
    uint8_t xAdvance;      /**< 光标 x 前进距离 */
    int8_t xOffset;        /**< 光标位置到字形左上角的 x 偏移 */
    int8_t yOffset;        /**< 基线到字形左上角的 y 偏移（负值在上） */
} GFXglyph;

/// 字体表（一个字号一表）
typedef struct {
    const uint8_t *bitmap; /**< 打包位图数据 */
    const GFXglyph *glyph; /**< 字形表（first..last 索引） */
    uint16_t first;        ///< ASCII 起始字符
    uint16_t last;         ///< ASCII 结束字符
    uint8_t yAdvance;      ///< 行高（newline 前进距离）
} GFXfont;
