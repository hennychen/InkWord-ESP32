#!/usr/bin/env python3
"""生成 16 级灰阶标板裸流，供 LAN /upload 验证与扫描量子化波形标定。

标板：16 条竖带（各 120px 宽 × 1080 高），左→右 nibble 0..15（0=黑、15=白）。
输出（与 lan_image.c 接收格式逐位一致）：
  gray_ramp_gray16.raw  1036800B  4bpp（偶 x=低半字节 / 奇 x=高半字节）
  gray_ramp_1bit.raw     259200B  1bit FS 抖动（MSB first，bit1=白）

用法：
  python3 tools/gen_gray_ramp.py [outdir]
  curl -X POST --data-binary @gray_ramp_gray16.raw http://192.168.4.1/upload
  curl -X POST --data-binary @gray_ramp_1bit.raw   http://192.168.4.1/upload

判读（bringup 文档 §15）：
  dither=off + gray16 上传 = 塌缩签名：仅带 0 黑、带 1~4 渐隐、带 >=5 全白；
  dither=on  或 1bit 上传 = 16 带应以抖动纹理全部可辨（二值跃迁免疫相位过驱）。
"""
import os
import sys

W, H = 1920, 1080
BAND = W // 16


def gray_of(x):
    return (min(x // BAND, 15)) * 17          # nibble 0..15 -> 0..255


def pack_gray16():
    buf = bytearray(W // 2 * H)
    for y in range(H):
        row = y * (W // 2)
        for x in range(0, W, 2):
            q0 = gray_of(x) * 15 // 255        # 还原 nibble（v=q*17 的逆映射取整一致）
            q1 = gray_of(x + 1) * 15 // 255
            buf[row + x // 2] = q0 | (q1 << 4)
    return bytes(buf)


def pack_1bit_fs():
    buf = bytearray(W // 8 * H)
    err_cur = [0] * (W + 2)
    err_nxt = [0] * (W + 2)
    for y in range(H):
        for x in range(W):
            old = gray_of(x) + err_cur[x]
            b = 1 if old >= 128 else 0
            e = old - (255 if b else 0)
            if b:
                buf[(y * W + x) >> 3] |= 0x80 >> (x & 7)
            if x + 1 < W:
                err_cur[x + 1] += e * 7 // 16
            if y + 1 < H:
                if x > 0:
                    err_nxt[x - 1] += e * 3 // 16
                err_nxt[x] += e * 5 // 16
                if x + 1 < W:
                    err_nxt[x + 1] += e // 16
        err_cur, err_nxt = err_nxt, [0] * (W + 2)
    return bytes(buf)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    g16 = pack_gray16()
    b1 = pack_1bit_fs()
    p16 = os.path.join(outdir, "gray_ramp_gray16.raw")
    p1 = os.path.join(outdir, "gray_ramp_1bit.raw")
    open(p16, "wb").write(g16)
    open(p1, "wb").write(b1)
    print("wrote %s (%d B)" % (p16, len(g16)))
    print("wrote %s (%d B)" % (p1, len(b1)))
    print("curl -X POST --data-binary @%s http://192.168.4.1/upload" % p16)
    print("curl -X POST --data-binary @%s http://192.168.4.1/upload" % p1)


if __name__ == "__main__":
    main()
