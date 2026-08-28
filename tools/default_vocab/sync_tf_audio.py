#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TF 卡音频校验/同步（2026-08-28，单元细分后核对）。

对比 SD 卡 /sdcard/audio/（TF 卡插 Mac 读卡器后挂载于 /Volumes/...）
与本地部署源 tools/default_vocab/out/audio/（2134 文件 = 2140 词 +
6 组同音共用），词表基准为 out/default_words.csv（audio 列即需求清单）。

用法：
  python3 sync_tf_audio.py              # 自动探测 TF 卷，只读校验
  python3 sync_tf_audio.py /Volumes/XXX # 指定卷路径
  python3 sync_tf_audio.py --sync       # 校验 + 补齐缺失（cp 单文件）
  python3 sync_tf_audio.py --sync --prune-extra  # 同时清理多余文件

判定规则：
  - 缺失：词表 audio 列所需文件在 TF 卡不存在（或 0 字节 → 视为损坏）
  - 多余：TF 卡有但词表无需（提示，默认不删）
  - 大小差异不判损（来源重抓规格可能微差），以 0 字节/缺失为准
"""
import csv
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "out")
SRC_AUDIO = os.path.join(OUT_DIR, "audio")
SRC_CSV = os.path.join(OUT_DIR, "default_words.csv")


def is_cjk(s):
    return any("\u4e00" <= ch <= "\u9fff" for ch in s)


def load_needs():
    """词表音频需求：{文件名: [text...]}（同音共用组多词共文件）。"""
    with open(SRC_CSV, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    needs = {}
    for r in rows[1:]:
        if is_cjk(r[0]):
            continue
        audio = r[4].strip()
        if audio:
            needs.setdefault(audio, []).append(r[0])
    return needs


def detect_volume():
    """自动探测 TF 卷：排除系统盘，优先含 audio/ 或 decks/ 的 FAT 卷。"""
    cands = []
    for v in os.listdir("/Volumes"):
        if v.startswith("."):
            continue
        root = os.path.join("/Volumes", v)
        if not os.path.isdir(root) or v == "Macintosh HD":
            continue
        score = 0
        if os.path.isdir(os.path.join(root, "audio")):
            score += 2
        if os.path.isdir(os.path.join(root, "decks")):
            score += 1
        cands.append((score, root))
    if not cands:
        return None
    cands.sort(reverse=True)
    return cands[0][1]


def main():
    args = [a for a in sys.argv[1:]]
    do_sync = "--sync" in args
    prune = "--prune-extra" in args
    paths = [a for a in args if not a.startswith("--")]
    vol = paths[0] if paths else detect_volume()
    if not vol or not os.path.isdir(vol):
        sys.exit("未找到 TF 卷：请插入读卡器后重试，或显式指定路径"
                 "（如 python3 sync_tf_audio.py /Volumes/NO\\ NAME）")
    tf_audio = os.path.join(vol, "audio")
    print(f"TF 卷：{vol}")
    if not os.path.isdir(tf_audio):
        if do_sync:
            os.makedirs(tf_audio, exist_ok=True)
            print(f"已创建 {tf_audio}")
        else:
            sys.exit(f"TF 卡无 audio/ 目录（{tf_audio}）；--sync 可创建并全量拷贝")

    needs = load_needs()
    src_files = set(os.listdir(SRC_AUDIO)) if os.path.isdir(SRC_AUDIO) else set()
    tf_files = set(os.listdir(tf_audio))

    missing = sorted(f for f in needs if f not in tf_files
                     or os.path.getsize(os.path.join(tf_audio, f)) == 0)
    extra = sorted(tf_files - set(needs))

    # 词维度报告（共用组任缺文件算全部受影响）
    miss_words = [w for f in missing for w in needs.get(f, [])]
    print(f"\n== 校验结果 ==")
    print(f"词表需求文件 {len(needs)}（{sum(len(v) for v in needs.values())} 词）")
    print(f"TF 卡现有 audio 文件 {len(tf_files)}")
    print(f"缺失/损坏 {len(missing)} 文件（影响 {len(miss_words)} 词）")
    for f in missing[:30]:
        print(f"  - {f}  <- {', '.join(needs.get(f, []))}")
    if len(missing) > 30:
        print(f"  ...（共 {len(missing)}）")
    print(f"多余文件 {len(extra)}（词表已无此词）")
    for f in extra[:10]:
        print(f"  + {f}")

    if not do_sync:
        if missing:
            print("\n补齐：python3 sync_tf_audio.py --sync")
        else:
            print("\nTF 卡音频与词表完全匹配，无需补充。")
        return

    ok = fail = 0
    for f in missing:
        if f not in src_files:
            print(f"[SKIP] 本地源也缺 {f}（先跑 fetch_audio.py api/norm）")
            continue
        src = os.path.join(SRC_AUDIO, f)
        dst = os.path.join(tf_audio, f)
        tmp = dst + ".tmp"
        try:
            shutil.copy2(src, tmp)
            os.replace(tmp, dst)
            ok += 1
        except OSError as e:
            print(f"[ERR] {f}: {e}")
            fail += 1
    print(f"\n同步完成：拷贝 {ok} / 失败 {fail}；缺失清零："
          f"{not [f for f in needs if not os.path.exists(os.path.join(tf_audio, f))]}")

    if prune and extra:
        n = 0
        for f in extra:
            p = os.path.join(tf_audio, f)
            try:
                os.remove(p)
                n += 1
            except OSError as e:
                print(f"[ERR] 删除 {f}: {e}")
        print(f"清理多余文件 {n} 个")


if __name__ == "__main__":
    main()
