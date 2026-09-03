#!/usr/bin/env python3
"""dump cjk_font_data.bin 各级字形为 PNG（字形完整性目验）。

按固件 cjk_font.c 同一头部布局解析（magic/ver/levels/n + cell[] +
stride[] + cp 表 + 各级位图），渲染样例字到 PNG 供人眼检查墨迹
左右边缘是否被裁切。
"""
import struct
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else \
    '/Users/pm/InkWord-ESP32/InkWord_Firmware/src/cjk_font_data.bin'
SAMPLE = sys.argv[2] if len(sys.argv) > 2 else '学英语圆的完全'

data = open(BIN, 'rb').read()
magic, ver, levels, n = struct.unpack_from('<4sHHI', data, 0)
cells = struct.unpack_from('<%dH' % levels, data, 12)
strides = struct.unpack_from('<%dH' % levels, data, 12 + levels * 2)
cp_off = 12 + levels * 4
cps = struct.unpack_from('<%dH' % n, data, cp_off)
cp_index = {cp: i for i, cp in enumerate(cps)}
bmp_off = (cp_off + 2 * n + 3) & ~3
lvl_base, off = [], bmp_off
for i in range(levels):
    lvl_base.append(off)
    off += n * strides[i] * cells[i]
print('bin=%s ver=%d levels=%s cells=%s strides=%s n=%d' %
      (BIN, ver, levels, cells, strides, n))


def glyph_rows(ch, level):
    i = cp_index.get(ord(ch))
    if i is None:
        return None
    base = lvl_base[level] + i * strides[level] * cells[level]
    rows = []
    for gy in range(cells[level]):
        rowbits = 0
        for gx in range(cells[level]):
            byte = data[base + gy * strides[level] + gx // 8]
            if byte & (0x80 >> (gx % 8)):
                rowbits |= 1 << (cells[level] - 1 - gx)
        rows.append(rowbits)
    return rows


def ink_cols(rows):
    """返回每行最右墨迹列（-1=空行），观察右侧是否有墨迹。"""
    return [r.bit_length() - 1 if r else -1 for r in rows]


def render_ascii(level):
    cell = cells[level]
    print('\n--- level %d (%dpx, stride %dB) 样例「%s」---' %
          (level, cell, strides[level], SAMPLE))
    cache = [glyph_rows(ch, level) for ch in SAMPLE]
    # 每字墨迹统计：最左/最右墨迹列（全字所有行聚合）
    for ch, rows in zip(SAMPLE, cache):
        if rows is None:
            print(' %s 未收录' % ch)
            continue
        lm = cell
        rm = -1
        for r in rows:
            if r:
                lm = min(lm, cell - r.bit_length())
                rm = max(rm, cell - 1 - (r & -r).bit_length() + 1 - 1)
                # 最右墨迹列 = cell-1 - 前导零(低位侧)
                low = (r & -r).bit_length() - 1
                rm = max(rm, cell - 1 - low)
        print(' %s: 墨迹列 [%d, %d] / cell宽 %d  右空余 %d' %
              (ch, lm, rm, cell, cell - 1 - rm))
    # ASCII art 输出（每字并排）
    for gy in range(cell):
        line = []
        for rows in cache:
            if rows is None:
                line.append(' ' * cell)
                continue
            r = rows[gy]
            line.append(''.join('#' if (r >> (cell - 1 - gx)) & 1 else '.'
                                for gx in range(cell)))
        print(' '.join(line))


for lv in range(levels):
    render_ascii(lv)
