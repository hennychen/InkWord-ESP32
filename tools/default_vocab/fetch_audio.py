#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""默认词库读音文件整理（2026-08-27）。

为 out/default_words.csv 英文词条（中考核心 2140 条）批量整理真人读音：
  1. api  ：dictionaryapi.dev 免费词典 API 下载真人 MP3 → out/audio_raw/
  2. norm ：ffmpeg 统一转 32k mono MP3（与后端 TtsService 同规格）→ out/audio/
     （piper 兜底合成的文件直接落 out/audio/，不经过 raw）
  3. fill ：回填 audio 列到 out/default_words.csv + out/default_words.json，
     并同步后端 SeedData 与固件 src 副本（cp 方向同 README 步骤 3/3b）

约定：
  - 文件名 slug：小写，[a-z0-9_-]，空格→'_'，撇号/点删除（o'clock→oclock，
    p.m.→pm），固件 WORD_AUDIO_MAX=96 字节（-4B 余量）远够用
  - 固件 study_mode_machine：w->audio 人工命名字段优先于 {cloud_id}.mp3，
    故 CSV audio 列填 {slug}.mp3 后，SD 卡 /sdcard/audio/ 放同名文件即生效
  - 中文古诗文 267 条本次不处理（后端 Piper 仅 en_US voice，audio 列留空）
  - 幂等：已存在的文件跳过；fill 前自动备份 .bakN（递增）

用法：
  python3 fetch_audio.py api          # 全量下载（0.3s/req，约 12 分钟）
  python3 fetch_audio.py api 50      # 先试 50 条
  python3 fetch_audio.py norm        # raw → 32k mono 转码
  python3 fetch_audio.py stats       # 覆盖统计
  python3 fetch_audio.py fill        # 回填 + 同步双端副本
"""

import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "out")
CSV_PATH = os.path.join(OUT_DIR, "default_words.csv")
JSON_PATH = os.path.join(OUT_DIR, "default_words.json")
RAW_DIR = os.path.join(OUT_DIR, "audio_raw")     # API 原始下载（保留档）
AUD_DIR = os.path.join(OUT_DIR, "audio")         # 最终产物（32k mono）
REPORT_PATH = os.path.join(OUT_DIR, "audio_api_report.json")  # API 结果清单

BACKEND_CSV = os.path.join(HERE, "..", "..", "InkWord_Backend", "src",
                           "InkWord.API", "SeedData", "default_words.csv")
FIRMWARE_JSON = os.path.join(HERE, "..", "..", "InkWord_Firmware", "src",
                             "default_words.json")

API_ENTRY = "https://api.dictionaryapi.dev/api/v2/entries/en/"
YOUDAO = "https://dict.youdao.com/dictvoice?type=1&audio="  # 美音（真人/高质量 TTS 混源）
REQ_GAP = 0.2          # 单 worker 请求间隔
WORKERS = 6            # 并发线程数（~3 req/s，礼貌限速）
TIMEOUT = 8
RETRIES = 1            # 网络类错误重试次数
# 域名分流：Google 系走本机 7897 代理（实测 0.2s vs 直连 1.4s 且 media
# 直连挂起）；有道国内站直连。代理不可用时自动回退全直连。
PROXY_HOSTS = ("dictionaryapi.dev", "gstatic.com")
PROXY_URL = "http://127.0.0.1:7897"

FFMPEG = shutil.which("ffmpeg") or "ffmpeg"


def is_cjk(s):
    return any("\u4e00" <= ch <= "\u9fff" for ch in s)


def load_csv():
    """返回 (rows, header)；rows 为 list[list[str]]。"""
    with open(CSV_PATH, encoding="utf-8") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = [r for r in reader if r]
    return header, rows


def slugify(text):
    """text → 文件名 slug（小写、[a-z0-9_-]）。"""
    s = text.strip().lower()
    s = s.replace(" ", "_").replace("'", "").replace(".", "")
    s = re.sub(r"[^a-z0-9_\-]", "", s)
    return s


def english_rows(rows):
    """英文词条（text 无 CJK）及其 (text, slug)。"""
    out = []
    for r in rows:
        if not is_cjk(r[0]):
            out.append((r[0], slugify(r[0])))
    return out


def check_slug_collision(items):
    """slug 重名检测：大小写同音词对（china/China、miss/Miss、a.m./am 等
    共 6 组）读音相同，共用同一音频文件，仅提示不退出。"""
    seen = {}
    for text, slug in items:
        seen.setdefault(slug, []).append(text)
    for slug, texts in seen.items():
        if len(texts) > 1:
            print("slug 共用：%s <- %s（同音词对共享音频）" % (slug, texts))


# ---------------------------------------------------------------- api --

def http_get(url):
    """GET 返回 (status, bytes)；网络异常返回 (None, err)。
    Google 系域名走代理 opener，其余直连；代理探测失败自动降级。"""
    last_err = None
    for _ in range(RETRIES + 1):
        try:
            if any(h in url for h in PROXY_HOSTS) and _PROXIED_OPENER:
                resp = _PROXIED_OPENER.open(url, timeout=TIMEOUT)
            else:
                req = urllib.request.Request(url, headers={"User-Agent": UA})
                resp = urllib.request.urlopen(req, timeout=TIMEOUT)
            try:
                return resp.status, resp.read()
            finally:
                resp.close()
        except urllib.error.HTTPError as e:
            # 404 等业务状态不重试
            return e.code, b""
        except Exception as e:  # noqa: BLE001 网络类异常统一重试
            last_err = e
            time.sleep(0.5)
    return None, last_err


UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36")
_PROXIED_OPENER = None   # 探测成功后置为走代理的 opener


def probe_proxy():
    """探测 7897 代理可用性（不可用则全直连）。"""
    global _PROXIED_OPENER
    try:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler(
            {"http": PROXY_URL, "https": PROXY_URL}))
        req = urllib.request.Request(API_ENTRY + "hello",
                                     headers={"User-Agent": UA})
        with opener.open(req, timeout=4) as r:
            if r.status == 200:
                _PROXIED_OPENER = opener
                print("代理 %s 可用（Google 系域名走代理）" % PROXY_URL)
                return
    except Exception as e:  # noqa: BLE001 探测失败回退直连
        print("代理 %s 不可用（%s），回退直连" % (PROXY_URL, e))


def find_audio_urls(entry_json):
    """API 响应里全部非空 audio URL，仅保留 gstatic 域名
    （api.dictionaryapi.dev/media 对本机网络挂起，实测 gstatic 秒回）。"""
    urls = []
    for entry in entry_json:
        for p in entry.get("phonetics", []):
            u = (p.get("audio") or "").strip()
            if u and "gstatic.com" in u:
                if u.startswith("//"):
                    u = "https:" + u
                urls.append(u)
    return urls


def fetch_one(text, slug, raw_path):
    """下载单词条音频：gstatic URL 优先 → 有道兜底（几乎全覆盖，含短语）。
    返回 source（'api'/'youdao'）或 None。"""
    word_q = urllib.parse.quote(text)
    status, body = http_get(API_ENTRY + word_q)
    api_urls = []
    if status == 200:
        try:
            api_urls = find_audio_urls(json.loads(body))
        except ValueError:
            api_urls = []
    youdao_url = YOUDAO + word_q
    src = None
    for u in api_urls + [youdao_url]:
        st, audio = http_get(u)
        if st == 200 and audio and audio[:2] != b"<!":
            tmp = raw_path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(audio)
            os.replace(tmp, raw_path)   # 半文件防护
            src = "youdao" if u == youdao_url else "api"
            break
    time.sleep(REQ_GAP)   # 单 worker 节流（成功/失败均间隔）
    return src


def cmd_api(limit):
    os.makedirs(RAW_DIR, exist_ok=True)
    probe_proxy()
    header, rows = load_csv()
    items = english_rows(rows)
    check_slug_collision(items)

    report = {}
    if os.path.exists(REPORT_PATH):
        with open(REPORT_PATH, encoding="utf-8") as f:
            report = json.load(f)

    todo = [(t, s) for t, s in items
            if not (os.path.exists(os.path.join(RAW_DIR, s + ".mp3"))
                    and os.path.getsize(os.path.join(RAW_DIR, s + ".mp3")) > 0)]
    skipped = len(items) - len(todo)
    if limit:
        todo = todo[:limit]
    print("待下载 %d 条（已存在跳过 %d），%d 线程并发"
          % (len(todo), skipped, WORKERS))

    ok = miss = 0
    with ThreadPoolExecutor(WORKERS) as ex:
        futs = {ex.submit(fetch_one, t, s,
                          os.path.join(RAW_DIR, s + ".mp3")): (t, s)
                for t, s in todo}
        for i, fut in enumerate(as_completed(futs), 1):
            text, slug = futs[fut]
            src = fut.result()
            if src:
                report[slug] = {"text": text, "source": src}
                ok += 1
            else:
                report[slug] = {"text": text, "source": "none",
                                "why": "all sources failed"}
                miss += 1
            if i % 100 == 0:
                print("[%d/%d] ok=%d miss=%d" % (i, len(todo), ok, miss),
                      flush=True)
                _save_report(report)

    _save_report(report)
    print("完成：%d ok / %d miss（详情见 audio_api_report.json）" % (ok, miss))


def _save_report(report):
    tmp = REPORT_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    os.replace(tmp, REPORT_PATH)


# -------------------------------------------------------------- norm --

def cmd_norm():
    """audio_raw/*.mp3 → audio/*.mp3（32k mono，与后端 ffmpeg 参数一致）。"""
    if not os.path.isdir(RAW_DIR):
        sys.exit("audio_raw/ 不存在，先跑 api")
    if not shutil.which(FFMPEG):
        sys.exit("ffmpeg 未安装（brew install ffmpeg）")
    os.makedirs(AUD_DIR, exist_ok=True)
    done = skip = fail = 0
    for name in sorted(os.listdir(RAW_DIR)):
        if not name.endswith(".mp3"):
            continue
        dst = os.path.join(AUD_DIR, name)
        if os.path.exists(dst) and os.path.getsize(dst) > 0:
            skip += 1
            continue
        src = os.path.join(RAW_DIR, name)
        tmp = dst + ".tmp"
        r = subprocess.run(
            [FFMPEG, "-y", "-loglevel", "error", "-i", src,
             "-codec:a", "libmp3lame", "-b:a", "32k", "-ac", "1",
             "-f", "mp3", tmp],
            stderr=subprocess.PIPE)
        if r.returncode == 0 and os.path.getsize(tmp) > 0:
            os.replace(tmp, dst)
            done += 1
        else:
            if os.path.exists(tmp):
                os.remove(tmp)
            print("转码失败：%s（%s）" % (name, r.stderr.decode()[:120].strip()))
            fail += 1
    print("norm 完成：%d 转码 / %d 跳过 / %d 失败" % (done, skip, fail))


# ------------------------------------------------------------- stats --

def cmd_stats():
    header, rows = load_csv()
    items = english_rows(rows)
    slugs = {s for _, s in items}
    have = {f[:-4] for f in os.listdir(AUD_DIR)
            if f.endswith(".mp3")} if os.path.isdir(AUD_DIR) else set()
    raw = {f[:-4] for f in os.listdir(RAW_DIR)
           if f.endswith(".mp3")} if os.path.isdir(RAW_DIR) else set()
    report = {}
    if os.path.exists(REPORT_PATH):
        with open(REPORT_PATH, encoding="utf-8") as f:
            report = json.load(f)
    api_none = [v["text"] for v in report.values() if v["source"] == "none"]
    print("英文词条 %d 条" % len(items))
    print("  audio_raw/（API 原始）：%d" % len(raw))
    print("  audio/（最终 32k）：%d / %d（%.1f%%）"
          % (len(have & slugs), len(slugs), 100.0 * len(have & slugs) / len(slugs)))
    print("  API 无真人音（待 piper 兜底）：%d" % len(api_none))
    for t in api_none[:30]:
        print("    -", t)
    if len(api_none) > 30:
        print("    ...（共 %d，全量见 audio_api_report.json source=none）" % len(api_none))


# -------------------------------------------------------------- fill --

def _backup(path):
    """递增备份 path → path.bakN（N 取现存最大 +1）。"""
    if not os.path.exists(path):
        return
    n = 1
    while os.path.exists("%s.bak%d" % (path, n)):
        n += 1
    shutil.copy2(path, "%s.bak%d" % (path, n))
    print("备份：%s.bak%d" % (os.path.basename(path), n))


def cmd_fill():
    header, rows = load_csv()
    items = english_rows(rows)
    check_slug_collision(items)
    slugs = dict(items)  # text -> slug
    aud_dir = AUD_DIR if os.path.isdir(AUD_DIR) else {}
    have = {f[:-4] for f in os.listdir(aud_dir) if f.endswith(".mp3")}

    # 1) CSV：audio 列 = {slug}.mp3（仅有文件的；中文行留空）
    _backup(CSV_PATH)
    filled = missing = 0
    with open(CSV_PATH, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(header)
        for r in rows:
            audio = ""
            if r[0] in slugs and slugs[r[0]] in have:
                audio = slugs[r[0]] + ".mp3"
                filled += 1
            elif r[0] in slugs:
                missing += 1
            r = r[:]
            r[4] = audio
            w.writerow(r)
    print("CSV 回填：%d 条有读音 / %d 条英文缺失（audio 留空）" % (filled, missing))

    # 2) JSON：words[] 英文条目补 "audio" 键（固件 word_parser 按 "audio" 键取）
    if os.path.exists(JSON_PATH):
        _backup(JSON_PATH)
        with open(JSON_PATH, encoding="utf-8") as f:
            data = json.load(f)
        for w_ in data.get("words", []):
            text = w_.get("text", "")
            if text in slugs and slugs[text] in have:
                w_["audio"] = slugs[text] + ".mp3"
            else:
                w_.pop("audio", None)
        tmp = JSON_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
        os.replace(tmp, JSON_PATH)
        print("JSON 回填：%d 条" % sum(1 for w_ in data["words"] if w_.get("audio")))

    # 3) 同步双端副本（方向同 README 步骤 3/3b）
    shutil.copy2(CSV_PATH, BACKEND_CSV)
    print("已同步 → %s" % os.path.normpath(BACKEND_CSV))
    if os.path.exists(os.path.dirname(FIRMWARE_JSON)):
        _backup(FIRMWARE_JSON)
        shutil.copy2(JSON_PATH, FIRMWARE_JSON)
        print("已同步 → %s" % os.path.normpath(FIRMWARE_JSON))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "api":
        limit = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        cmd_api(limit)
    elif cmd == "norm":
        cmd_norm()
    elif cmd == "stats":
        cmd_stats()
    elif cmd == "fill":
        cmd_fill()
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
