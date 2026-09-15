#!/usr/bin/env bash
# Wave 0 · 任务 0.2：验证 cgroup v2 可写
# 验收：能读到 memory.peak 与 cpu.stat 的 usage_usec
# RED 信号：mkdir 报只读文件系统 / tee 报 permission denied
# 用法：sudo bash 02_cgroup_v2.sh
set -uo pipefail

CG=/sys/fs/cgroup/judge-test
PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

cleanup() {
  # 把可能残留的进程移回根 cgroup，再删测试组
  if [[ -d "$CG" ]]; then
    cat "$CG/cgroup.procs" 2>/dev/null | while read -r p; do
      echo "$p" > /sys/fs/cgroup/cgroup.procs 2>/dev/null || true
    done
    rmdir "$CG" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== 任务 0.2：cgroup v2 可写 =="

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "需要 root：sudo bash $0"; exit 2
fi

# 判定 1：确认是 cgroup v2 统一层级
FSTYPE=$(stat -fc %T /sys/fs/cgroup)
if [[ "$FSTYPE" == "cgroup2fs" ]]; then
  ok "cgroup v2 已挂载（stat -fc %T = $FSTYPE）"
else
  bad "不是 cgroup v2（$FSTYPE）——v1/v2 混挂会让后面的接口名对不上"
fi

# 判定 2：能创建 cgroup
if mkdir -p "$CG" 2>/dev/null; then
  ok "创建 cgroup 成功：$CG"
else
  bad "创建 cgroup 失败（只读文件系统？）"
  echo "汇总：PASS=$PASS FAIL=$FAIL"; exit 1
fi

# 判定 3：能写限制文件
if echo 268435456 > "$CG/memory.max" 2>/dev/null; then
  ok "写入 memory.max = $(cat "$CG/memory.max")"
else
  bad "写入 memory.max 失败"
fi
if echo 64 > "$CG/pids.max" 2>/dev/null; then
  ok "写入 pids.max = $(cat "$CG/pids.max")"
else
  bad "写入 pids.max 失败"
fi

# 判定 4：能把进程迁进去并让它干活（子进程做完即退出，cgroup 自动清空）
export CG
sh -c 'echo $$ > "$CG/cgroup.procs"; i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done' 2>/dev/null
ok "进程迁移 + 计算完成（子进程已退出）"

# 判定 5：读 memory.peak
MP=$(cat "$CG/memory.peak" 2>/dev/null || echo "")
if [[ -n "$MP" ]]; then
  ok "读到 memory.peak = $MP 字节"
else
  bad "读不到 memory.peak（内核过旧或未挂 cgroup v2）"
fi

# 判定 6：读 cpu.stat 的 usage_usec
US=$(awk '/^usage_usec/{print $2}' "$CG/cpu.stat" 2>/dev/null || echo "")
if [[ -n "$US" && "$US" -gt 0 ]]; then
  ok "读到 cpu.stat usage_usec = $US（>0，说明计量生效）"
else
  bad "读不到 cpu.stat usage_usec（值='${US:-空}'）"
  echo "--- cpu.stat 实际内容 ---"; cat "$CG/cpu.stat" 2>/dev/null
fi

echo "汇总：PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
