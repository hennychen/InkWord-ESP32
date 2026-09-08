#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""字库 bin 结构对拍器（开源通用化 Phase 4，2026-10-24）。

校验 gen_cjk_font.swift（macOS 权威链）与 gen_cjk_font.py（Linux 平行
链）产出的 CKF1 字库 bin：格式自洽、码点表升序、长度精确闭环、
edge-touch 残余统计。双链字体/光栅化不同位图不逐字节一致（见
gen_cjk_font.py 模块头"已知差异"），本工具做结构层对拍——两链产物
必须同样全绿；--strict 下贴边超容忍值（256，与生成链 TOUCH_TOLERANCE
同源）判失败，供 CI 使用。

用法：
  python3 tools/font_check.py                     # 检查 src/cjk_font_data.bin
  python3 tools/font_check.py /tmp/pyfont/cjk_font_data.bin --strict
  python3 tools/font_check.py deck_poems.bin      # 子集字库
"""

import os
import struct
import sys

MAGIC = b"CKF1"
TOUCH_TOLERANCE = 256          # 与生成链 TOUCH_TOLERANCE 同源（双链同步）


def check(path, strict):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        print("font_check: ERROR cannot read %s (%s)" % (path, e))
        return 1
    errors = []
    warns = []

    # ---- 1. 头字段 ----
    if len(data) < 28:
        print("font_check: ERROR file too small (%dB) for CKF1 header" % len(data))
        return 1
    magic, version, levels, n = struct.unpack_from("<4sHHI", data, 0)
    if magic != MAGIC:
        errors.append("magic %r != %r" % (magic, MAGIC))
    if version != 1:
        errors.append("version %d != 1" % version)
    if levels != 4:
        # 消费端 cjk_font.h CJK_FONT_LEVELS=4；子集/主集均四级
        errors.append("levels %d != 4" % levels)
    if n == 0:
        errors.append("glyph count n = 0")

    cells = struct.unpack_from("<%dH" % levels, data, 12)
    strides = struct.unpack_from("<%dH" % levels, data, 12 + levels * 2)
    if len(data) < 12 + levels * 4 + 2 * n:
        errors.append("file truncated: cp table needs %dB, file %dB"
                      % (12 + levels * 4 + 2 * n, len(data)))
        for e in errors:
            print("font_check: ERROR %s" % e)
        return 1

    for lvl in range(levels):
        if cells[lvl] == 0:
            errors.append("level %d cell = 0" % lvl)
        expect_stride = (cells[lvl] + 7) // 8
        if strides[lvl] != expect_stride:
            errors.append("level %d stride %d != (cell+7)/8 = %d"
                          % (lvl, strides[lvl], expect_stride))

    # ---- 2. cp 表（升序/无重复/BMP）----
    cp_off = 12 + levels * 4
    cps = struct.unpack_from("<%dH" % n, data, cp_off)
    for i in range(1, n):
        if cps[i] <= cps[i - 1]:
            errors.append("cp table not strictly ascending at #%d "
                          "(U+%04X after U+%04X)" % (i, cps[i], cps[i - 1]))
            break

    # ---- 3. 长度精确闭环 ----
    bitmap_off = (cp_off + 2 * n + 3) & ~3
    expect_len = bitmap_off
    for lvl in range(levels):
        expect_len += n * strides[lvl] * cells[lvl]
    if len(data) != expect_len:
        errors.append("length mismatch: header+tables+bitmaps = %dB, file = %dB"
                      % (expect_len, len(data)))

    # ---- 4. 位图统计（全零字形 + 每级 edge-touch；四角像素与生成链
    # edgeTouch 同样计两次，统计口径一致）----
    level_bases = []
    off = bitmap_off
    for lvl in range(levels):
        level_bases.append(off)
        off += n * strides[lvl] * cells[lvl]

    def edge_touch_bits(b, cell, stride):
        """位图四边贴边像素数（与 gen_cjk_font.* edgeTouch 同口径）。
        四角像素被水平边和垂直边各计一次（共 2 次），与生成链 TOUCH_TOLERANCE
        阈值耦合——修正须双链同步，否则验收硬指标失效。"""
        t = 0
        last_byte = (cell - 1) >> 3
        last_bit = 0x80 >> ((cell - 1) & 7)
        for gy in range(cell):
            row = gy * stride
            if b[row] & 0x80:
                t += 1
            if b[row + last_byte] & last_bit:
                t += 1
        t += sum(bin(x).count("1") for x in b[0:stride])
        t += sum(bin(x).count("1")
                  for x in b[(cell - 1) * stride:cell * stride])
        return t

    empty = []
    touched = [0] * levels
    for i in range(n):
        glyph_empty = True
        for lvl in range(levels):
            sz = strides[lvl] * cells[lvl]
            b = data[level_bases[lvl] + i * sz:level_bases[lvl] + (i + 1) * sz]
            if any(b):
                glyph_empty = False
            touched[lvl] += edge_touch_bits(b, cells[lvl], strides[lvl])
        if glyph_empty:
            empty.append(cps[i])
    if empty:
        warns.append("%d all-zero glyphs (spaces expected): %s"
                     % (len(empty), " ".join("U+%04X" % c for c in empty[:8])
                        + (" ..." if len(empty) > 8 else "")))

    # ---- 报告 ----
    print("font_check: %s" % path)
    print("  magic=%s version=%d levels=%d n=%d cells=%s strides=%s"
          % (magic.decode("ascii", "replace"), version, levels, n,
             "/".join(str(c) for c in cells),
             "/".join(str(s) for s in strides)))
    total_bits = n * sum(s * c for s, c in zip(strides, cells))
    print("  bitmaps @%d, %d glyphs x %dB = %dB, file %dB"
          % (bitmap_off, n, sum(s * c for s, c in zip(strides, cells)),
             total_bits, len(data)))
    for lvl in range(levels):
        mark = "OK" if touched[lvl] == 0 else (
            "WARN" if touched[lvl] <= TOUCH_TOLERANCE else "FAIL")
        print("  level %d: cell=%2d stride=%d edge-touch=%d [%s]"
              % (lvl, cells[lvl], strides[lvl], touched[lvl], mark))
    for w in warns:
        print("  NOTE: %s" % w)

    for e in errors:
        print("  ERROR: %s" % e)
    if errors:
        print("font_check: FAIL (%d structural error(s))" % len(errors))
        return 1
    over = [lvl for lvl in range(levels) if touched[lvl] > TOUCH_TOLERANCE]
    if strict and over:
        print("font_check: FAIL (strict: edge-touch > %d at level(s) %s)"
              % (TOUCH_TOLERANCE, ",".join(str(l) for l in over)))
        return 1
    print("font_check: OK%s" % (" (strict)" if strict else ""))
    return 0


def main():
    args = sys.argv[1:]
    strict = "--strict" in args
    paths = [a for a in args if not a.startswith("--")]
    if not paths:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        paths = [os.path.join(root, "src", "cjk_font_data.bin")]
    rc = 0
    for p in paths:
        rc |= check(p, strict)
    return rc


if __name__ == "__main__":
    sys.exit(main())
