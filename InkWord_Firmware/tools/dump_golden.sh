#!/usr/bin/env bash
# dump_golden.sh —— 黄金帧基线一键采集（多屏兼容 P1-a 配套，2026-09-05）
#
# 把「临时钉默认面板 → 烧 demo 自检固件 → 串口捕获 dump → 恢复 ini」的
# 手工流程封装为一条命令，降低逐屏补基线的摩擦（PANEL_COMPAT_DESIGN
# §十六 SOP / src/selftest_golden.h 头注释流程的自动化版）。
#
# 用法：
#   tools/dump_golden.sh <panel_id> [dump_file] [serial_port]
#   例：tools/dump_golden.sh depg0370_uc8253
#       tools/dump_golden.sh wft0290_bw /tmp/golden_wft0290.txt
#
# 流程：
#   1. 备份 platformio.ini（trap 保证任何退出路径都恢复原文件）
#   2. 向 [env:inkword-s3-demo] 临时注入 -D EPD_PANEL_DEFAULT_ID=<panel>
#   3. 构建 + 烧录 inkword-s3-demo（自检固件，跑完序列后挂起）
#   4. 串口捕获，直到日志出现 "selftest done"（420s 超时兜底——三色屏
#      多页全刷 14.6s/页较慢）
#   5. 校验 dump 内 [GOLDEN] BEGIN/END 段配对并打印页清单
#
# 注意：多数 USB-UART 板 pio monitor 建连时会经 DTR/RTS 触发复位，可捕获
# 完整日志；若首页缺失（BEGIN 页 id 不从首页开始），按板载 RST 后用
# monitor-only 段重跑（见文末提示）。
#
# 采集完成后生成/合并基线（gen_golden.py 契约：多屏单表累积，
# 勿逐屏 -o 覆盖丢前屏）：
#   cat /tmp/golden_*.txt | python3 tools/gen_golden.py /dev/stdin \
#       -o src/selftest_golden.h
#   然后重编译烧录 demo env 复跑，自检应从 NO BASELINE 转 PASS。

set -euo pipefail

FW_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PIO_PY="$HOME/.platformio/penv/bin/python"
INI="$FW_DIR/platformio.ini"

PANEL="${1:-}"
DUMP="${2:-/tmp/golden_${PANEL:-x}.txt}"
PORT="${3:-$(ls /dev/cu.usbserial* 2>/dev/null | head -1 || true)}"

[ -n "$PANEL" ] || { echo "用法: $0 <panel_id> [dump_file] [serial_port]" >&2; exit 1; }
[ -n "$PORT" ] || { echo "错误: 未找到串口设备（/dev/cu.usbserial*），请接板或以第三参数指定" >&2; exit 1; }
[ -x "$PIO_PY" ] || { echo "错误: 未找到 PlatformIO penv Python（$PIO_PY）" >&2; exit 1; }

# 串口被残留 monitor 占用时烧录会失败，先做无害检测
if lsof "$PORT" >/dev/null 2>&1; then
    echo "警告: $PORT 已被占用（残留 monitor？），尝试继续…" >&2
fi

INI_BAK="$(mktemp /tmp/pio_ini_bak.XXXXXX)"
cp "$INI" "$INI_BAK"
restore_ini() {
    cp "$INI_BAK" "$INI" && rm -f "$INI_BAK"
}
trap restore_ini EXIT

cd "$FW_DIR"

echo "==> [1/4] 注入默认面板 '$PANEL'（demo env，用后自动恢复）"
python3 - "$PANEL" <<'PYEOF'
import sys

panel = sys.argv[1]
p = "platformio.ini"
s = open(p, encoding="utf-8").read()
anchor = "-D INKWORD_GOLDEN_FRAME=1"
assert anchor in s, "锚点缺失：demo env 的 GOLDEN_FRAME 行不在？ini 结构已变"
# 残留检测用注入标记而非 EPD_PANEL_DEFAULT_ID 泛匹配：demo env 后相邻的
# 注释块（面板 env 说明）合法含有该宏名，泛匹配会误判
assert "dump_golden.sh 临时注入" not in s, \
    "检测到上次注入残留（脚本异常退出？），请人工核对 platformio.ini"
# 按行插入到锚点行之后（锚点行带行内注释，不能在锚点子串后拼接）
lines = s.split("\n")
for i, ln in enumerate(lines):
    if anchor in ln:
        lines.insert(i + 1, '    \'-D EPD_PANEL_DEFAULT_ID="%s"\'  ; dump_golden.sh 临时注入（自动恢复）' % panel)
        break
else:
    raise AssertionError("锚点行未找到")
s = "\n".join(lines)
open(p, "w", encoding="utf-8").write(s)
PYEOF

echo "==> [2/4] 构建 + 烧录 inkword-s3-demo → $PORT"
"$PIO_PY" -m platformio run -e inkword-s3-demo -t upload --upload-port "$PORT"

echo "==> [3/4] 串口捕获（等待 selftest done，最长 420s）→ $DUMP"
rm -f "$DUMP"
"$PIO_PY" -m platformio device monitor --port "$PORT" >"$DUMP" 2>&1 &
MON_PID=$!
DONE=0
for _ in $(seq 1 84); do
    if grep -q "selftest done" "$DUMP" 2>/dev/null; then DONE=1; break; fi
    if ! kill -0 "$MON_PID" 2>/dev/null; then echo "警告: monitor 提前退出" >&2; break; fi
    sleep 5
done
sleep 2   # 尾部 flush
kill "$MON_PID" 2>/dev/null || true
wait "$MON_PID" 2>/dev/null || true

echo "==> [4/4] 校验 dump"
BEGINS=$(grep -c "^\[GOLDEN\] BEGIN" "$DUMP" || true)
ENDS=$(grep -c "^\[GOLDEN\] END" "$DUMP" || true)
echo "    面板页段：BEGIN=$BEGINS END=$ENDS $( [ "$DONE" = 1 ] && echo '/ selftest done ✓' || echo '/ 超时未见完成标记 ⚠' )"
if [ "$BEGINS" -gt 0 ]; then
    grep "^\[GOLDEN\] BEGIN" "$DUMP" | sed 's/^/    页: /'
fi
if [ "$BEGINS" = 0 ] || [ "$BEGINS" != "$ENDS" ]; then
    echo "错误: dump 段不完整。按板载 RST 后可仅重跑捕获段：" >&2
    echo "  $PIO_PY -m platformio device monitor --port $PORT | tee $DUMP" >&2
    exit 2
fi

echo ""
echo "✓ 采集完成：$DUMP（$BEGINS 页）"
echo "全部目标屏采集完后，累积生成基线并复验："
echo "  cat /tmp/golden_*.txt | python3 tools/gen_golden.py /dev/stdin -o src/selftest_golden.h"
echo "  $PIO_PY -m platformio run -e inkword-s3-demo -t upload --upload-port $PORT   # 复跑应转 PASS"
