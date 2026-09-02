#!/usr/bin/env python3
"""gen_golden.py —— 黄金帧基准生成（T2.2 修 D2）

从 demo 自检固件的串口捕获文件解析 [GOLDEN] dump 段，生成
src/selftest_golden.h 基准表（重新编译后自检从 NO BASELINE 转
PASS/FAIL 判定模式）。

用法：
    pio device monitor | tee /tmp/golden_dump.txt   # 跑一次自检
    python3 tools/gen_golden.py /tmp/golden_dump.txt -o src/selftest_golden.h

解析契约（selftest_frame.c 输出，勿单方改格式）：
    [GOLDEN] BEGIN <page> <w> <h>
    [GOLDEN] B64 <64 字符/行>
    [GOLDEN] END <page>

校验：每页解码长度必须 == h * ((w+7)//8)（行主序 MSB-first 整帧）。
风险提示：帧含字库/词库/NVS 设置状态——布局无关的升级也须重 dump。
"""
import argparse
import base64
import datetime
import re
import sys

BEGIN_RE = re.compile(r"\[GOLDEN\] BEGIN (\S+) (\d+) (\d+)")
B64_RE = re.compile(r"\[GOLDEN\] B64 ([A-Za-z0-9+/=]+)")
END_RE = re.compile(r"\[GOLDEN\] END (\S+)")

IDENT_RE = re.compile(r"[^A-Za-z0-9_]")


def parse_dump(path):
    """解析 dump 文件 → [(page, w, h, bytes), ...]（按出现顺序）"""
    pages = {}
    order = []
    cur = None  # (page, w, h, chunks)
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = BEGIN_RE.search(line)
            if m:
                if cur:
                    sys.exit(f"错误: [{cur[0]}] 未 END 又遇 BEGIN")
                cur = (m.group(1), int(m.group(2)), int(m.group(3)), [])
                continue
            m = B64_RE.search(line)
            if m and cur:
                cur[3].append(m.group(1))
                continue
            m = END_RE.search(line)
            if m and cur:
                if m.group(1) != cur[0]:
                    sys.exit(f"错误: BEGIN {cur[0]} 与 END {m.group(1)} 不配对")
                page, w, h, chunks = cur
                raw = base64.b64decode("".join(chunks))
                expect = h * ((w + 7) // 8)
                if len(raw) != expect:
                    sys.exit(f"错误: [{page}] 解码 {len(raw)}B != 期望 "
                             f"{expect}B（{w}x{h}）—— dump 不完整？")
                if page in pages:
                    print(f"警告: [{page}] 重复段，以最后一次为准", file=sys.stderr)
                pages[page] = (w, h, raw)
                if page not in order:
                    order.append(page)
                cur = None
    if cur:
        sys.exit(f"错误: [{cur[0]}] 未闭合（缺 END）")
    if not order:
        sys.exit("错误: 未找到任何 [GOLDEN] 段——确认烧的是 demo env 且串口捕获完整")
    return [(p, *pages[p]) for p in order]


def emit_header(pages, src):
    out = []
    out.append("/**")
    out.append(" * @file selftest_golden.h")
    out.append(" * @brief 黄金帧基准表 —— tools/gen_golden.py 自动生成，勿手改")
    out.append(" *")
    out.append(f" * 生成：{datetime.date.today().isoformat()}，源 {src}")
    entries = ", ".join(f"{p} {w}x{h} {len(r)}B" for p, w, h, r in pages)
    out.append(f" * 页面：{entries}")
    out.append(" * 帧格式：行主序 MSB-first，每行 (w+7)/8 字节，bit=1=黑")
    out.append(" * 风险联动：字库/词库/NVS 设置变化会合法改变帧——相关升级后")
    out.append(" * 须重 dump 重生成（勿用本表判断此类变化为回归）")
    out.append(" */")
    out.append("#ifndef INKWORD_SELFTEST_GOLDEN_H")
    out.append("#define INKWORD_SELFTEST_GOLDEN_H")
    out.append("")
    out.append(f"#define GOLDEN_COUNT {len(pages)}")
    out.append("")
    table = []
    for page, w, h, raw in pages:
        ident = "k_gf_" + IDENT_RE.sub("_", page)
        out.append(f"/* {page}: {w}x{h} = {len(raw)}B */")
        out.append(f"static const unsigned char {ident}[] = {{")
        for i in range(0, len(raw), 16):
            row = ", ".join(f"0x{b:02X}" for b in raw[i:i + 16])
            out.append(f"    {row},")
        out.append("};")
        out.append("")
        table.append(f'    {{ "{page}", {ident}, sizeof({ident}) }},')
    out.append("static const struct {")
    out.append("    const char *page;")
    out.append("    const unsigned char *frame;")
    out.append("    unsigned int len;")
    out.append("} k_golden[] = {")
    out.extend(table)
    out.append("};")
    out.append("")
    out.append("#endif /* INKWORD_SELFTEST_GOLDEN_H */")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump", help="串口捕获文件（含 [GOLDEN] 段）")
    ap.add_argument("-o", "--output", default="-",
                    help="输出 .h 路径（默认 stdout）")
    args = ap.parse_args()

    pages = parse_dump(args.dump)
    text = emit_header(pages, args.dump)
    if args.output == "-":
        sys.stdout.write(text)
    else:
        with open(args.output, "w") as f:
            f.write(text)
        total = sum(len(r) for _, _, _, r in pages)
        print(f"已生成 {args.output}：{len(pages)} 页共 {total}B 基准。"
              f"目视确认后重新编译烧录 demo env 生效。", file=sys.stderr)


if __name__ == "__main__":
    main()
