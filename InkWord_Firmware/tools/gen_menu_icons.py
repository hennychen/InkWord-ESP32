#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_menu_icons.py — 功能菜单 1bit 剪影图标生成器（v1.2 菜单视觉升级）

产出 src/menu_icons.h：11 枚 20x20 行主序 MSB-first 位图（bit=1 着色，
epd_gfx_draw_bitmap 直绘格式，同 cjk_font 先例）。约 60B/枚共 ~660B flash。

设计口径（1bit 墨水屏约束）：
  - 纯剪影无灰度，线宽 >=1.5px 防断线；3x3 过采样多数表决抗锯齿边缘；
  - 20px 网格上手绘级几何谓词组合，每枚图标一个 in_* 谓词函数；
  - TINY 档（项高 28）不消费本资源（menu_ui.c 同徽标省略先例）。

用法：
  python3 tools/gen_menu_icons.py            # 生成 + ASCII 预览（人工核对）
  python3 tools/gen_menu_icons.py --write    # 预览核对通过后落盘 .h
"""

import math
import sys
from pathlib import Path

SZ = 20                      # 画布边长（px）
ROW_BYTES = (SZ + 7) // 8    # 3


# ---------------------------------------------------------- 几何原语（谓词）
def rect(x, y, x0, y0, x1, y1):
    return x0 <= x <= x1 and y0 <= y <= y1


def circle(x, y, cx, cy, r):
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(x, y, cx, cy, r, w):
    d = math.hypot(x - cx, y - cy)
    return abs(d - r) <= w / 2


def seg(x, y, x0, y0, x1, y1, w):
    """点到线段距离 <= w/2（含端点圆帽）。"""
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
    px, py = x0 + t * dx, y0 + t * dy
    return math.hypot(x - px, y - py) <= w / 2


def tri(x, y, ax, ay, bx, by, cx, cy):
    """半平面同侧法（含边界）。"""
    def cross(px, py, qx, qy, rx, ry):
        return (qx - px) * (ry - py) - (qy - py) * (rx - px)
    d1, d2, d3 = (cross(x, y, ax, ay, bx, by), cross(x, y, bx, by, cx, cy),
                  cross(x, y, cx, cy, ax, ay))
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def star(x, y, cx, cy, r_out, r_in, points=5, rot=-90.0):
    """五角星内点测试：极径夹角判扇区，比较星形边界半径。"""
    dx, dy = x - cx, y - cy
    r = math.hypot(dx, dy)
    if r > r_out:
        return False
    a = math.degrees(math.atan2(dy, dx)) - rot
    a = math.fmod(a, 360.0 / points)
    if a < 0:
        a += 360.0 / points
    # 扇区边界半径：三角波插值（0 度=外顶点，半程=内顶点）
    half = 180.0 / points
    frac = a / half if a <= half else (360.0 / points - a) / half
    rb = r_in + (r_out - r_in) * (1.0 - abs(frac * 2 - 1))
    return r <= rb


def poly(x, y, pts):
    """ray casting 任意多边形。"""
    inside = False
    n = len(pts)
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        if (y0 > y) != (y1 > y):
            xt = x0 + (y - y0) * (x1 - x0) / (y1 - y0)
            if x < xt:
                inside = not inside
    return inside


# ---------------------------------------------------------- 图标定义（11 枚）
# 语义与菜单 s_items 一一对应；「挖空」用 not/in_ 补集表达。
ICONS = [
    # (符号名, 用途, 谓词)
    ("collected", "收藏列表",
     lambda x, y: star(x, y, 10, 10.5, 9, 3.8)),

    ("modesel", "模式选择",
     lambda x, y: (rect(x, y, 2, 4, 11, 12) and not rect(x, y, 4, 6, 9, 10))
                  or rect(x, y, 8, 7, 17, 16)),

    ("decks", "词书选择",
     lambda x, y: poly(x, y, [(2, 4), (9, 6), (9, 16), (2, 14)])
                  or poly(x, y, [(18, 4), (11, 6), (11, 16), (18, 14)])
                  or seg(x, y, 2, 14, 18, 14, 1.5)),

    ("chat", "AI 对话",
     lambda x, y: (rect(x, y, 1, 2, 18, 12) and not (
                     circle(x, y, 6.5, 7, 1.6) or circle(x, y, 10, 7, 1.6)
                     or circle(x, y, 13.5, 7, 1.6)))
                  or poly(x, y, [(5, 12), (9, 12), (4, 17)])),

    ("quiz", "快速测验",
     lambda x, y: (ring(x, y, 9.5, 9.5, 7.5, 2) or seg(x, y, 6, 10, 9, 13, 2.6)
                   or seg(x, y, 9, 13, 14, 6, 2.6))),

    ("audio", "音频同步",
     lambda x, y: circle(x, y, 6, 14.5, 3) or seg(x, y, 8.8, 13.8, 8.8, 3, 1.8)
                  or poly(x, y, [(8.8, 3), (15.5, 6.5), (8.8, 9.5)])),

    ("wifi", "Wi-Fi 配网",
     lambda x, y: circle(x, y, 10, 15.5, 1.8)
                  or (ring(x, y, 10, 15.5, 5.5, 1.8) and -0.55 <= (x - 10) / 5.5 <= 0.55
                      and y <= 15.5)
                  or (ring(x, y, 10, 15.5, 9.5, 1.8) and -0.75 <= (x - 10) / 9.5 <= 0.75
                      and y <= 15.5)),

    ("ap", "AP 配网门户",
     lambda x, y: rect(x, y, 1, 11, 18, 16)
                  or seg(x, y, 5, 11, 2, 4, 1.8) or seg(x, y, 15, 11, 18, 4, 1.8)
                  or circle(x, y, 2, 4, 1.3) or circle(x, y, 18, 4, 1.3)),

    ("lan", "LAN 接收页",
     lambda x, y: rect(x, y, 8.5, 2, 10.5, 10) or tri(x, y, 4.5, 8.5, 15.5, 8.5, 10, 15)
                  or rect(x, y, 1, 17.5, 18, 19)),

    ("settings", "设置",
     lambda x, y: seg(x, y, 2, 4, 17, 4, 2) or seg(x, y, 2, 10, 17, 10, 2)
                  or seg(x, y, 2, 16, 17, 16, 2)
                  or circle(x, y, 13.5, 4, 2.6) or circle(x, y, 6, 10, 2.6)
                  or circle(x, y, 13.5, 16, 2.6)),

    ("info", "设备信息",
     lambda x, y: ring(x, y, 10, 10, 7.8, 1.8) or circle(x, y, 10, 5.5, 1.5)
                  or rect(x, y, 9, 8, 11, 14.5)),

    ("keys", "按键说明",
     lambda x, y: circle(x, y, 10, 10, 3)
                  or tri(x, y, 7, 7.5, 13, 7.5, 10, 2) or tri(x, y, 7, 12.5, 13, 12.5, 10, 18)
                  or tri(x, y, 7.5, 7, 7.5, 13, 2, 10) or tri(x, y, 12.5, 7, 12.5, 13, 18, 10)),
]


# ---------------------------------------------------------- 栅格化（3x3 过采样）
def rasterize(fn):
    """3x3 子像素过采样多数表决 -> 1bit 平滑边缘。"""
    rows = []
    for py in range(SZ):
        row = bytearray(ROW_BYTES)
        for px in range(SZ):
            hits = 0
            for sy in (-1, 0, 1):
                for sx in (-1, 0, 1):
                    if fn(px + sx / 3.0, py + sy / 3.0):
                        hits += 1
            if hits >= 5:
                row[px >> 3] |= 0x80 >> (px & 7)
        rows.append(bytes(row))
    return b"".join(rows)


def preview(name, note, data):
    print(f"== {name}  {note}")
    for py in range(SZ):
        line = "".join("#" if data[py * ROW_BYTES + (px >> 3)] >> (7 - (px & 7)) & 1 else "."
                       for px in range(SZ))
        print("  |" + line + "|")


HEADER = """\
/**
 * @file menu_icons.h
 * @brief 功能菜单 1bit 剪影图标（自动生成，勿手改）
 *
 * 由 tools/gen_menu_icons.py 生成：11 枚 20x20 行主序 MSB-first 位图
 * （bit=1 着色），epd_gfx_draw_bitmap 直绘格式（cjk_font 同款）。
 * 消费方 menu_ui.c 独家 include（单编译单元，无重复定义）；TINY 档
 * 项高 28px 不消费（同中文徽标省略先例）。~660B flash。
 */
#ifndef INKWORD_MENU_ICONS_H
#define INKWORD_MENU_ICONS_H

#include <stdint.h>

#define MENU_ICON_SZ 20
#define MENU_ICON_BYTES ((MENU_ICON_SZ + 7) / 8 * MENU_ICON_SZ)

#ifdef __cplusplus
extern "C" {
#endif

{decls}

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_MENU_ICONS_H */
""".replace("{decls}", "{__DECLS__}")


def main():
    write = "--write" in sys.argv
    out = {}
    for name, note, fn in ICONS:
        data = rasterize(fn)
        out[name] = data
        preview(name, note, data)
        if len(data) != ROW_BYTES * SZ:
            sys.exit(f"size mismatch: {name}")

    if not write:
        print("\n(--write 落盘 src/menu_icons.h)")
        return

    decls, defs = [], []
    for name, note, _ in ICONS:
        sym = f"menu_icon_{name}"
        decls.append(f"extern const uint8_t {sym}[MENU_ICON_BYTES];/* {note} */")
        hexs = ",".join(f"0x{b:02X}" for b in out[name])
        defs.append(f"const uint8_t {sym}[MENU_ICON_BYTES] = /* {note} */{{\n{hexs},\n}};")

    body = HEADER.replace("{__DECLS__}", "\n".join(decls)) + "\n/* ---- 定义（唯一编译单元：menu_ui.c） ---- */\n" + "\n".join(defs) + "\n"
    dst = Path(__file__).resolve().parent.parent / "src" / "menu_icons.h"
    dst.write_text(body, encoding="utf-8")
    print(f"\nwrote {dst} ({len(body)} B)")


if __name__ == "__main__":
    main()
