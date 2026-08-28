#!/usr/bin/env python3
"""归类产物落地：/tmp/default_words_units.csv → 三处正式产物（2026-08-28）。

前置：textbook_units.py v3 已跑出 /tmp/default_words_units.csv（只改
source/grade 两列）。本脚本做落地前的单元名修正与全量校验，再写出：

  1. InkWord_Backend/src/InkWord.API/SeedData/default_words.csv（种子）
  2. tools/default_vocab/out/default_words.csv（生成产物目录同步）
  3. InkWord_Firmware/src/default_words.json（固件内嵌，write_json 契约
     派生：id 递增/difficulty 数字/空字段省略/version=条数/compact）
  4. tools/default_vocab/out/default_words.json（同上同步）

修正（v3 运行后 BOOKS 收紧，此处同步替换已有 source 值）：
  - 八上 U10 话题去英文逗号（后端 Split(',') 无转义，逗号撕裂列）
  - 九 U7 话题缩短（source 63B 超 CSV 60B 安全线，固件 WORD_SOURCE_MAX=64 无余量）

校验：2408 行、11 列、除 source/grade 外与原 CSV 全同、语文行不动、
source≤60B/grade≤20B、无英文逗号/引号/控制符。
"""
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "../..")   # InkWord_Firmware/tools → 工作区根
CSV_TMP = "/tmp/default_words_units.csv"
CSV_BACKEND = os.path.join(ROOT, "InkWord_Backend/src/InkWord.API/SeedData/default_words.csv")
CSV_OUT_DIR = os.path.join(ROOT, "tools/default_vocab/out")
JSON_FW = os.path.join(ROOT, "InkWord_Firmware/src/default_words.json")
CSV_ORIG = CSV_BACKEND   # 原样对照（落地前）

FIXUPS = {
    "Unit 10 If you go to the party, you'll have a great time!":
        "Unit 10 If you go to the party you'll have a great time!",
    "Unit 7 Teenagers should be allowed to choose their own clothes.":
        "Unit 7 Teenagers should be allowed to choose clothes.",
}

MAX = {"source": 60, "grade": 20}   # 字节（gen_default_vocab -4B 余量线）
COLS = ("text", "phonetic", "meaning", "example", "audio", "tag",
        "difficulty", "root", "inflections", "source", "grade")
EXPECTED_ROWS = 2408                 # 表头 1 + 数据 2407


def main():
    with open(CSV_TMP, newline="", encoding="utf-8") as f:
        rows = [r for r in csv.reader(f)]
    with open(CSV_ORIG, newline="", encoding="utf-8") as f:
        orig = [r for r in csv.reader(f)]
    hdr, data = rows[0], rows[1:]
    assert hdr == list(COLS), f"表头不符: {hdr}"
    assert len(rows) == EXPECTED_ROWS == len(orig), \
        f"行数异常: {len(rows)} vs {orig and len(orig)}"

    errs = []
    for i, r in enumerate(data):
        if len(r) != 11:
            errs.append(f"第{i + 2}行列数={len(r)}")
            continue
        r[9] = FIXUPS.get(r[9], r[9])
        r[10] = FIXUPS.get(r[10], r[10])
        for col, val in ((9, r[9]), (10, r[10])):
            if len(val.encode("utf-8")) > MAX[COLS[col]]:
                errs.append(f"第{i + 2}行 {COLS[col]} 超限: {val[:30]}")
        if re.search(r'[,"\r\n\x00-\x1f\x7f]', r[9] + r[10]):
            errs.append(f"第{i + 2}行 source/grade 含禁字符")
        # 除 9/10 列外与原行全同（text 序稳定）
        if any(r[j] != orig[i + 1][j] for j in range(11) if j not in (9, 10)):
            errs.append(f"第{i + 2}行除 source/grade 外有差异")
    if errs:
        for e in errs[:10]:
            print("[ERR]", e)
        sys.exit(f"校验失败 {len(errs)} 处，未写出任何文件")

    # 无引号写出（Split(',') 语义：字段已保证无逗号/引号/换行）
    body = ",".join(COLS) + "\n" + "".join(",".join(r) + "\n" for r in data)
    for path in (CSV_BACKEND, os.path.join(CSV_OUT_DIR, "default_words.csv")):
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)

    # 固件 json 派生（gen_default_vocab.write_json 契约）
    words = []
    for i, r in enumerate(data, 1):
        f = dict(zip(COLS, r))
        obj = {"id": i, "text": f["text"], "difficulty": int(f["difficulty"])}
        for k in COLS:
            if k not in obj and f[k]:
                obj[k] = f[k]
        words.append(obj)
    jbody = json.dumps({"version": len(words), "words": words},
                       ensure_ascii=False, separators=(",", ":"))
    for path in (JSON_FW, os.path.join(CSV_OUT_DIR, "default_words.json")):
        with open(path, "w", encoding="utf-8") as f:
            f.write(jbody)

    # 落地后统计
    from collections import Counter
    per_grade = Counter(r[10] for r in data if r[9].startswith(("Unit", "Starter")))
    print(f"落地完成：{len(data)} 词；单元细分分布")
    for g in ("七年级上", "七年级下", "八年级上", "八年级下", "九年级"):
        print(f"  {g}: {per_grade.get(g, 0)}")
    print(f"  考纲拓展: {sum(1 for r in data if r[10] == '考纲拓展')}")
    print(f"  小学已学归 Starter: "
          f"{sum(1 for r in data if r[9].startswith('Starter'))}")
    print(f"产物：\n  {CSV_BACKEND}\n  {JSON_FW}\n  {CSV_OUT_DIR}/default_words.{{csv,json}}")


if __name__ == "__main__":
    main()
