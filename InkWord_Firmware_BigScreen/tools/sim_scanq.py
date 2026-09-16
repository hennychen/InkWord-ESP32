#!/usr/bin/env python3
"""scanq 扫描量子化波形离线模拟/自检（与 firmware src/lan/waveform_scanq.c 同构）。

功能：
  1. 按 nsat/mmax/map 重建 LUT 码表，做「打包→解包」往返自检
     （打包：byte=from>>2, shift=6-2*(from&3)，与 tools/decode_waveform.py 同式）；
  2. 打印每个目标灰阶 to 的剂量表（白修扫描数 / 白剂量 ms / 净黑剂量 ms），
     与原 ED047TC1 GC16 意图剂量对照（§15.2：白满剂量 550ms、黑段 1020ms）；
  3. 单调性/饱和检查与标定 curl 序列提示。

用法：
  python3 tools/sim_scanq.py                          # 固件默认参数
  python3 tools/sim_scanq.py --nsat 10 --mmax 5 \
      --map 0,0,1,1,1,2,2,2,3,3,3,4,4,4,5,5
"""
import argparse

SCAN_MS = 110          # 实测单扫描时长（§4 帧节奏）
# 原 ED047TC1 GC16 各 to 的白修意图剂量 ms（decode_waveform.py 净驱动矩阵导出）
ORIG_INTENT = {0: 0, 1: 10, 2: 20, 3: 28, 4: 36, 5: 44, 6: 52, 7: 60,
               8: 70, 9: 80, 10: 90, 11: 100, 12: 120, 13: 140, 14: 240, 15: 550}


def build(nsat, mmax, mapp):
    k = nsat + mmax
    lut = [[[0] * 16 for _ in range(16)] for _ in range(k)]  # [phase][to][from]
    for p in range(k):
        for to in range(16):
            for frm in range(16):
                if p < nsat:
                    lut[p][to][frm] = 0 if frm == 0 else 1
                else:
                    lut[p][to][frm] = 2 if (p - nsat) < mapp[to] else 0
    return lut


def pack(lut):
    data = [[[0] * 4 for _ in range(16)] for _ in range(len(lut))]
    for p, phase in enumerate(lut):
        for to in range(16):
            for frm in range(16):
                data[p][to][frm >> 2] |= phase[to][frm] << (6 - 2 * (frm & 3))
    return data


def unpack_code(data, p, to, frm):
    return (data[p][to][frm >> 2] >> (6 - 2 * (frm & 3))) & 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nsat", type=int, default=10)
    ap.add_argument("--mmax", type=int, default=5)
    ap.add_argument("--map", default="0,0,1,1,1,2,2,2,3,3,3,4,4,4,5,5")
    a = ap.parse_args()
    mapp = [int(v) for v in a.map.split(",")]
    assert len(mapp) == 16, "map 须 16 个整数"
    assert all(0 <= m <= a.mmax for m in mapp), "map 值域 0..mmax"

    lut = build(a.nsat, a.mmax, mapp)
    data = pack(lut)
    # 往返自检：解包必须与源码表逐位一致
    for p in range(len(lut)):
        for to in range(16):
            for frm in range(16):
                assert unpack_code(data, p, to, frm) == lut[p][to][frm], \
                    "roundtrip fail p=%d to=%d frm=%d" % (p, to, frm)
    print("packing roundtrip OK: phases=%d (%d+%d), lut=%dB"
          % (a.nsat + a.mmax, a.nsat, a.mmax, (a.nsat + a.mmax) * 64))

    print("\n to | map(to) | 白剂量ms | 净黑剂量ms(from=15) | 原意图白剂量ms")
    for to in range(16):
        m = mapp[to]
        white_ms = m * SCAN_MS
        net_black = (a.nsat - m) * SCAN_MS   # from>0：A 段全黑 + B 段 m 白
        print("%3d | %7d | %8d | %19d | %14d"
              % (to, m, white_ms, net_black, ORIG_INTENT[to]))

    distinct = sorted(set(mapp))
    print("\n可区分灰级数（量子=%dms）: %d -> map 值集 %s"
          % (SCAN_MS, len(distinct), distinct))
    for t in range(1, 16):
        if mapp[t] < mapp[t - 1]:
            print("WARN: map 非单调 to=%d" % t)
            break
    if mapp[15] * SCAN_MS < ORIG_INTENT[15]:
        print("WARN: to=15 白剂量 %dms < 原意图 550ms，可能回不到全白"
              % (mapp[15] * SCAN_MS))

    print("\n标定序列（SoftAP 192.168.4.1）：")
    print("  curl 'http://192.168.4.1/dither'                 # 关兜底 -> dither=0")
    print("  curl 'http://192.168.4.1/wf?wf=scanq'            # 切 scanq")
    print("  curl 'http://192.168.4.1/wf?nsat=%d&mmax=%d&map=%s'"
          % (a.nsat, a.mmax, a.map))
    print("  curl -X POST --data-binary @gray_ramp_gray16.raw http://192.168.4.1/upload")
    print("  # 拍照判读 16 带层次 → 调 map 重传（免重烧）；收敛后 /dither 恢复")


if __name__ == "__main__":
    main()
