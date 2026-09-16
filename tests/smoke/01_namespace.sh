#!/usr/bin/env bash
# Wave 0 · 任务 0.1：验证 namespace 隔离
# 验收：隔离内看不到宿主挂载；pid/net 隔离生效；退出后宿主无残留
# RED 信号：unshare: unshare failed: Operation not permitted
# 用法：sudo bash 01_namespace.sh
set -uo pipefail

PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

echo "== 任务 0.1：namespace 隔离 =="

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "需要 root：sudo bash $0"; exit 2
fi

HOST_MOUNTS=$(wc -l < /proc/self/mountinfo)
HOST_NETIF=$(ls /sys/class/net | tr '\n' ' ')
echo "宿主基线：mountinfo=$HOST_MOUNTS 行；网卡=[$HOST_NETIF]"

INNER=$(unshare --mount --pid --net --fork --mount-proc /bin/bash -c '
  printf "MOUNTS=%s\n" "$(wc -l < /proc/self/mountinfo)"
  printf "PROCS=%s\n"  "$(ls -1 /proc | grep -cE "^[0-9]+$")"
  printf "NETIF=%s\n"  "$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d " " | tr "\n" ",")"
' 2>&1)
RC=$?

echo "--- 隔离内输出 ---"
echo "$INNER"
echo "-------------------"

if [[ $RC -ne 0 ]]; then
  bad "unshare 失败（rc=$RC）——RED 信号：Operation not permitted"
  echo "汇总：PASS=$PASS FAIL=$FAIL"; exit 1
fi

NS_MOUNTS=$(printf '%s\n' "$INNER" | sed -n 's/^MOUNTS=//p' | head -1)
NS_PROCS=$(printf '%s\n' "$INNER"  | sed -n 's/^PROCS=//p'  | head -1)
NS_NETIF=$(printf '%s\n' "$INNER"  | sed -n 's/^NETIF=//p'  | head -1)

# 判定 1：mount namespace
if [[ -n "$NS_MOUNTS" && "$NS_MOUNTS" -lt "$HOST_MOUNTS" ]]; then
  ok "mount namespace 生效（隔离内 $NS_MOUNTS < 宿主 $HOST_MOUNTS）"
else
  bad "mount namespace 可疑（隔离内 ${NS_MOUNTS:-?} vs 宿主 $HOST_MOUNTS）"
fi

# 判定 2：pid namespace（隔离内只应有极少数进程）
if [[ -n "$NS_PROCS" && "$NS_PROCS" -le 5 ]]; then
  ok "pid namespace 生效（隔离内进程数 $NS_PROCS）"
else
  bad "pid namespace 可疑（隔离内进程数 ${NS_PROCS:-?}）"
fi

# 判定 3：net namespace（隔离内只应有 lo）
if [[ "$NS_NETIF" == "lo," || "$NS_NETIF" == "lo" ]]; then
  ok "net namespace 生效（隔离内网卡仅 lo）"
else
  bad "net namespace 可疑（隔离内网卡=[$NS_NETIF]）"
fi

# 判定 4：退出后宿主无残留
HOST_MOUNTS_AFTER=$(wc -l < /proc/self/mountinfo)
if [[ "$HOST_MOUNTS_AFTER" == "$HOST_MOUNTS" ]]; then
  ok "退出后宿主无残留（mountinfo 仍为 $HOST_MOUNTS 行）"
else
  bad "宿主 mountinfo 变化（$HOST_MOUNTS -> $HOST_MOUNTS_AFTER）"
fi

echo "汇总：PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
