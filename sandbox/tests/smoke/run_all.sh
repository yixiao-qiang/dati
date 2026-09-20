#!/bin/bash
# Wave 1 · 冒烟测试总入口
# 用法：sudo bash run_all.sh [测试编号...]
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
ARTIFACTS_DIR="$SCRIPT_DIR/artifacts"
mkdir -p "$BUILD_DIR" "$ARTIFACTS_DIR"

echo "==================== Wave 1 冒烟运行 ===================="
echo "时间   : $(date -Iseconds)"
echo "用户   : $(whoami) (uid=$(id -u))"
echo "主机   : $(hostname)"
echo "系统   : $(. /etc/os-release && echo "$PRETTY_NAME")"
echo "内核   : $(uname -r)"
echo "g++    : $(g++ --version | head -1)"
echo "========================================================"

# 构建
echo "== 构建沙箱与测试 =="
if ! cmake -S "$SCRIPT_DIR/../.." -B "$BUILD_DIR" -G Ninja > "$ARTIFACTS_DIR/cmake.log" 2>&1; then
    echo "  cmake 失败，见 $ARTIFACTS_DIR/cmake.log"
    tail -20 "$ARTIFACTS_DIR/cmake.log"
    exit 1
fi
if ! cmake --build "$BUILD_DIR" >> "$ARTIFACTS_DIR/build.log" 2>&1; then
    echo "  构建失败，见 $ARTIFACTS_DIR/build.log"
    tail -20 "$ARTIFACTS_DIR/build.log"
    exit 1
fi
echo "  构建成功"

# 测试列表（编号:可执行文件）
TESTS=(
    "00:test_00_status"
    "01:test_01_hello"
    "02:test_02_seccomp"
    "03:test_03_fs_isolation"
    "04:test_04_mle"
    "05:test_05_tle"
)

PASS=0
FAIL=0
FAILED_LIST=""

for entry in "${TESTS[@]}"; do
    num="${entry%%:*}"
    bin="${entry##*:}"

    # 如果指定了测试编号，只跑指定的
    if [ $# -gt 0 ]; then
        skip=1
        for arg in "$@"; do
            if [ "$arg" = "$num" ]; then skip=0; fi
        done
        [ $skip -eq 1 ] && continue
    fi

    echo ""
    echo "########## [$num] $bin ##########"
    "$BUILD_DIR/tests/smoke/$bin" 2>&1 | tee "$ARTIFACTS_DIR/${num}_${bin}.log"
    rc=${PIPESTATUS[0]}
    if [ $rc -eq 0 ]; then
        echo "  => PASS"
        PASS=$((PASS + 1))
    else
        echo "  => FAIL (rc=$rc)"
        FAIL=$((FAIL + 1))
        FAILED_LIST="$FAILED_LIST $num"
    fi
done

echo ""
echo "==================== 汇总 ===================="
echo "  PASS=$PASS  FAIL=$FAIL"
if [ $FAIL -gt 0 ]; then
    echo "  未通过:$FAILED_LIST"
fi
echo "=============================================="
echo "产物目录：$ARTIFACTS_DIR"

if [ $FAIL -eq 0 ]; then
    echo "全部通过 ✅"
    exit 0
else
    echo "有 $FAIL 条未通过 ❌"
    exit 1
fi
