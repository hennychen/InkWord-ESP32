#!/usr/bin/env python3
"""host 复刻固件 cjk_text.c wrap_walk 断行几何（3.7\" MID 档验证用）。

复刻点：utf8_next / ink_span / adv_one(advance=ink_w+2) / wrap_walk
断行条件 cur_x - x + uw > max_w；未收录字符按全宽 cell 推进。
用法：python3 sim_mean_wrap.py [词...]；默认取词库最长释义前 3 条。
"""
import json
import struct
import sys
import unicodedata

BIN = '/Users/pm/InkWord-ESP32/InkWord_Firmware/src/cjk_font_data.bin'
VOCAB = '/Users/pm/InkWord-ESP32/tools/default_vocab/out/default_words.json'
CT_SPACING = 2

data = open(BIN, 'rb').read()
magic, ver, levels, n = struct.unpack_from('<4sHHI', data, 0)
assert magic == b'CKF1', magic
cells = struct.unpack_from('<%dH' % levels, data, 12)
strides = struct.unpack_from('<%dH' % levels, data, 12 + levels * 2)
cp_off = 12 + levels * 4
cps = struct.unpack_from('<%dH' % n, data, cp_off)
cp_index = {cp: i for i, cp in enumerate(cps)}
bmp_off = (cp_off + 2 * n + 3) & ~3
lvl_base = []
off = bmp_off
for i in range(levels):
    lvl_base.append(off)
    off += n * strides[i] * cells[i]
print('bin: ver=%d levels=%s cells=%s strides=%s n=%d' % (ver, levels, cells, strides, n))


def glyph_bits(cp, level):
    i = cp_index.get(cp)
    if i is None:
        return None
    base = lvl_base[level] + i * strides[level] * cells[level]
    return data[base:base + strides[level] * cells[level]]


def ink_span(bits, cell, stride):
    l = -1
    r = -1
    for x in range(cell):
        if l >= 0:
            break
        for y in range(cell):
            if bits[y * stride + x // 8] & (0x80 >> (x % 8)):
                l = x
                break
    if l < 0:
        return 0, 0
    for x in range(cell - 1, -1, -1):
        if x <= r:
            break
        for y in range(cell):
            if bits[y * stride + x // 8] & (0x80 >> (x % 8)):
                r = x
                break
    return l, r - l + 1


def adv_one(cp, level):
    """返回 (advance, ink_l, ink_w)；未收录 → 全宽占位（固件画框）。"""
    cell, stride = cells[level], strides[level]
    if cp < 0x20 or cp == 0x7F:
        cp = 0x20
    bits = glyph_bits(cp, level)
    if bits is None:
        return cell + CT_SPACING, None, None, True
    l, w = ink_span(bits, cell, stride)
    if w == 0:
        return (cell if cp == 0x3000 else cell // 2), l, w, False
    return w + CT_SPACING, l, w, False


def run_width(s, level):
    w = 0
    for ch in s:
        adv, _, _, miss = adv_one(ord(ch), level)
        w += adv
    return w


def wrap_walk(s, max_w, level):
    """复刻固件断行：返回 [(line_text, line_width)]，并统计未收录字符。"""
    lines = []
    cur, cur_w = '', 0
    line_empty = True
    missing = []
    i = 0
    while i < len(s):
        ch = s[i]
        cp = ord(ch)
        if cp in (0x20, 0x09, 0x3000):
            if not line_empty:
                adv, _, _, _ = adv_one(cp, level)
                cur += ch
                cur_w += adv
            i += 1
            continue
        if cp < 0x80 and 0x21 <= cp <= 0x7E:
            unit = ''
            while i < len(s) and 0x21 <= ord(s[i]) <= 0x7E:
                unit += s[i]
                i += 1
        else:
            unit = ch
            i += 1
        uw = run_width(unit, level)
        for c in unit:
            if glyph_bits(ord(c), level) is None and ord(c) >= 0x80:
                missing.append(c)
        if not line_empty and cur_w + uw > max_w:
            lines.append((cur, cur_w))
            cur, cur_w = '', 0
            line_empty = True
        cur += unit
        cur_w += uw
        line_empty = False
    if not line_empty or not lines:
        lines.append((cur, cur_w))
    return lines, missing


def simulate(text, meaning, max_w, x0, level, gfx_w, label):
    stream = meaning  # 简化：仅释义本体（root/example 拼接同链路）
    lines, missing = wrap_walk(stream, max_w, level)
    print('\n=== %s | max_w=%d x0=%d level=%d(%dpx) gfx_w=%d ===' %
          (label, max_w, x0, level, cells[level], gfx_w))
    over = False
    for idx, (txt, w) in enumerate(lines):
        right = x0 + w - CT_SPACING
        flag = ''
        if right > gfx_w:
            flag = '  <<< 超出画布右缘!'
            over = True
        elif x0 + w > max_w + x0 + 1:
            flag = '  <<< 超出断行宽?'
        print('L%-2d w=%3d 右缘x=%3d %s | %s' % (idx, w, right, flag, txt))
    print('行数=%d 未收录=%s' % (len(lines), ''.join(sorted(set(missing))) or '无'))
    return over


def main():
    # MID 档（3.7" 416x240）：margin 16 → max_w=384；level 1=20px 默认档
    GFX_W, MARGIN, LEVEL_DEF, LEVEL_BIG = 416, 16, 1, 2
    MAXW = GFX_W - 2 * MARGIN
    words = json.load(open(VOCAB))
    if isinstance(words, dict):
        words = words.get('words', words)
    args = sys.argv[1:]
    if args:
        picked = [w for w in words if w.get('text', '').lower() in
                  [a.lower() for a in args]]
    else:
        picked = sorted(words, key=lambda w: len(w.get('meaning', '')),
                        reverse=True)[:3]
    any_over = False
    for w in picked:
        any_over |= simulate(w['text'], w.get('meaning', ''), MAXW, MARGIN,
                             LEVEL_DEF, GFX_W, '%s 默认档20px' % w['text'])
        any_over |= simulate(w['text'], w.get('meaning', ''), MAXW, MARGIN,
                             LEVEL_BIG, GFX_W, '%s 大字档24px' % w['text'])
    print('\n结论：%s' % ('存在超界行！' if any_over else '所有行均在画布内'))


if __name__ == '__main__':
    main()
