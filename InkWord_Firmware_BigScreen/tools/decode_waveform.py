#!/usr/bin/env python3
"""离线解码 epdiy 波形头，核对指定 (to,from) 组合在各相位的 2bit 驱动码。

码值语义（components/epdiy/src/output_common/lut.h）：
  0 = nop（0b00，fill_line_noop）
  1 = 驱黑（DARK_BYTE 0b01010101）
  2 = 驱白（CLEAR_BYTE 0b10101010）
  3 = 浮空/双驱

波形布局（lut.c waveform_lut_64k）：data[phase][to][from_packed]，
每字节打包 4 个 from，code = (byte >> (6 - 2*(from%4))) & 3。
"""
import re
import sys

HDR = sys.argv[1] if len(sys.argv) > 1 else \
    "components/epdiy/src/waveforms/epdiy_ED047TC1.h"
MODE = sys.argv[2] if len(sys.argv) > 2 else "2"   # GC16 = type 2

src = open(HDR, encoding="utf-8", errors="replace").read()

# data[PHASES][16][4]
m = re.search(
    r"epd_wp_epdiy_ED047TC1_%s_0_data\[\d+\]\[16\]\[4\]\s*=\s*(\{.*?\});" % MODE,
    src, re.S)
assert m, "data array not found for mode %s" % MODE
flat = [int(x, 16) for x in re.findall(r"0[xX][0-9a-fA-F]+", m.group(1))]
n_phases = len(flat) // 64
assert len(flat) == n_phases * 64, len(flat)

mt = re.search(r"epd_wp_epdiy_ED047TC1_%s_0_times\[\d+\]\s*=\s*\{(.*?)\}" % MODE, src, re.S)
times = [int(x) for x in re.findall(r"-?\d+", mt.group(1))]


def code(phase, to, frm):
    byte = flat[phase * 64 + to * 4 + (frm >> 2)]
    return (byte >> (6 - 2 * (frm & 3))) & 3


def seq(to, frm):
    return [code(p, to, frm) for p in range(n_phases)]


def net(codes, times_):
    """净驱动量：白(2) 记 +time，黑(1) 记 -time，单位 ms"""
    return sum(t * (1 if c == 2 else -1 if c == 1 else 0)
               for c, t in zip(codes, times_))


print("mode=%s phases=%d times=%s" % (MODE, n_phases, times))
print("总时长 %dms" % sum(times))
print()
cases = [
    ("黑->白 to=15 from=0", 15, 0),
    ("白->黑 to=0  from=15", 0, 15),
    ("白->白 to=15 from=15 (nop?)", 15, 15),
    ("黑->黑 to=0  from=0", 0, 0),
    ("灰 to=8  from=0", 8, 0),
    ("to=15 from=8", 15, 8),
]
for name, to, frm in cases:
    s = seq(to, frm)
    print("%-32s codes=%s" % (name, "".join(str(c) for c in s)))
    print("%-32s 白帧数=%d 黑帧数=%d nop帧数=%d 净=%+dms" %
          ("", s.count(2), s.count(1), s.count(0), net(s, times)))
    print()

# 全 16x16 矩阵的净驱动量（末相位），快速看有无异常行
print("=== 净驱动量矩阵 (行=to, 列=from, 单位 ms) ===")
print("to\\from " + "".join("%7d" % f for f in range(16)))
for to in range(16):
    print("%5d " % to + "".join("%7d" % net(seq(to, f), times) for f in range(16)))
