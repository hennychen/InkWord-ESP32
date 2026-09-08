#!/usr/bin/env bash
# FSRS 双端对拍一键脚本（A6 收口）
# 双端读同一份 tools/fsrs_test_vectors.csv（gen_fsrs_vectors.py 生成），
# 对拍目的：双端实现一致性与回归防护，非算法正确性证明。
#
# 用法：./tools/run_fsrs_crosscheck.sh
# 退出码：0=双端全绿，非 0=任一端失败

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== FSRS 双端对拍 ==="
echo ""

# 1. 重新生成测试向量（确保 CSV + 固件头文件同步）
echo "[1/3] 生成测试向量..."
python3 "$SCRIPT_DIR/gen_fsrs_vectors.py"
echo ""

# 2. 后端 C# 对拍
echo "[2/3] 后端 .NET 对拍（FsrsServiceTests）..."
cd "$ROOT_DIR/InkWord_Backend"
dotnet test --filter "FsrsServiceTests" --verbosity quiet
echo "✓ 后端对拍通过"
echo ""

# 3. 固件 C 对拍
echo "[3/3] 固件 native-test 对拍（test_srs_engine）..."
cd "$ROOT_DIR/InkWord_Firmware"
~/.platformio/penv/bin/pio test -e native-test -f "test_srs_engine" --verbose 2>&1 | grep -E "PASSED|FAILED|succeeded|failed"
if [ ${PIPESTATUS[0]} -eq 0 ]; then
    echo "✓ 固件对拍通过"
else
    echo "✗ 固件对拍失败"
    exit 1
fi

echo ""
echo "=== 双端对拍完成 ==="
echo "双端读同一份 CSV，评分结果一致（容差：S/D 相对 1e-4，interval 整数精确相等）"
