#!/usr/bin/env python3
"""生成 InkWord 系统默认词库 CSV。

数据源（.raw/，脚本 download.py 下载，不入 git）：
  英语（qwerty-learner，GPL-3.0，public/dicts/*.json，字段 name/usphone/ukphone/trans）
    ZhongKaoHeXin.json        中考核心词汇 2140 词
    GaoKao_3500.json          新课标高考 3500 词
    PEPXiaoXue3_1~6_2         人教版小学（三年级起点）8 册
    PEPChuZhong7_1~9_2        人教版初中 6 册
    PEPGaoZhong_1~11          人教版高中 11 册
  语文
    primary_data.js           小学必背古诗词 92 首（MIZHANG08/AncientPoemsPrimary）
    poetry_chuzhong.json      初中古诗文 118 篇（tangyuan0821/Junior-Middle-School-poetry，CC-BY-SA-4.0）
    poetry_gaokao.json        高考必背古诗文 60 篇（clover-yan/gaokao-poetry，CC-BY-SA-4.0）

产物（out/）：
  default_words.csv          系统默认组合（≈2400 条，设备端全库同步硬上限 4000）
  default_words.json         固件内嵌兜底词库（embed 进固件，无 SD 卡开箱即用）
  subdicts/PEP_*.csv         教材分册词库（按需经管理端导入）

CSV 列序对齐 AdminWordController.ImportCsv（后端 line.Split(',') 简单分割，
无引号转义）：字段内英文逗号/引号/换行必须清洗掉。字段字节上限对齐固件
word_parser.h（各宏值减 4B 余量）。
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, ".raw")
OUT = os.path.join(HERE, "out")

# ---- 字段字节上限（对齐固件 word_parser.h，-4B 安全余量）----
MAX = {"text": 60, "phonetic": 60, "meaning": 252, "example": 252,
       "audio": 92, "tag": 28, "root": 92, "inflections": 92,
       "source": 60, "grade": 20}

HEADER = ("text,phonetic,meaning,example,audio,tag,difficulty,"
          "root,inflections,source,grade")

DEVICE_TOTAL_MAX = 4000  # 固件 MAX_WORDS：全库同步硬上限


def clean(s):
    """清洗字段：后端 Split(',') 无转义，禁英文逗号/引号/换行/控制符。"""
    s = (s or "").replace("\r", " ").replace("\n", " ")
    s = s.replace('"', "'").replace(",", "，")
    s = re.sub(r"[\x00-\x1f\x7f]", "", s)
    return s.strip()


def trunc(s, limit):
    """UTF-8 字节安全截断。"""
    s = clean(s)
    while s and len(s.encode("utf-8")) > limit:
        s = s[:-1]
    return s


def row(text, phonetic, meaning, example, tag, difficulty, source, grade):
    """构造一行 11 列（audio/root/inflections 默认空）。"""
    f = {
        "text": trunc(text, MAX["text"]),
        "phonetic": trunc(phonetic, MAX["phonetic"]),
        "meaning": trunc(meaning, MAX["meaning"]),
        "example": trunc(example, MAX["example"]),
        "audio": "",
        "tag": trunc(tag, MAX["tag"]),
        "difficulty": str(difficulty),
        "root": "",
        "inflections": "",
        "source": trunc(source, MAX["source"]),
        "grade": trunc(grade, MAX["grade"]),
    }
    return ",".join(f[k] for k in ("text", "phonetic", "meaning", "example",
                                   "audio", "tag", "difficulty", "root",
                                   "inflections", "source", "grade"))


# ---- 英语（qwerty-learner JSON）----

def load_english(fname):
    with open(os.path.join(RAW, fname), encoding="utf-8") as f:
        return json.load(f)


def english_rows(items, tag, difficulty, source, grade):
    rows, seen = [], set()
    for w in items:
        text = clean(w.get("name", ""))
        if not text or (tag, text) in seen:
            continue
        seen.add((tag, text))
        phone = w.get("usphone") or w.get("ukphone") or ""
        meaning = "；".join(w.get("trans") or [])
        rows.append(row(text, phone, meaning, "", tag, difficulty,
                        source, grade))
    return rows


# ---- 语文（title/author/content）----

def load_poetry_json(fname):
    with open(os.path.join(RAW, fname), encoding="utf-8") as f:
        return json.load(f)


def load_primary_js():
    """primary_data.js 为 JS 常量数组，正则提取 title/author/content 三元组。"""
    with open(os.path.join(RAW, "primary_data.js"), encoding="utf-8") as f:
        s = f.read()
    poems = []
    for m in re.finditer(
            r'title:\s*"([^"]*)",\s*author:\s*"([^"]*)",\s*'
            r'content:\s*"([^"]*)"', s):
        poems.append({"title": m.group(1), "author": m.group(2),
                      "content": m.group(3)})
    return poems


def first_sentence(content):
    """取首句（首个句末标点前，含标点）作 Example。"""
    m = re.match(r"^[^。！？!?]*[。！？!?]?", content)
    return m.group(0) if m else content[:24]


def poetry_rows(poems, tag, difficulty, source, grade):
    rows, seen = [], set()
    for p in poems:
        text = clean(p.get("title", ""))
        if not text or (tag, text) in seen:
            continue
        seen.add((tag, text))
        author = clean(p.get("author", ""))
        content = clean(p.get("content", ""))
        # 全文超 Meaning 上限时截断并标注省略
        if len(content.encode("utf-8")) > MAX["meaning"]:
            cut = trunc(content, MAX["meaning"] - len("……".encode()))
            meaning = cut + "……"
        else:
            meaning = content
        rows.append(row(text, author, meaning, first_sentence(content),
                        tag, difficulty, source, grade))
    return rows


# ---- 分册元数据（PEP 文件名 → Tag/Grade/Source）----

PEP_META = (
    [("PEPXiaoXue%d_%d_T.json" % (g, v),
      "人教版%s" % {3: "三", 4: "四", 5: "五", 6: "六"}[g] + ("上" if v == 1 else "下"),
      "%s年级" % {3: "三", 4: "四", 5: "五", 6: "六"}[g],
      "人教版PEP %s年级%s册" % ({3: "三", 4: "四", 5: "五", 6: "六"}[g],
                                  "上" if v == 1 else "下"))
     for g in (3, 4, 5, 6) for v in (1, 2)]
    + [("PEPChuZhong%d_%d_T.json" % (g, v),
        "人教版%s%s" % ({7: "七", 8: "八", 9: "九"}[g],
                        "上" if v == 1 else "下"),
        "%s年级" % {7: "七", 8: "八", 9: "九"}[g],
        "人教版 %s年级%s册" % (g, "上" if v == 1 else "下"))
       for g, v in ((7, 1), (7, 2), (8, 1), (8, 2), (9, 1))]
    + [("PEPGaoZhong_%d_T.json" % g,
        "人教版高中必修%d" % g if g <= 5 else "人教版高中选修%d" % g,
        "高中", "人教版 高中%s%d" % ("必修" if g <= 5 else "选修", g))
       for g in range(1, 12)]
)


def write_csv(path, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(HEADER + "\n")
        f.writelines(r + "\n" for r in rows)


COLS = ("text", "phonetic", "meaning", "example", "audio", "tag",
        "difficulty", "root", "inflections", "source", "grade")


def write_json(path, rows):
    """固件内嵌词库 JSON（从同源 CSV 行派生，保证两产物同源同序）。

    契约对齐后端 /admin/words/export 与固件 word_parser：
    - id/difficulty 为数字（cJSON valueint 路径）；version = 条数
    - 空字段整键省略（解析器缺键天然落空串，见 word_parser.c 注释）
    - 不含 cloudId：内嵌为出厂兜底，云端 Guid 以在线同步/导出路径
      下发为准（cloud_id 空 = 本地词条，评分/收藏不上报）
    - compact + ensure_ascii=False：UTF-8 原文比 \\uXXXX 转义省 ~30% flash
    """
    words = []
    for i, r in enumerate(rows, 1):
        f = dict(zip(COLS, r.split(",")))
        obj = {"id": i, "text": f["text"],
               "difficulty": int(f["difficulty"])}
        for k in ("phonetic", "meaning", "example", "audio", "tag",
                  "root", "inflections", "source", "grade"):
            if f[k]:
                obj[k] = f[k]
        words.append(obj)
    data = {"version": len(words), "words": words}
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(data, fp, ensure_ascii=False, separators=(",", ":"))


def validate(rows, name):
    """校验：11 列、无英文逗号撕裂、字节上限、总规模。"""
    errs = 0
    for i, r in enumerate(rows, 2):  # +表头偏移，报行号
        f = r.split(",")
        if len(f) != 11:
            print("[ERR] %s 第%d行列数=%d" % (name, i, len(f)))
            errs += 1
        else:
            for key, val in zip(("text", "phonetic", "meaning", "example",
                                 "audio", "tag", "root", "inflections",
                                 "source", "grade"),
                                (f[0], f[1], f[2], f[3], f[4], f[5], f[7],
                                 f[8], f[9], f[10])):
                if len(val.encode("utf-8")) > MAX[key]:
                    print("[ERR] %s 第%d行 %s 超限" % (name, i, key))
                    errs += 1
    return errs


def main():
    # ---- 默认组合（均衡：中考核心 + 三学段语文）----
    default = []
    default += english_rows(
        load_english("ZhongKaoHeXin.json"),
        tag="中考核心", difficulty=2, source="中考考纲核心词汇", grade="初中")
    default += poetry_rows(
        load_primary_js(),
        tag="小学古诗", difficulty=1, source="小学必背古诗词", grade="小学")
    default += poetry_rows(
        load_poetry_json("poetry_chuzhong.json"),
        tag="初中古诗文", difficulty=2, source="初中语文教材古诗文", grade="初中")
    default += poetry_rows(
        load_poetry_json("poetry_gaokao.json"),
        tag="高考古诗文", difficulty=3, source="高考语文必背60篇", grade="高中")

    if len(default) > DEVICE_TOTAL_MAX:
        print("[FATAL] 默认词库 %d 条 > 设备上限 %d"
              % (len(default), DEVICE_TOTAL_MAX))
        sys.exit(1)

    write_csv(os.path.join(OUT, "default_words.csv"), default)
    write_json(os.path.join(OUT, "default_words.json"), default)
    errs = validate(default, "default_words.csv")

    # ---- 分册词库（按需导入；导入会占用 4000 总额度）----
    sub_stats = {}
    for fname, tag, grade, source in PEP_META:
        rows = english_rows(load_english(fname), tag=tag, difficulty=(
            1 if "XiaoXue" in fname else 2 if "ChuZhong" in fname else 3),
            source=source, grade=grade)
        # 文件名 PEPXiaoXue3_1_T.json -> PEP_小学三上.csv 风格
        out_name = ("PEP_" + tag.replace("人教版", "")
                    .replace("年级", "").replace("高中", "高中-")
                    + ".csv").replace(" ", "_")
        write_csv(os.path.join(OUT, "subdicts", out_name), rows)
        errs += validate(rows, out_name)
        sub_stats[tag] = len(rows)

    # 高考 3500 整册（与分册同规格）
    rows = english_rows(load_english("GaoKao_3500.json"),
                        tag="高考3500", difficulty=3,
                        source="新课标高考词汇", grade="高中")
    write_csv(os.path.join(OUT, "subdicts", "高考3500.csv"), rows)
    errs += validate(rows, "高考3500.csv")
    sub_stats["高考3500"] = len(rows)

    # ---- 报告 ----
    print("默认词库 default_words.csv：%d 条（上限 %d）"
          % (len(default), DEVICE_TOTAL_MAX))
    for t in ("中考核心", "小学古诗", "初中古诗文", "高考古诗文"):
        print("  %-8s %d" % (t, sum(1 for r in default
                                    if r.split(",")[5] == t)))
    print("分册词库（out/subdicts/，按需导入）：")
    for t, n in sub_stats.items():
        print("  %-16s %d" % (t, n))
    print("全部分册合计 %d 条（同批导入将超设备上限，仅可挑选组合）"
          % sum(sub_stats.values()))
    if errs:
        print("[FATAL] 校验错误 %d 处" % errs)
        sys.exit(1)
    print("校验通过：列数/字节上限/无英文逗号撕裂")


if __name__ == "__main__":
    main()
