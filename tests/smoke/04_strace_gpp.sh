#!/usr/bin/env bash
# Wave 0 · 任务 0.4：用 strace 记录 g++ 的系统调用序列
# 验收：得到 g++ 实际使用的 syscall 列表；断言 = 5 个 glibc 启动期必现项 + 3 个 syscall 族覆盖；产物落盘留档
# 实测（g++ 13.3.0 / glibc 2.39 / 内核 7.0，Ubuntu 24.04.3）：
#   进程创建走 vfork（不是旧文档按 g++ 11.4/glibc 2.35 预期的 clone3）；
#   文件状态走 newfstatat/fstat（不是 statx）；madvise 在最小编译负载下不出现。
#   白名单只认真实 trace，不认版本预期——这正是任务 0.4 的意义。
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

# 断言 A：现代 glibc 启动期必现的 syscall（在 g++ 13.3 / glibc 2.39 实测全部出现）
REQUIRED=(rseq set_robust_list mprotect arch_prctl getrandom)
for s in "${REQUIRED[@]}"; do
  if grep -qx "$s" "$ART/gpp_syscalls.txt"; then
    ok "包含 $s"
  else
    bad "缺少 $s（glibc 启动期应必现；若缺失，先检查 strace 是否漏了 -f）"
  fi
done

# 断言 B：syscall 族覆盖——具体成员随 gcc/glibc 版本变化，只断言“族内至少实测到一个”，
# 并打印实际命中的成员，让版本差异肉眼可见（Wave 1 白名单按命中的真实成员编写）。
check_family() {
  local label="$1"; shift
  local hit="" s
  for s in "$@"; do
    if grep -qx "$s" "$ART/gpp_syscalls.txt"; then hit="${hit:+$hit, }$s"; fi
  done
  if [[ -n "$hit" ]]; then
    ok "$label 命中：$hit"
  else
    bad "$label 未抓到任何成员（$*）"
  fi
}
check_family "进程创建族" clone3 clone vfork
check_family "文件状态族" statx newfstatat fstat
check_family "内存映射族" mmap mprotect munmap mremap

# 断言 C：信息项，不计失败
# madvise 是 malloc 归还/标注内存的可选 hint；hello world 最小编译负载下不触发属正常。
# Wave 1 编译期白名单先不必放行 madvise，等大编译负载实测后再决定是否补充。
if grep -qx madvise "$ART/gpp_syscalls.txt"; then
  echo "  [INFO] madvise 出现（大内存负载下的预期行为）"
else
  echo "  [INFO] madvise 未出现（最小编译负载下正常，不计失败）"
fi

echo "汇总：PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
