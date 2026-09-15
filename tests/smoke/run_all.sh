#!/usr/bin/env bash
# Wave 0 冒烟总入口
# 用法：
#   sudo bash run_all.sh            # 跑全部 8 条
#   sudo bash run_all.sh 0.1 0.3    # 只跑指定几条
# 退出码：全绿 0，否则 1
set -uo pipefail

cd "$(dirname "$0")" || exit 2
SMOKE_DIR="$(pwd)"
ART="$SMOKE_DIR/artifacts"
mkdir -p "$ART"
LOG="$ART/run.log"

# ---- 用例表 ----
declare -A CMD=(
  [0.1]="bash 01_namespace.sh"
  [0.2]="bash 02_cgroup_v2.sh"
  [0.3]="build/test_seccomp"
  [0.4]="bash 04_strace_gpp.sh"
  [0.5]="build/test_pivot_root"
  [0.6]="build/test_cgroup_migrate"
  [0.7]="bash 07_landlock.sh"
  [0.8]="bash 08_redis_stream.sh"
)
ORDER=(0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8)

if [[ $# -gt 0 ]]; then SELECTED=("$@"); else SELECTED=("${ORDER[@]}"); fi

# ---- 环境快照 ----
{
  echo "==================== Wave 0 冒烟运行 ===================="
  echo "时间   : $(date -Is)"
  echo "用户   : $(id -un) (uid=$(id -u))"
  echo "主机   : $(hostname)"
  echo "系统   : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
  echo "内核   : $(uname -r)"
  echo "g++    : $(g++ --version 2>/dev/null | head -1)"
  echo "========================================================"
} | tee "$LOG"

# ---- 需要 C++ 时先构建 ----
need_cpp=0
for t in "${SELECTED[@]}"; do
  [[ "$t" == "0.3" || "$t" == "0.5" || "$t" == "0.6" ]] && need_cpp=1
done
if [[ $need_cpp -eq 1 ]]; then
  echo ""
  echo "== 构建 C++ 冒烟 ==" | tee -a "$LOG"
  if command -v cmake >/dev/null 2>&1; then
    if cmake -B build -S . >>"$LOG" 2>&1 && cmake --build build >>"$LOG" 2>&1; then
      echo "  构建成功" | tee -a "$LOG"
    else
      echo "  !! CMake 构建失败（0.3/0.5/0.6 将无法运行）" | tee -a "$LOG"
    fi
  else
    echo "  !! 未找到 cmake" | tee -a "$LOG"
  fi
fi

# ---- 逐条执行 ----
declare -A RESULT
for t in "${SELECTED[@]}"; do
  cmd="${CMD[$t]:-}"
  if [[ -z "$cmd" ]]; then
    echo "未知用例：$t" | tee -a "$LOG"; RESULT[$t]="UNKNOWN"; continue
  fi
  {
    echo ""
    echo "########## [$t] $cmd ##########"
  } | tee -a "$LOG"

  out="$(bash -c "$cmd" 2>&1)"; rc=$?
  echo "$out" | tee -a "$LOG"

  if [[ $rc -eq 0 ]]; then RESULT[$t]="PASS"; else RESULT[$t]="FAIL(rc=$rc)"; fi
done

# ---- 汇总 ----
{
  echo ""
  echo "==================== 汇总 ===================="
} | tee -a "$LOG"
fails=0
for t in "${SELECTED[@]}"; do
  r="${RESULT[$t]:-SKIP}"
  printf "  %-5s %s\n" "$t" "$r" | tee -a "$LOG"
  [[ "$r" == "PASS" ]] || fails=$((fails + 1))
done
{
  echo "=============================================="
  echo "产物目录：$ART"
} | tee -a "$LOG"

if [[ $fails -eq 0 ]]; then
  echo "全部通过 ✅"
  exit 0
else
  echo "有 $fails 条未通过 ❌"
  exit 1
fi
