#!/usr/bin/env python3
"""人教版单元词表归类：中考考纲词 → 教材册/单元归属（2026-08-28）。

用途：default_words.csv 的 2140 个英语词从「中考考纲核心词汇」平铺桶
细分为 grade=册（七年级上…九年级）+ source=单元（含话题名），供设备
教材目录（catalog_index/browse_mode）三级浏览。

策略（v3 通道合一，对 LLM 前倾偏差鲁棒）：
  1. 单元清单硬编码（人教 Go for it! 2012 经典版 5 册 58 单元，话题名
     人工核准）；LLM 生成「该单元新学生词表」（严格 prompt），双模型
     交集（QWEN ∩ DEEPSEEK）收敛单模型虚胖，交集缓存 /tmp 复用；
  2. 反向匹配考纲词：词 → (册序, 单元序)，跨册重复取首现；
  3. 虚胖修剪：交集首现后 >100 词的单元（v2 rescue 前倾误吸高发区，
     如七上 Unit 1=173 词）整体移入重标注池；
  4. 统一标注（重标注池 + 未匹配词）：prompt 附全部 58 单元话题清单，
     输出结构化 JSON（册 + 单元号，含「小学已学」出口均摊七上 Starter
     复习单元），4 线程并行批次；册/单元在 BOOKS 内校验，越界丢弃；
  5. 仍未匹配 → 「考纲拓展」桶，报告供人工抽查。

密钥经环境变量传入（QW_KEY / DS_KEY），不写入任何文件。
输出：/tmp/units_report.md（报告）+ /tmp/default_words_units.csv（新词表）。
"""
import csv
import json
import os
import re
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

CSV_IN = os.path.join(os.path.dirname(__file__),
                      "../../InkWord_Backend/src/InkWord.API/SeedData/default_words.csv")
CSV_OUT = "/tmp/default_words_units.csv"
REPORT = "/tmp/units_report.md"
CACHE = "/tmp/units_inter.json"   # 交集词表缓存（重跑标注不必重生成）
FAT_LIMIT = 100                   # 单元词数超此值 → 整单元移入重标注池

# ---- 人教 Go for it! 2012 经典版单元清单（话题名人工核准） ----
BOOKS = [
    ("七年级上", [("Starter Unit 1", "Good morning!"),
                  ("Starter Unit 2", "What's this in English?"),
                  ("Starter Unit 3", "What color is it?"),
                  ("Unit 1", "My name's Gina."),
                  ("Unit 2", "Is this your pencil?"),
                  ("Unit 3", "This is my sister."),
                  ("Unit 4", "Where's my schoolbag?"),
                  ("Unit 5", "Do you have a soccer ball?"),
                  ("Unit 6", "Do you like bananas?"),
                  ("Unit 7", "How much are these socks?"),
                  ("Unit 8", "When is your birthday?"),
                  ("Unit 9", "My favorite subject is science.")]),
    ("七年级下", [("Unit 1", "Can you play the guitar?"),
                  ("Unit 2", "What time do you go to school?"),
                  ("Unit 3", "How do you get to school?"),
                  ("Unit 4", "Don't eat in class."),
                  ("Unit 5", "Why do you like pandas?"),
                  ("Unit 6", "I'm watching TV."),
                  ("Unit 7", "It's raining!"),
                  ("Unit 8", "Is there a post office near here?"),
                  ("Unit 9", "What does he look like?"),
                  ("Unit 10", "I'd like some noodles."),
                  ("Unit 11", "How was your school trip?"),
                  ("Unit 12", "What did you do last weekend?")]),
    ("八年级上", [("Unit 1", "Where did you go on vacation?"),
                  ("Unit 2", "How often do you exercise?"),
                  ("Unit 3", "I'm more outgoing than my sister."),
                  ("Unit 4", "What's the best movie theater?"),
                  ("Unit 5", "Do you want to watch a game show?"),
                  ("Unit 6", "I'm going to study computer science."),
                  ("Unit 7", "Will people have robots?"),
                  ("Unit 8", "How do you make a banana milk shake?"),
                  ("Unit 9", "Can you come to my party?"),
                  ("Unit 10", "If you go to the party you'll have a great time!")]),
    ("八年级下", [("Unit 1", "What's the matter?"),
                  ("Unit 2", "I'll help to clean up the city parks."),
                  ("Unit 3", "Could you please clean your room?"),
                  ("Unit 4", "Why don't you talk to your parents?"),
                  ("Unit 5", "What were you doing when the rainstorm came?"),
                  ("Unit 6", "An old man tried to move the mountains."),
                  ("Unit 7", "What's the highest mountain in the world?"),
                  ("Unit 8", "Have you read Treasure Island yet?"),
                  ("Unit 9", "Have you ever been to a museum?"),
                  ("Unit 10", "I've had this bike for three years.")]),
    ("九年级",   [("Unit 1", "How can we become good learners?"),
                  ("Unit 2", "I think that mooncakes are delicious!"),
                  ("Unit 3", "Could you please tell me where the restrooms are?"),
                  ("Unit 4", "I used to be afraid of the dark."),
                  ("Unit 5", "What are the shirts made of?"),
                  ("Unit 6", "When was it invented?"),
                  ("Unit 7", "Teenagers should be allowed to choose clothes."),
                  ("Unit 8", "It must belong to Carla."),
                  ("Unit 9", "I like music that I can dance to."),
                  ("Unit 10", "You're supposed to shake hands."),
                  ("Unit 11", "Sad movies make me cry."),
                  ("Unit 12", "Life is full of the unexpected."),
                  ("Unit 13", "We're trying to save the earth!"),
                  ("Unit 14", "I remember meeting all of you in Grade 7.")]),
]

PROVIDERS = {
    "qwen": ("https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
             os.environ.get("QW_KEY", ""), "qwen-flash"),
    "deepseek": ("https://api.deepseek.com/chat/completions",
                 os.environ.get("DS_KEY", ""), "deepseek-chat"),
}

WORD_RE = re.compile(r"[a-zA-Z][a-zA-Z'\- ]*")


def chat(provider, prompt, timeout=90, max_tokens=1800):
    url, key, model = PROVIDERS[provider]
    if not key:
        raise RuntimeError(f"{provider} key missing")
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.1, "max_tokens": max_tokens,
    }).encode()
    req = urllib.request.Request(url, data=body, headers={
        "Authorization": f"Bearer {key}",
        "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = json.load(r)
    return data["choices"][0]["message"]["content"]


def gen_unit_words(provider, book, unit_no, topic):
    """单单元新学生词表生成（严格：只列本单元词头新词，30-60 个）"""
    prompt = (
        f"人教版初中英语《Go for it!》{book} {unit_no} \"{topic}\" 的学生词表。"
        "严格只列本单元课本词表中作为【新学生词】出现的单词（词头），"
        "30-60 个。绝对不要列入：之前单元已学过的词、常见通用词"
        "（a/the/is/what 等小学已会词）、派生变形、短语。"
        "输出格式：小写英文单词，逗号分隔，一行以内，"
        "不要释义、音标、编号或任何中文。")
    for attempt in range(3):
        try:
            text = chat(provider, prompt)
            words = [w.strip().lower() for w in re.split(r"[,;、\n]+", text)]
            words = [w for w in words if w and "'" not in w[:1]]
            # 过滤明显非单词（介词短语片段等保留——匹配层自会过滤）
            words = [w for w in words if re.fullmatch(r"[a-z][a-z'\- ]*", w)]
            if len(words) >= 10:
                return set(words)
        except Exception as e:
            print(f"  [{provider} {book}{unit_no}] attempt{attempt}: {e}", file=sys.stderr)
            time.sleep(3)
    return set()


def valid_map():
    """(册名, 单元号, 是否Starter) → "Unit N 话题" 合法单元名映射"""
    valid = {}
    for book, units in BOOKS:
        for uno, topic in units:
            starter = uno.startswith("Starter")
            num = uno.replace("Starter Unit", "").replace("Unit", "").strip()
            valid[(book, int(num), starter)] = f"{uno} {topic}"
    return valid


def gen_intersections():
    """58 单元 × 2 模型 × 2 采样：单模型自并集再跨模型交集（救活
    单次采样波动导致的空单元），结果缓存 /tmp/units_inter.json 复用"""
    if os.path.exists(CACHE):
        with open(CACHE, encoding="utf-8") as f:
            raw = json.load(f)
        return {tuple(map(int, k.split(","))): set(v) for k, v in raw.items()}

    jobs = []
    for bi, (book, units) in enumerate(BOOKS):
        for ui, (uno, topic) in enumerate(units):
            for pv in PROVIDERS:
                for _ in range(2):
                    jobs.append((pv, bi, uno, ui, topic))
    print(f"生成任务 {len(jobs)} 个（58 单元 × 2 模型 × 2 采样）…")

    def run(job):
        pv, bi, uno, ui, topic = job
        return (bi, ui, pv), gen_unit_words(pv, BOOKS[bi][0], uno, topic)

    by_cell = {}
    with ThreadPoolExecutor(max_workers=6) as ex:
        for (bi, ui, pv), ws in ex.map(run, jobs):
            if ws:
                by_cell.setdefault((bi, ui), {}).setdefault(pv, []).append(ws)

    results = {}
    for key, pvmap in by_cell.items():
        unions = [set().union(*runs) for runs in pvmap.values()]
        inter = set.intersection(*unions) if len(unions) == 2 else set()
        if len(inter) >= 5:
            results[key] = inter
    with open(CACHE, "w", encoding="utf-8") as f:
        json.dump({f"{k[0]},{k[1]}": sorted(v) for k, v in results.items()}, f)
    return results


def label_words(pool):
    """统一标注通道：附 58 单元话题清单，词 → 册/单元（或小学已学/
    不在教材）。30 词/批 × 4 线程并行；结果在 BOOKS 内校验。
    返回 {词: ("primary", None) | (册名, "Unit N 话题")}"""
    catalog_txt = "\n".join(
        f"{book}：" + "；".join(f"{uno} {topic}" for uno, topic in units)
        for book, units in BOOKS)
    valid = valid_map()
    out = {}

    def run_batch(i):
        batch = pool[i:i + 30]
        prompt = (
            "人教版初中英语《Go for it!》(2012版)五册单元话题清单：\n"
            + catalog_txt + "\n\n"
            "任务：下列中考考纲单词，逐一标注其首次作为【新学生词】在人教"
            "教材中系统学习的册与单元号。严格依据清单中的单元话题判断，"
            "宁可标晚（后续单元复现扩展）也不要凭感觉标早。\n规则：\n"
            '- 中国小学英语已学过的常见词（apple/pen/dog/one 等）输出 '
            '{"w":"词","p":1}\n'
            '- 初中新学词输出 {"w":"词","b":"册名","n":单元号,"s":0}（Starter '
            'Unit 则 s=1）\n'
            '- 不在人教教材出现的输出 {"w":"词","x":1}\n'
            "只输出 JSON 数组，不加任何其他文字。\n"
            + json.dumps(batch, ensure_ascii=False))
        for attempt in range(2):
            try:
                text = chat("qwen", prompt, timeout=150, max_tokens=3000)
                m = re.search(r"\[.*\]", text, re.S)
                if not m:
                    continue
                return json.loads(m.group(0))
            except Exception as e:
                print(f"  label batch{i} attempt{attempt}: {e}", file=sys.stderr)
                time.sleep(3)
        return []

    with ThreadPoolExecutor(max_workers=4) as ex:
        for arr in ex.map(run_batch, range(0, len(pool), 30)):
            for it in arr:
                if not isinstance(it, dict):
                    continue
                w = str(it.get("w", "")).strip().lower()
                if not w or w in out or it.get("x"):
                    continue
                if it.get("p"):
                    out[w] = ("primary", None)
                    continue
                b, n, s = str(it.get("b", "")).strip(), it.get("n"), it.get("s")
                if not isinstance(n, int) or n < 1:
                    continue
                key = (b, n, bool(s))
                if key in valid:
                    out[w] = (b, valid[key])
    return out


def main():
    # 1. 双模型交集词表（缓存复用）
    results = gen_intersections()
    total_gen = sum(len(v) for v in results.values())
    print(f"单元词表就绪：{len(results)}/58 单元（交集），累计 {total_gen} 词次")

    # 2. 交集首现映射（词 → 最早册/单元，学习顺序）
    word_pos = {}
    for (bi, ui), ws in results.items():
        book = BOOKS[bi][0]
        uno, topic = BOOKS[bi][1][ui]
        for w in ws:
            if w not in word_pos:
                word_pos[w] = (book, f"{uno} {topic}")

    # 3. 读考纲 CSV；初分配 + 虚胖检测（>FAT_LIMIT 词单元整体重标注）
    with open(CSV_IN, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    hdr, data = rows[0], rows[1:]
    si, gi, ti = hdr.index("source"), hdr.index("grade"), hdr.index("text")

    unit_load = {}
    for r in data:
        if "考纲" in r[si]:
            w = r[ti].strip().lower()
            if w in word_pos:
                unit_load[word_pos[w]] = unit_load.get(word_pos[w], 0) + 1
    fat_units = {u for u, n in unit_load.items() if n > FAT_LIMIT}

    pool = []
    for r in data:
        if "考纲" not in r[si]:
            continue
        w = r[ti].strip().lower()
        if w not in word_pos or word_pos[w] in fat_units:
            if w not in pool:
                pool.append(w)
    print(f"虚胖单元 {len(fat_units)} 个（>{FAT_LIMIT} 词）已隔离；"
          f"重标注池 {len(pool)} 词（虚胖修剪 + 未匹配）")

    # 4. 统一标注（附话题清单，4 线程）
    labeled = label_words(pool) if pool else {}
    prim_pool = sum(1 for v in labeled.values() if v[0] == "primary")
    print(f"统一标注完成：{len(labeled)}/{len(pool)}"
          f"（小学已学 {prim_pool} 归 Starter 复习）")

    # 5. 回写 CSV：交集命中（非虚胖）> 标注 > 考纲拓展
    starters = [f"{uno} {topic}" for uno, topic in BOOKS[0][1][:3]]
    hit, miss = 0, []
    inter_n, redo_n, prim_n = 0, 0, 0
    per_book = {}
    prim_i = 0
    for r in data:
        if "考纲" not in r[si]:          # 语文/古诗行保持原样
            continue
        w = r[ti].strip().lower()
        if w in word_pos and word_pos[w] not in fat_units:
            book, unit = word_pos[w]
            inter_n += 1
        elif w in labeled:
            b, unit = labeled[w]
            if b == "primary":           # 小学已学 → Starter 复习均摊
                book, unit = "七年级上", starters[prim_i % 3]
                prim_i += 1
                prim_n += 1
            else:
                book, unit = b, unit
            redo_n += 1
        else:
            miss.append(r[ti])
            r[gi], r[si] = "考纲拓展", "考纲拓展词汇"
            continue
        r[gi], r[si] = book, unit
        per_book[book] = per_book.get(book, 0) + 1
        hit += 1

    with open(CSV_OUT, "w", newline="", encoding="utf-8") as f:
        wtr = csv.writer(f)
        wtr.writerow(hdr)
        wtr.writerows(data)

    # 6. 报告
    with open(REPORT, "w", encoding="utf-8") as f:
        f.write("# 人教单元归类报告（v3 交集+修剪+统一标注）\n\n")
        f.write(f"- 考纲英语词：{hit + len(miss)}\n- 命中教材单元：{hit}"
                f"（{100.0 * hit / (hit + len(miss)):.1f}%）\n"
                f"  - 双模型交集首现：{inter_n}\n"
                f"  - 统一标注（虚胖修剪 + 未匹配捞回）：{redo_n}"
                f"，其中小学已学归 Starter 复习：{prim_n}\n"
                f"- 考纲拓展桶：{len(miss)}\n\n## 各册分布\n")
        for book, _ in BOOKS:
            f.write(f"- {book}: {per_book.get(book, 0)}\n")
        f.write("\n## 未匹配词（进考纲拓展桶，人工抽查）\n")
        f.write("、".join(miss) + "\n")
    print(f"完成：命中 {hit}（交集 {inter_n} + 标注 {redo_n}），"
          f"拓展 {len(miss)}；报告 {REPORT}；新表 {CSV_OUT}")


if __name__ == "__main__":
    main()
