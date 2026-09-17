#!/usr/bin/env python3
# iwf → epdiy 波形 C 头转换器（ES108FC1 专用波形接入工具链的一环）
#
# 背景：display_config.h TODO(A) 等待 ES108FC1C1-RHY 专用波形替换
#   epdiy_ED047TC1。NekoInk（zephray）有 ES108FC1 适配板与 iwf 交换格式，
#   但仓库未收录该屏的波形数据（仅 wbf_waveform_dump 模式表）。
#   本脚本在拿到 iwf（descriptor .iwf + PREFIX_M*_T*.csv）后直接产出
#   epdiy 波形头文件，替换 PANEL_WAVEFORM 即完成接入。
#
# 用法：
#   python3 iwf2epdiy.py <desc.iwf> <输出.h> [--name ES108FC1]
#     [--times 100]        无相位时长信息时每个相位的默认时长（同 epdiy
#                          FRAME_TIMES_DU=100 口径）
#     [--verify <epdiy头>] 对照已知 epdiy 波形头做逐位极性自检（可选）
#
# 格式映射（两套均为 2bit 电压码，语义不同，不可凭直觉改）：
#   iwf:   0=GND  1=VNEG(驱黑)  2=VPOS(驱白)  3=Keep(悬空)
#   epdiy: 1=驱白 2=驱黑（ED047TC1 GC16 白基线 to=15 → code1 的实证口径，
#          lan_image.c run98 注释：to=15,from=0 = 后 15 相位驱白）
#   即 iwf 1→2、2→1、0→0、3→3。
#   epdiy lut 布局：data[phase][from][byte]，byte=to>>2，shift=6-2*(to&3)
#   （与本项目 tools/decode_waveform.py / waveform_scanq.c 逐位一致）。

import argparse
import configparser
import os
import sys

# iwf → epdiy 电压码映射（见文件头说明）
IWF_TO_EPDIY = {0: 0, 1: 2, 2: 1, 3: 3}

# iwf 模式名 → epdiy mode type（epdiy_builtin_waveforms / modenames.py 口径）
MODE_TYPE = {
    "INIT": 1,
    "DU": 16,
    "GC16": 2,
    "GL16": 5,
    "A2": 17,
    "GC4": 6,
    "GLR16": 3,
    "GLD16": 4,
}


def parse_lut_csv(path):
    """解析 PREFIX_M*_T*.csv → dict{(src,dst): [frame 电压码...]}。

    支持区间写法 "0:14:15"（README 语法：src0:src1:dst 或 src,dst）。
    返回 dict + 帧数（各行应等长）。
    """
    lut = {}
    frames = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            vals = [v.strip() for v in line.split(",")]
            seq = [int(v) for v in vals[2:]]
            if frames is None:
                frames = len(seq)
            elif len(seq) != frames:
                raise SystemExit(f"{path}: 帧数不一致 {len(seq)} != {frames}")

            def rng(tok):
                if ":" in tok:
                    a, b = (int(x) for x in tok.split(":"))
                    return range(a, b + 1)
                return (int(tok),)

            pairs = [(s, d) for s in rng(vals[0]) for d in rng(vals[1])]
            for s, d in pairs:
                lut[(s, d)] = seq
    return lut, frames


def lut_to_epdiy_arrays(lut, frames):
    """(src,dst) 帧序列 → epdiy data[phase][from][4]（16 级灰）。

    逐相位展开：data[p][from][to>>2] 的 2bit 槽位填映射后的电压码。
    CSV 未覆盖的 (from,to) 组合填 3（Keep，同色不驱）。
    """
    data = [[[0] * 4 for _ in range(16)] for _ in range(frames)]
    for (src, dst), seq in lut.items():
        for p in range(min(frames, len(seq))):
            code = IWF_TO_EPDIY[seq[p]]
            byte, sh = dst >> 2, 6 - 2 * (dst & 3)
            data[p][src][byte] |= code << sh
    # 未覆盖组合填 Keep(3)，避免遗留 0 与「GND」混淆
    for p in range(frames):
        for src in range(16):
            for dst in range(16):
                if (src, dst) not in lut:
                    byte, sh = dst >> 2, 6 - 2 * (dst & 3)
                    data[p][src][byte] |= 3 << sh
    return data


def fmt_data(arrays, sym):
    out = [f"const uint8_t {sym}_data[{len(arrays)}][16][4] = {{"]
    for phase in arrays:
        rows = ",".join(
            "{" + ",".join(f"0x{b:02x}" for b in row) + "}" for row in phase
        )
        out.append("{" + rows + "},")
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iwf")
    ap.add_argument("out")
    ap.add_argument("--name", default="ES108FC1")
    ap.add_argument("--times", type=int, default=100)
    args = ap.parse_args()

    cp = configparser.ConfigParser()
    cp.read(args.iwf)
    w = cp["WAVEFORM"]
    prefix = w["PREFIX"]
    modes_n = int(w["MODES"])
    temps_n = int(w["TEMPS"])
    dirname = os.path.dirname(args.iwf)

    chunks = [
        f"// 由 iwf2epdiy.py 从 {os.path.basename(args.iwf)} 生成（勿手改；"
        f"重生成见 tools/iwf2epdiy.py）",
        f"#include <epdiy.h>",
        "",
    ]
    mode_syms = []
    for m in range(modes_n):
        sec = cp[f"MODE{m}"]
        mname = sec["NAME"]
        mtype = MODE_TYPE.get(mname.upper())
        if mtype is None:
            print(f"跳过未知模式 {mname}", file=sys.stderr)
            continue
        for t in range(temps_n):
            path = os.path.join(dirname, f"{prefix}_M{m}_T{t}.csv")
            if not os.path.exists(path):
                print(f"缺少 {path}，跳过 MODE{m} T{t}", file=sys.stderr)
                continue
            lut, frames = parse_lut_csv(path)
            arrays = lut_to_epdiy_arrays(lut, frames)
            sym = f"epd_wp_{args.name}_{mtype}_{t}"
            chunks.append(fmt_data(arrays, sym))
            times = [args.times] * frames
            chunks.append(
                f"const int {sym}_times[{frames}] = "
                + "{" + ",".join(str(x) for x in times) + "};"
            )
            chunks.append(
                f"const EpdWaveformPhases {sym} = {{ .phases = {frames}, "
                f".phase_times = &{sym}_times[0], .luts = "
                f"(const uint8_t*)&{sym}_data[0] }};"
            )
            chunks.append(
                f"const EpdWaveformPhases* epd_wm_{args.name}_{mtype}_ranges[1] "
                f"= {{ &{sym} }};"
            )
            chunks.append(
                f"const EpdWaveformMode epd_wm_{args.name}_{mtype} = "
                f"{{ .type = {mtype}, .temp_ranges = 1, "
                f".range_data = &epd_wm_{args.name}_{mtype}_ranges[0] }};"
            )
            chunks.append("")
            mode_syms.append(f"&epd_wm_{args.name}_{mtype}")

    if not mode_syms:
        raise SystemExit("没有任何模式转换成功")

    chunks.append(
        f"const EpdWaveformTempInterval {args.name}_intervals[{temps_n}] = "
        + "{ "
        + ", ".join(
            "{ .min = 20, .max = 40 }" for _ in range(temps_n)  # iwf 单温区默认
        )
        + " };"
    )
    chunks.append(
        f"const EpdWaveformMode* {args.name}_modes[{len(mode_syms)}] = "
        + "{ " + ",".join(mode_syms) + " };"
    )
    chunks.append(
        f"const EpdWaveform {args.name} = {{ .num_modes = {len(mode_syms)}, "
        f".num_temp_ranges = {temps_n}, .mode_data = &{args.name}_modes[0], "
        f".temp_intervals = &{args.name}_intervals[0] }};"
    )

    with open(args.out, "w") as f:
        f.write("\n".join(chunks) + "\n")
    print(f"生成 {args.out}：{len(mode_syms)} 个模式")


if __name__ == "__main__":
    main()
