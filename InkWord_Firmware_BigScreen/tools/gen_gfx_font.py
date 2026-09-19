#!/usr/bin/env python3
"""gen_gfx_font.py —— Adafruit GFX 经典字体大字表生成器（大屏 UI 重设计 2026-09-17）

背景：词卡双栏词头需 36/48pt 大字（现有 FreeSans 上限 24pt，Adafruit
gfx-fonts 官方无 36/48pt 表，GNU FreeFont TTF 源本机不可得）。取系统
Helvetica.ttc 生成——FreeSans 为 URW Nimbus Sans（Helvetica 克隆）衍生，
Helvetica 与其同形，与现有 FreeSans 9~24pt 小字视觉连续。

输出：与 src/gfx/fonts/FreeSans24pt7b.h 完全同构的 Adafruit GFX 格式
（位流连续打包 MSB-first 跨行不重置——见 gfxfont.h 注释；GFXglyph/
GFXfont 结构体；PROGMEM 空宏）。

口径校准：官方 gfx-fonts 表按 dpi=141 生成（FreeSans24pt7b 'A' cap 34px
= 0.729em × 24pt@141dpi 反推）；本脚本默认同口径，--check 可与现有表
度量比对验证。

用法（生成 36/48pt 四表）：
  python3 tools/gen_gfx_font.py /System/Library/Fonts/Helvetica.ttc 0 36 \
      src/gfx/fonts/Helv36pt7b.h
  python3 tools/gen_gfx_font.py /System/Library/Fonts/Helvetica.ttc 1 36 \
      src/gfx/fonts/HelvBold36pt7b.h
  python3 tools/gen_gfx_font.py /System/Library/Fonts/Helvetica.ttc 0 48 \
      src/gfx/fonts/Helv48pt7b.h
  python3 tools/gen_gfx_font.py /System/Library/Fonts/Helvetica.ttc 1 48 \
      src/gfx/fonts/HelvBold48pt7b.h
"""
import argparse
import sys

try:
    import freetype
except ImportError:
    sys.exit("需要 freetype-py：pip install freetype-py")

DEFAULT_RANGE = (0x20, 0x7E)   # ASCII 可见字符（与现有 FreeSans 表同域）
DEFAULT_DPI = 141              # 官方 gfx-fonts 口径（'A' cap 34px @24pt 反推）


def gen_face(font_path, face_index, pt, dpi):
    face = freetype.Face(font_path, face_index)
    face.set_char_size(pt * 64, 0, dpi, dpi)   # 26.6 fixed point
    return face


def extract_glyph(face, cp):
    """渲染单字形（MONO 1bpp），返回 (位流, width, height, xAdvance, xOffset, yOffset)。"""
    if not face.get_char_index(cp):
        return None
    face.load_char(chr(cp), freetype.FT_LOAD_RENDER
                   | freetype.FT_LOAD_TARGET_MONO)
    g = face.glyph
    bmp = g.bitmap
    w, rows, pitch = bmp.width, bmp.rows, bmp.pitch
    bits = []
    for y in range(rows):
        row = bmp.buffer[y * pitch:(y + 1) * pitch]
        for x in range(w):
            bits.append((row[x >> 3] >> (7 - (x & 7))) & 1)
    adv = g.advance.x >> 6
    return bits, w, rows, adv, g.bitmap_left, -g.bitmap_top


def build(font_path, face_index, pt, dpi, cp_lo, cp_hi, name, out_path):
    face = gen_face(font_path, face_index, pt, dpi)
    family = face.family_name.decode()
    style = face.style_name.decode()

    bitmap_bytes = bytearray()
    glyphs = []
    bitbuf = 0
    nbits = 0

    def push_bits(bits):
        """连续位流（MSB-first，字形内跨行不重置——消费端 epd_gfx.c
        drawChar 的 bo 计数口径）。"""
        nonlocal bitbuf, nbits
        for b in bits:
            bitbuf = (bitbuf << 1) | b
            nbits += 1
            if nbits == 8:
                bitmap_bytes.append(bitbuf)
                bitbuf = 0
                nbits = 0

    def flush_to_byte():
        """字形尾 pad 到字节边界：bitmapOffset 为字节偏移（消费端
        f->bitmap + offset 直接相加），字形起点必须整字节对齐。"""
        nonlocal bitbuf, nbits
        if nbits:
            bitmap_bytes.append((bitbuf << (8 - nbits)) & 0xFF)
            bitbuf = 0
            nbits = 0

    for cp in range(cp_lo, cp_hi + 1):
        r = extract_glyph(face, cp)
        if r is None:
            sys.exit(f"U+{cp:04X} 不在字体 {family} {style} 内（range 需收敛）")
        bits, w, h, adv, xo, yo = r
        offset = len(bitmap_bytes)   # 字节偏移（字形起点整字节对齐）
        push_bits(bits)
        flush_to_byte()
        if adv > 255 or w > 255 or h > 255:
            sys.exit(f"U+{cp:04X} 度量溢出 uint8（w={w} h={h} adv={adv}），"
                     "需降 pt 或分表")
        glyphs.append((offset, w, h, adv, xo, yo, chr(cp)))

    if len(bitmap_bytes) > 65535:
        sys.exit(f"位图总量 {len(bitmap_bytes)}B 溢出 bitmapOffset uint16，"
                 "需分表")

    # yAdvance：缩放后行高（freetype-py 版本差异兼容：部分版本
    # face.size 直接是 SizeMetrics，无 .metrics 包装；26.6 定点 +32 舍入）
    sm = getattr(face.size, "metrics", face.size)
    y_adv = (sm.height + 32) >> 6
    if y_adv > 255:
        sys.exit(f"yAdvance {y_adv} 溢出 uint8")

    name_u = name  # C 符号前缀（如 Helv48pt7b）
    lines = []
    lines.append("#pragma once")
    lines.append("// ============================================================")
    lines.append(f"// {name_u} — {family} {style} {pt}pt @{dpi}dpi 点阵表"
                 f"（Adafruit GFX 格式）")
    lines.append(f"// 生成：python3 tools/gen_gfx_font.py {font_path} "
                 f"{face_index} {pt} <out.h>（freetype-py，MONO 1bpp）")
    lines.append(f"// 来源：macOS 系统字体（大屏 UI 重设计 2026-09-17）；与"
                 f" FreeSans 小字表")
    lines.append("// 同为 Helvetica 风格，混排视觉连续。勿手改。")
    lines.append("// ============================================================")
    lines.append('#include "gfxfont.h"')
    lines.append("")
    lines.append(f"const uint8_t {name_u}Bitmaps[] PROGMEM = {{")
    hexes = [f"0x{b:02X}" for b in bitmap_bytes]
    for i in range(0, len(hexes), 12):
        lines.append("    " + ", ".join(hexes[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"const GFXglyph {name_u}Glyphs[] PROGMEM = {{")
    for i, (off, w, h, adv, xo, yo, ch) in enumerate(glyphs):
        comma = "," if i < len(glyphs) - 1 else "};"
        esc = ch if 0x20 < ord(ch) < 0x7F and ch not in ("\\",) else \
            f"\\x{ord(ch):02x}"
        lines.append(f"    {{{off}, {w}, {h}, {adv}, {xo}, {yo}}}{comma}"
                     f"  // 0x{cp_lo + i:02X} '{esc}'")
    lines.append("")
    lines.append(f"const GFXfont {name_u} PROGMEM = {{(uint8_t *){name_u}Bitmaps,")
    lines.append(f"                                        (GFXglyph *)"
                 f"{name_u}Glyphs, 0x{cp_lo:02X},")
    lines.append(f"                                        0x{cp_hi:02X}, {y_adv}}};")
    lines.append("")
    lines.append(f"// Approx. {len(bitmap_bytes) + len(glyphs) * 6 + 20} bytes")
    lines.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    print(f"{name_u}: glyphs={len(glyphs)} bitmap={len(bitmap_bytes)}B "
          f"yAdvance={y_adv} ({family} {style} {pt}pt@{dpi}dpi)")


def check(font_path, pt, dpi, ref_name, ref_path):
    """度量校准：比对生成的 Helvetica 度量与现有 FreeSans 表（同 pt 同 dpi）。"""
    import re
    ref = {}
    pat = re.compile(r"\{\d+, (\d+), (\d+), (\d+), (-?\d+), (-?\d+)\},"
                     r"\s*// 0x([0-9A-F]+)")
    with open(ref_path) as f:
        for line in f:
            m = pat.search(line)
            if m:
                w, h, adv, xo, yo, cp = m.groups()
                ref[int(cp, 16)] = tuple(map(int, (w, h, adv, xo, yo)))
    face = gen_face(font_path, 0, pt, dpi)
    diffs = {}
    for cp, ref_v in sorted(ref.items()):
        r = extract_glyph(face, cp)
        if r is None:
            continue
        got = (r[1], r[2], r[3], r[4], r[5])
        d = max(abs(a - b) for a, b in zip(got, ref_v[:5]))
        diffs.setdefault(d, []).append(chr(cp))
    for d in sorted(diffs):
        sample = "".join(diffs[d][:20])
        print(f"  maxdiff={d}px: {len(diffs[d])} glyphs [{sample}...]")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("font", help="字体文件（ttf/ttc）")
    ap.add_argument("face_index", type=int, help="ttc face 序号")
    ap.add_argument("pt", type=int, help="字号（pt）")
    ap.add_argument("out", help="输出 .h 路径")
    ap.add_argument("--name", help="C 符号前缀（缺省从输出文件名推导）")
    ap.add_argument("--dpi", type=int, default=DEFAULT_DPI)
    ap.add_argument("--range", default=f"{DEFAULT_RANGE[0]:x}-{DEFAULT_RANGE[1]:x}",
                    help="码点区间 hex-hex（缺省 20-7e）")
    ap.add_argument("--check", metavar="REF_H",
                    help="校准模式：与现有表 .h 度量比对（不生成文件）")
    args = ap.parse_args()

    if args.check:
        check(args.font, args.pt, args.dpi, args.name or "REF", args.check)
        return

    import os
    stem = os.path.splitext(os.path.basename(args.out))[0]
    name = args.name or stem
    lo_s, hi_s = args.range.split("-")
    build(args.font, args.face_index, args.pt, args.dpi,
          int(lo_s, 16), int(hi_s, 16), name, args.out)


if __name__ == "__main__":
    main()
