#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""烧录前板卡家族门禁：防小屏（InkWord_Firmware / Arduino）与大屏
（InkWord_Firmware_BigScreen / ESP-IDF + epdiy）固件互烧。

背景（2026-09-19 实测）：小屏与大屏两套治具的 CP2102 桥都枚举成同一个
`/dev/cu.usbserial-0001` —— 串口节点名只反映「有桥在供电」，与板上烧了
什么固件无关，凭端口判断必然搞混；而互烧会连带重写分区表（小屏 8 分区
含 ota_0/ota_1 vs 大屏 4 分区含 storage），恢复要重烧正确家族一整轮。

判定链（板子身份优先于板上内容）：
  ① tools/boards.json 按 MAC 命中 → 家族取自登记的 families 列表；同一块
     N16R8 模组会在两套屏之间轮用，所以家族是「当前接哪套线束」的属性，
     不是模组的属性；只接一套屏的板子写单元素即可恢复硬拦；
  ② 未登记 → 回落读 0x8000 分区表签名（ota_0 = 小屏 / storage = 大屏），
     它只能说明「板上现在的固件是哪一族」，故 check 默认拒绝，需人工
     确认实体板后 register 落 MAC。

esptool 固定 --baud 115200：460800/921600 经 USB hub 会报
「Invalid head of packet」噪声（2026-09-19 实测）。

用法：
  tools/board_gate.py detect                        # 只看身份，不判定
  tools/board_gate.py check --env inkword-s3-demo    # 烧录前门禁
  tools/board_gate.py check --expect big
  tools/board_gate.py register --env bigscreen-app --label "大屏桌面板"
  tools/board_gate.py list
退出码：0 放行 / 2 家族不符 / 3 身份未登记 / 4 串口通信失败 / 5 用法错误
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "tools" / "boards.json"
REGISTRY_EXAMPLE = ROOT / "tools" / "boards.example.json"
PART_TABLE_ADDR = 0x8000
PART_TABLE_LEN = 0x1000
DEFAULT_PORT = os.environ.get("INKWORD_PORT", "/dev/cu.usbserial-0001")

PROJECT_INI = {
    "small": ROOT / "InkWord_Firmware" / "platformio.ini",
    "big": ROOT / "InkWord_Firmware_BigScreen" / "platformio.ini",
}
FAMILY_LABEL = {
    "small": '小屏 InkWord_Firmware（Arduino/GxEPD2，NS4150B 音频）',
    "big": '大屏 InkWord_Firmware_BigScreen（ESP-IDF/epdiy，ES108FC1 无音频）',
}
# 分区名 → 家族：两族都有 nvs/phy_init/factory，唯 OTA 槽与 storage 互斥
FAMILY_MARKER = {"ota_0": "small", "storage": "big"}

EXIT_OK, EXIT_MISMATCH, EXIT_UNREGISTERED, EXIT_COMMS, EXIT_USAGE = 0, 2, 3, 4, 5


class GateError(Exception):
    pass


def esptool_cmd() -> list[str]:
    override = os.environ.get("INKWORD_ESPTOOL")
    cands = [shlex.split(override)] if override else []
    cands += [[p] for p in (shutil.which("esptool.py"), shutil.which("esptool")) if p]
    for c in cands:
        try:
            subprocess.run(c + ["version"], capture_output=True, check=True)
            return c
        except Exception:
            continue
    raise GateError("找不到可用的 esptool（可设 INKWORD_ESPTOOL=/path/to/esptool.py）")


def parse_table(blob: bytes) -> list[dict]:
    rows = []
    for off in range(0, len(blob) - 31, 32):
        row = blob[off:off + 32]
        if row[0] != 0xAA:
            break                       # 首个非 0xAA 行即表尾
        # IDF 4.4（小屏 Arduino/core 2.0）单字节 magic：type@1 offset@3
        # size@7 label@11；IDF 5.x（大屏）magic 扩成 0xAA50 双字节，整行
        # 字段后移 1 字节——两族表要同一份解析读，故按行探测。
        shift = 1 if row[1] == 0x50 else 0
        rows.append({
            "name": row[11 + shift:27 + shift].split(b"\x00")[0].decode("ascii", "replace"),
            "offset": int.from_bytes(row[3 + shift:7 + shift], "little"),
            "size": int.from_bytes(row[7 + shift:11 + shift], "little"),
        })
    return rows


def probe(port: str) -> dict:
    """esptool 逐命令读 MAC 与分区表（本机 esptool.py 4.7 不支持命令链，
    每命令各复位一次）。"""
    base = esptool_cmd() + ["--chip", "esp32s3", "--port", port, "--baud", "115200"]

    def run(label: str, args: list[str]) -> str:
        p = subprocess.run(base + args, capture_output=True, text=True)
        if p.returncode != 0:
            tail = " / ".join((p.stdout + p.stderr).strip().splitlines()[-2:])
            raise GateError(
                f"esptool {label} 失败（{port}）：{tail}\n"
                "  → 端口不存在：板子没接到主机（CP2102 只要供电就会枚举）\n"
                "  → Invalid head of packet：换 Mac 直插口，别走 hub\n"
                "  → Failed to connect：板子在掉电复位循环（大屏经 hub 供电不足）")
        return p.stdout

    mac = None
    m = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", run("read_mac", ["read_mac"]))
    if m:
        mac = m.group(1).lower()

    names: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        bin_file = Path(td) / "partitions.bin"
        # 板子供电不稳（大屏经 hub 反复 POWERON）时 read_flash 会整块返回
        # 0x00 且退出码仍为 0，故空表重试一次再判「读不到」
        for _ in (1, 2):
            run("read_flash 分区表",
                ["read_flash", hex(PART_TABLE_ADDR), hex(PART_TABLE_LEN), str(bin_file)])
            names = [r["name"] for r in parse_table(bin_file.read_bytes())]
            bin_file.unlink()
            if names:
                break
    on_flash = next((FAMILY_MARKER[n] for n in names if n in FAMILY_MARKER), None)
    return {"port": port, "mac": mac, "partitions": names,
            "family_on_flash": on_flash}


def load_registry() -> dict:
    if not REGISTRY.exists():
        return {"boards": {}}
    return json.loads(REGISTRY.read_text(encoding="utf-8"))


def save_registry(data: dict) -> None:
    REGISTRY.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")


def family_from_env(env: str) -> str:
    hits = [fam for fam, ini in PROJECT_INI.items()
            if ini.exists() and re.search(rf"^\[env:{re.escape(env)}\]",
                                          ini.read_text(encoding="utf-8"), re.M)]
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        raise GateError(f"env {env} 在两个工程的 platformio.ini 里同名，请用 --expect")
    raise GateError(f"两个工程的 platformio.ini 里都没有 [env:{env}]")


def resolve(info: dict, allow_unregistered: bool) -> dict:
    entry = load_registry()["boards"].get(info["mac"] or "")
    if entry:
        # families：同一块 devkit 在两套屏间复用时可有多个家族（2026-09-19
        # 现场确认：N16R8 换屏线束不换模组，单 family 字段是错的模型）
        fams = entry.get("families") or [entry.get("family")]
        return {"families": fams, "family": fams[0], "source": "boards.json 登记",
                "label": entry.get("label", ""), "registered": True, "warn": None}
    if info["family_on_flash"]:
        fam = info["family_on_flash"]
        msg = ("分区签名只能证明「板上现在的固件」属于 "
               f"{fam}，不能证明这块板就是 {fam}")
        return {"families": [fam], "family": fam, "source": "分区表签名推断",
                "label": "", "registered": allow_unregistered, "warn": msg}
    return {"families": [], "family": None, "source": "无", "label": "",
            "registered": False,
            "warn": "分区表为空或不含家族标记，无法判定"}


def refuse(msg: str) -> None:
    """先冲 stdout，避免管道下 stderr 的拒绝原因跑到证据行前面。"""
    sys.stdout.flush()
    print(msg, file=sys.stderr)


def show(info: dict, verdict: dict | None, as_json: bool) -> None:
    if as_json:
        print(json.dumps({"board": info, "verdict": verdict}, ensure_ascii=False, indent=2))
        return
    print(f"端口        {info['port']}")
    print(f"MAC         {info['mac'] or '（未读到）'}")
    print(f"分区        {' '.join(info['partitions']) or '（空表/无家族标记）'}")
    print(f"板上固件    {info['family_on_flash'] or '未知'}")
    if verdict:
        fams = "/".join(verdict["families"]) or "未知"
        print(f"登记家族    {fams}（{verdict['source']}）")
        if verdict.get("label"):
            print(f"登记名称    {verdict['label']}")
        if verdict.get("warn"):
            print(f"注意        {verdict['warn']}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--json", action="store_true", dest="as_json")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("detect", help="只读身份，不做判定")
    sub.add_parser("list", help="列出版卡登记")

    p_check = sub.add_parser("check", help="烧录前门禁")
    p_check.add_argument("--env", help="目标 PlatformIO env，家族取自其所属工程")
    p_check.add_argument("--expect", choices=sorted(FAMILY_LABEL))
    p_check.add_argument("--allow-unregistered", action="store_true",
                         help="未登记板子按分区签名放行（首次试用，事后须 register）")

    p_reg = sub.add_parser("register", help="按 MAC 登记当前板子")
    p_reg.add_argument("--env")
    p_reg.add_argument("--family", choices=sorted(FAMILY_LABEL))
    p_reg.add_argument("--label", default="")
    p_reg.add_argument("--panel", default="", help="小屏填面板 registry id 或面板 env 名")
    p_reg.add_argument("--power", default="", help="供电约束（大屏：5V/2A 直插，不走 hub）")

    args = ap.parse_args()

    if args.cmd == "list":
        boards = load_registry()["boards"]
        if not boards:
            print(f"（未登记任何板子；参考 {REGISTRY_EXAMPLE.name} 建 {REGISTRY.name}）")
        for mac, e in boards.items():
            fams = "/".join(e.get("families") or [e.get("family")])
            print(f"{mac}  {fams:<10} {e.get('label', '')}")
        return EXIT_OK

    try:
        info = probe(args.port)
    except GateError as exc:
        print(f"门禁通信失败：{exc}", file=sys.stderr)
        return EXIT_COMMS

    if args.cmd == "detect":
        show(info, resolve(info, False), args.as_json)
        return EXIT_OK

    if args.cmd == "register":
        fam = args.family or (family_from_env(args.env) if args.env else None)
        if not fam:
            print("register 需 --family 或 --env 指明家族", file=sys.stderr)
            return EXIT_USAGE
        if not info["mac"]:
            print("未读到 MAC，不能登记", file=sys.stderr)
            return EXIT_COMMS
        data = load_registry()
        boards = data.setdefault("boards", {})
        rec = boards.setdefault(info["mac"], {"added": date.today().isoformat()})
        fams = rec.get("families") or ([rec["family"]] if rec.get("family") else [])
        if fam not in fams:                      # 复用的 devkit 可属多个家族
            fams.append(fam)
        rec["families"] = fams
        rec.pop("family", None)
        rec["label"] = args.label or rec.get("label", "")
        rec["panel"] = args.panel or rec.get("panel", "")
        rec["power"] = args.power or rec.get("power", "")
        rec["family_on_flash_at_register"] = info["family_on_flash"]
        save_registry(data)
        print(f"已登记 {info['mac']} → {'/'.join(fams)} 于 "
              f"{REGISTRY.relative_to(ROOT)}")
        return EXIT_OK

    # check
    expected = args.expect or (family_from_env(args.env) if args.env else None)
    if not expected:
        print("check 需 --env 或 --expect 指明目标家族", file=sys.stderr)
        return EXIT_USAGE
    verdict = resolve(info, args.allow_unregistered)
    show(info, verdict, args.as_json)
    print(f"目标        {expected} —— {FAMILY_LABEL[expected]}")

    if verdict["family"] is None:
        refuse(f"\n拒绝烧录：{verdict['warn']}")
        return EXIT_UNREGISTERED
    if expected not in verdict["families"]:
        refuse(f"\n拒绝烧录：目标是 {expected}，但端口上的板子（MAC "
               f"{info['mac']}）登记为 {'/'.join(verdict['families']) or '未知'}。"
               f"\n  误烧会重写分区表，请先换板或改目标。")
        return EXIT_MISMATCH
    if info["family_on_flash"] and info["family_on_flash"] != expected:
        print(f"提示        板上当前是 {info['family_on_flash']} 固件，本次烧录会把"
              f"分区表重写为 {expected} 家族布局（另一套屏的固件即被覆盖）。")
    if not verdict["registered"]:
        refuse(f'\n拒绝烧录：该板 MAC 未登记。确认实体板后执行：\n'
               f'  tools/board_gate.py register --port {args.port} --family {expected} '
               f'--label "…"\n（临时放行可加 --allow-unregistered）')
        return EXIT_UNREGISTERED
    print("\n放行：板子家族与目标一致")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
