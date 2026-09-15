#!/usr/bin/env bash
# Wave 0 · 任务 0.4：用 strace 记录 g++ 的系统调用序列
# 验收：得到 g++ 实际使用的 syscall 列表，确认包含 clone3/statx/rseq/...；产物落盘留档
# 产物：artifacts/gpp_trace.txt（完整 trace）、artifacts/gpp_syscalls.txt（去重列表，Wave 1 白名单输入）
# 用法：sudo bash 04_strace_gpp.sh
set -uo pipefail

cd "$(dirname "$0")" || exit 2
ART="$(pwd)/artifacts"
mkdir -p "$ART"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

echo "== 任务 0.4：strace 记录 g++ syscall 序列 =="

command -v strace >/dev/null 2>&1 || { echo "缺少 strace（apt install strace）"; exit 2; }
command -v g++    >/dev/null 2>&1 || { echo "缺少 g++"; exit 2; }

# 准备最小样本
cat > "$WORK/hello.cpp" <<'EOF'
#include <cstdio>
int main() { std::printf("hello\n"); return 0; }
EOF

echo "g++ 版本：$(g++ --version | head -1)" | tee "$ART/gpp_version.txt"

# 追踪整个编译过程（-f 跟随 g++ 派生出的 cc1plus/as/collect2）
if strace -f -e trace=all -o "$ART/gpp_trace.txt" \
        g++ -O2 "$WORK/hello.cpp" -o "$WORK/hello" 2>"$WORK/strace_err.txt"; then
  ok "strace 记录完成 → $ART/gpp_trace.txt（$(wc -l < "$ART/gpp_trace.txt") 行）"
else
  bad "strace/编译失败"; sed -n '1,20p' "$WORK/strace_err.txt"
  echo "汇总：PASS=$PASS FAIL=$FAIL"; exit 1
fi

# 提取去重的 syscall 名
grep -oP '^\d+\s+\K\w+' "$ART/gpp_trace.txt" | sort -u > "$ART/gpp_syscalls.txt"
N=$(wc -l < "$ART/gpp_syscalls.txt")
if [[ "$N" -gt 0 ]]; then
  ok "提取 syscall 列表：$N 个 → $ART/gpp_syscalls.txt"
else
  bad "未能提取 syscall（trace 格式异常）"
  echo "汇总：PASS=$PASS FAIL=$FAIL"; exit 1
fi

# 必须包含的关键 syscall（计划点名的 8 个）
REQUIRED=(clone3 statx rseq set_robust_list mprotect madvise arch_prctl getrandom)
for s in "${REQUIRED[@]}"; do
  if grep -qx "$s" "$ART/gpp_syscalls.txt"; then
    ok "包含 $s"
  else
    bad "缺少 $s"
  fi
done

echo "汇总：PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
