#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FSRS-4.5 测试向量生成器（M3/M4 双端对拍基准，2026-08-22）。

按 open-spaced-repetition FSRS-4.5 默认 17 参数独立实现，模拟 12 组复习
序列，固化为 tools/fsrs_test_vectors.csv。后端 FsrsServiceTests（C#）与
固件 test/test_srs_engine.c（C）读同一份 CSV 对拍——对拍目的是双端实现
一致性与回归防护，非算法正确性证明。

公式与 FsrsService.cs / srs_engine.c 严格同源：
  S0(r) = W[r-1]；D0(r) = clamp(W4 - e^(W5*(r-1)), 1, 10)
  R(t,S) = (1 + FACTOR*t/S)^DECAY，DECAY=-0.5，FACTOR=19/81
  遗忘(r=1)：S' = W11*D^(-W12)*((S+1)^W13 - 1)*e^(W14*(1-R))
  回忆(r≥2)：S' = S*(1 + e^W8*(11-D)*S^(-W9)*(e^(W10*(1-R))-1)*HP*EB)
    HP = W15 (r=Hard)，EB = W16 (r=Easy)
  D' = clamp(D - W6*(r-3), 1, 10)
  interval = max(1, round(S*(R*^(1/DECAY)-1)/FACTOR))，R*=0.90 时 = round(S)

CSV 列：case,step,quality,elapsed_days,expect_s,expect_d,expect_interval
  elapsed_days = 距上一次复习的天数（按时复习序列 = 上一步 interval）。
  首步 elapsed 恒 0（S=0 走初始化分支，不依赖 elapsed）。
  s/d 保留 6 位小数（对拍容差：S/D 相对 1e-4，interval 整数精确相等）。

重放语义（双端一致）：
  1. 初始 S=0, D=0，last_review = T0
  2. 每步：在 T0 + Σ(elapsed) 时刻评分 → 得新 S/D/interval
     （影子接口：NextState(S, D, last_review, q, now)）
"""

import csv
import math
import os

W = [0.4872, 1.4003, 3.7145, 13.8206, 5.1618, 1.2298, 0.8975, 0.031,
     1.6474, 0.1367, 1.0461, 2.1072, 0.0793, 0.3246, 1.587, 0.2272, 2.8755]
DECAY = -0.5
FACTOR = 19.0 / 81.0
R_STAR = 0.90


def map_rating(q):
    """quality 0~5 → rating 1=Again 2=Hard 3=Good 4=Easy（双端同映射）"""
    if q <= 1:
        return 1
    if q == 2:
        return 2
    if q >= 5:
        return 4
    return 3


def retrievability(s, t):
    return (1.0 + FACTOR * t / s) ** DECAY


def next_interval(s):
    return max(1, round(s * (R_STAR ** (1.0 / DECAY) - 1.0) / FACTOR))


def next_state(s, d, elapsed_days, q):
    r = map_rating(q)
    if s <= 0:
        s = W[r - 1]
        d = min(10.0, max(1.0, W[4] - math.exp(W[5] * (r - 1))))
    else:
        R = min(0.999, max(0.005, retrievability(s, elapsed_days)))
        if r == 1:
            s = W[11] * d ** (-W[12]) * ((s + 1.0) ** W[13] - 1.0) \
                * math.exp(W[14] * (1.0 - R))
        else:
            hp = W[15] if r == 2 else 1.0
            eb = W[16] if r == 4 else 1.0
            s = s * (1.0 + math.exp(W[8]) * (11.0 - d) * s ** (-W[9])
                     * (math.exp(W[10] * (1.0 - R)) - 1.0) * hp * eb)
        s = max(0.01, s)
        d = min(10.0, max(1.0, d - W[6] * (r - 3)))
    return s, d, next_interval(s)


# 12 组序列：覆盖四档评分、遗忘恢复、混合模式、长序列
CASES = [
    ("good-run",       [3, 3, 3, 3, 3, 3]),
    ("easy-run",       [5, 5, 5, 5, 5, 5]),
    ("hard-run",       [2, 2, 2, 2, 2, 2]),
    ("again-run",      [0, 0, 0, 0, 0, 0]),
    ("lapse-recover",  [3, 3, 0, 3, 4, 3]),
    ("mixed-wide",     [5, 2, 4, 1, 3, 5]),
    ("again-edge-q1",  [1, 1, 3, 3, 3, 3]),
    ("good-high-q4",   [4, 4, 4, 4, 4, 4]),
    ("altern-ge",      [3, 5, 3, 5, 3, 5]),
    ("altern-ha",      [2, 0, 2, 0, 2, 0]),
    ("long-good",      [3, 3, 3, 3, 3, 3, 3, 3, 3, 3]),
    ("double-lapse",   [3, 0, 0, 3, 3, 3]),
]


def main():
    rows = []
    for name, seq in CASES:
        s, d = 0.0, 0.0
        elapsed = 0  # 首步 S=0 走初始化，elapsed 无效
        for step, q in enumerate(seq):
            s, d, interval = next_state(s, d, elapsed, q)
            rows.append({
                "case": name,
                "step": step,
                "quality": q,
                "elapsed_days": elapsed,
                "expect_s": f"{s:.6f}",
                "expect_d": f"{d:.6f}",
                "expect_interval": interval,
            })
            elapsed = interval  # 按时复习：下次 elapsed = 本次排期间隔

    out = "fsrs_test_vectors.csv"
    with open(out, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {len(rows)} rows -> {out}")

    # 固件内嵌向量头（on-target 测试无文件系统，嵌入编译）
    fw = os.path.join(os.path.dirname(__file__) or ".",
                      "..", "InkWord_Firmware", "test", "fsrs_vectors.h")
    os.makedirs(os.path.dirname(fw), exist_ok=True)
    with open(fw, "w", encoding="utf-8") as f:
        f.write("/* 本文件由 tools/gen_fsrs_vectors.py 自动生成，勿手改；\n")
        f.write(" * 与 tools/fsrs_test_vectors.csv 同源（改向量先重跑脚本）。 */\n")
        f.write("#ifndef INKWORD_TEST_FSRS_VECTORS_H\n")
        f.write("#define INKWORD_TEST_FSRS_VECTORS_H\n\n")
        f.write("typedef struct {\n")
        f.write("    const char *case_name;\n")
        f.write("    int step, quality, elapsed_days, interval;\n")
        f.write("    double s, d;\n")
        f.write("} fsrs_vec_t;\n\n")
        f.write(f"#define FSRS_VEC_COUNT {len(rows)}\n")
        f.write("static const fsrs_vec_t kFsrsVectors[FSRS_VEC_COUNT] = {\n")
        for r in rows:
            f.write(f"    {{ \"{r['case']}\", {r['step']}, {r['quality']}, "
                    f"{r['elapsed_days']}, {r['expect_interval']}, "
                    f"{r['expect_s']}, {r['expect_d']} }},\n")
        f.write("};\n\n#endif /* INKWORD_TEST_FSRS_VECTORS_H */\n")
    print(f"wrote {len(rows)} rows -> {os.path.normpath(fw)}")

    # 锚点自检（公开已知事实，防止参考实现自身写错）
    assert abs(next_state(0, 0, 0, 3)[0] - W[2]) < 1e-12, "S0(Good)=W[2]"
    assert next_state(0, 0, 0, 3)[1] == 1.0, "D0(Good)=clamp(...)=1.0"
    assert next_interval(3.7145) == 4, "interval=round(S) @ R*=0.9"
    assert map_rating(0) == 1 and map_rating(5) == 4, "quality 映射"
    print("anchor checks passed")


if __name__ == "__main__":
    main()
