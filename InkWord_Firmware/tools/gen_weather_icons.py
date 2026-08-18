#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 src/weather_icons.h 的 40x40 单色天气图标（Adafruit GFX 行主序 MSB-first）。

以几何图元（圆盘/矩形/带粗细线段）绘制八种填充式天气图标，
打包为 C 数组并输出 ASCII 预览，便于核对形状。

用法：python3 tools/gen_weather_icons.py > /tmp/icons.txt
人工核对预览后，将数组段复制进 src/weather_icons.h。
"""
import math

W = H = 40          # 图标尺寸（px）
ROW_BYTES = W // 8  # 每行 5 字节


def new():
    return [[0] * W for _ in range(H)]


def px(c, x, y):
    x, y = int(x), int(y)
    if 0 <= x < W and 0 <= y < H:
        c[y][x] = 1


def disc(c, cx, cy, r):
    """实心圆盘。"""
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                px(c, x, y)


def rect(c, x0, y0, x1, y1):
    """实心矩形（闭区间）。"""
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(c, x, y)


def line(c, x0, y0, x1, y1, b=1):
    """带粗细线段：b=0 单像素，b=1 约 3px 粗。"""
    n = max(abs(x1 - x0), abs(y1 - y0)) * 2 or 1
    for i in range(n + 1):
        t = i / n
        x = round(x0 + (x1 - x0) * t)
        y = round(y0 + (y1 - y0) * t)
        for oy in range(-b, b + 1):
            for ox in range(-b, b + 1):
                px(c, x + ox, y + oy)


def flake(c, cx, cy):
    """小雪花：十字(±3) + 对角叉(±2)。"""
    line(c, cx - 3, cy, cx + 3, cy, 0)
    line(c, cx, cy - 3, cx, cy + 3, 0)
    line(c, cx - 2, cy - 2, cx + 2, cy + 2, 0)
    line(c, cx - 2, cy + 2, cx + 2, cy - 2, 0)


def cloud(c, dy=0, big=False):
    """云朵：三圆盘 + 平底矩形。big=居中大云，否则上移小云（给雨雪留底部空间）。"""
    if big:
        disc(c, 12, 21 + dy, 7)
        disc(c, 21, 18 + dy, 9)
        disc(c, 30, 21 + dy, 7)
        rect(c, 5, 24 + dy, 36, 31 + dy)
    else:
        disc(c, 12, 16 + dy, 6)
        disc(c, 21, 13 + dy, 8)
        disc(c, 30, 16 + dy, 6)
        rect(c, 6, 19 + dy, 35, 25 + dy)


def cloud_small_low(c):
    """局部多云专用：下移小云，右上留白给太阳。"""
    disc(c, 10, 26, 6)
    disc(c, 19, 23, 8)
    disc(c, 28, 26, 6)
    rect(c, 4, 29, 34, 35)


def icon_clear():
    c = new()
    disc(c, 20, 20, 8)
    for k in range(8):  # 8 向光芒
        a = math.radians(k * 45)
        x0, y0 = 20 + round(11 * math.cos(a)), 20 + round(11 * math.sin(a))
        x1, y1 = 20 + round(16 * math.cos(a)), 20 + round(16 * math.sin(a))
        line(c, x0, y0, x1, y1)
    return c


def icon_partly():
    c = new()
    disc(c, 27, 10, 6)              # 右上角太阳
    line(c, 27, 2, 27, 0)           # 上光芒
    line(c, 33, 4, 34, 3)           # 右上光芒
    line(c, 35, 10, 37, 10)         # 右光芒
    cloud_small_low(c)              # 前景下移小云，右上留白给太阳
    return c


def icon_cloud():
    c = new()
    cloud(c, big=True)
    return c


def icon_fog():
    c = new()
    cloud(c)                        # 上移云
    line(c, 5, 30, 35, 30)          # 三条雾线
    line(c, 9, 34, 34, 34)
    line(c, 13, 38, 30, 38)
    return c


def icon_rain():
    c = new()
    cloud(c)
    for x0, y0, x1, y1 in [(11, 28, 8, 33), (19, 28, 16, 33),
                           (27, 28, 24, 33), (33, 28, 30, 32)]:
        line(c, x0, y0, x1, y1)     # 斜雨滴
    return c


def icon_shower():
    c = new()
    cloud(c)
    for x0, y0, x1, y1 in [(12, 28, 5, 38), (21, 28, 14, 38), (30, 28, 23, 38)]:
        line(c, x0, y0, x1, y1)     # 长斜雨线（阵雨）
    return c


def icon_snow():
    c = new()
    cloud(c)
    flake(c, 11, 32)
    flake(c, 21, 36)
    flake(c, 31, 32)
    return c


def icon_thunder():
    c = new()
    cloud(c)
    line(c, 25, 27, 17, 34)         # 闪电折线
    line(c, 17, 34, 22, 34)
    line(c, 22, 34, 13, 39)
    return c


ICONS = [
    ("clear", icon_clear),
    ("partly", icon_partly),
    ("cloud", icon_cloud),
    ("fog", icon_fog),
    ("rain", icon_rain),
    ("shower", icon_shower),
    ("snow", icon_snow),
    ("thunder", icon_thunder),
]


def pack(c):
    """打包为行主序 MSB-first 字节流（bit=1 前景）。"""
    out = []
    for y in range(H):
        for j in range(ROW_BYTES):
            b = 0
            for k in range(8):
                if c[y][j * 8 + k]:
                    b |= 0x80 >> k
            out.append(b)
    return out


def preview(name, c):
    print(f"---- {name} ----")
    for y in range(H):
        print("".join("#" if v else "." for v in c[y]))
    print()


def emit(name, data):
    print(f"/* {name} */")
    print(f"static const uint8_t wx_icon_{name}_bits[{H * ROW_BYTES}] = {{")
    for i in range(0, len(data), 12):
        print("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",")
    print("};")
    print()


HEADER_TMPL = """/**
 * @file weather_icons.h
 * @brief 待机页天气图标（40x40 单色位图，Adafruit GFX 行主序 MSB-first）
 *
 * 图标枚举与后端 /api/device/weather 返回的 icon 字段一一对应（0~7），
 * 后端负责 WMO 天气码到本枚举的映射。
 *
 * 本文件由 tools/gen_weather_icons.py 几何图元生成，勿手改数组；
 * 修改形状请改脚本后重新生成：
 *   python3 tools/gen_weather_icons.py --header src/weather_icons.h
 */
#ifndef INKWORD_WEATHER_ICONS_H
#define INKWORD_WEATHER_ICONS_H

#include <stdint.h>

#define WX_ICON_W      (40)
#define WX_ICON_H      (40)
#define WX_ICON_BYTES  (WX_ICON_W / 8 * WX_ICON_H)   /**< 200 字节/枚 */

/** 天气图标枚举（与后端 icon 字段契约一致，勿改顺序） */
typedef enum {
    WX_ICON_CLEAR = 0,    /**< 晴 */
    WX_ICON_PARTLY,       /**< 多云间晴 */
    WX_ICON_CLOUD,        /**< 阴 */
    WX_ICON_FOG,          /**< 雾 */
    WX_ICON_RAIN,         /**< 雨 */
    WX_ICON_SHOWER,       /**< 阵雨 */
    WX_ICON_SNOW,         /**< 雪 */
    WX_ICON_THUNDER,      /**< 雷暴 */
    WX_ICON_COUNT
} weather_icon_t;

/* ---- 位图数据（epd_gfx_draw_bitmap 直接可用，bit=1 前景） ---- */

"""

FOOTER_TMPL = """/**
 * @brief 枚举到位图的映射。
 * @param icon weather_icon_t 枚举值（后端 icon 字段）。
 * @return 位图指针；icon 越界返回 NULL（调用方应降级为纯文本）。
 */
static inline const uint8_t *weather_icon_bits(int icon)
{
    static const uint8_t *const s_table[WX_ICON_COUNT] = {
        wx_icon_clear_bits, wx_icon_partly_bits, wx_icon_cloud_bits,
        wx_icon_fog_bits,   wx_icon_rain_bits,   wx_icon_shower_bits,
        wx_icon_snow_bits,  wx_icon_thunder_bits,
    };
    return (icon >= 0 && icon < WX_ICON_COUNT) ? s_table[icon] : NULL;
}

#endif /* INKWORD_WEATHER_ICONS_H */
"""


def write_header(path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(HEADER_TMPL)
        for name, fn in ICONS:
            data = pack(fn())
            f.write(f"/* {name} */\n")
            f.write(f"static const uint8_t wx_icon_{name}_bits[{H * ROW_BYTES}] = {{\n")
            for i in range(0, len(data), 12):
                f.write("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",\n")
            f.write("};\n\n")
        f.write(FOOTER_TMPL)


if __name__ == "__main__":
    import sys
    if len(sys.argv) >= 3 and sys.argv[1] == "--header":
        write_header(sys.argv[2])
        print(f"header written: {sys.argv[2]}")
    else:
        for name, fn in ICONS:
            preview(name, fn())
        print("=" * 60)
        print("/* ---- 以下为 C 数组，复制进 src/weather_icons.h ---- */")
        print()
        for name, fn in ICONS:
            emit(name, pack(fn()))
