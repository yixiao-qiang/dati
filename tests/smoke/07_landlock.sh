#!/usr/bin/env bash
# Wave 0 · 任务 0.7：验证 Landlock 可用性（可选，不阻塞 Wave 1）
# 验收：/sys/kernel/security/lsm 包含 landlock；不可用则降级为"只读挂载"，本项标注降级
# 用法：sudo bash 07_landlock.sh
set -uo pipefail

PASS=0; FAIL=0; WARN=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
warn() { echo "  [WARN] $*"; WARN=$((WARN + 1)); }

echo "== 任务 0.7：Landlock 可用性 =="

KREL=$(uname -r)
echo "内核版本：$KREL（Landlock 需 5.13+）"

# 判定 1：LSM 列表
if [[ -r /sys/kernel/security/lsm ]]; then
  LSM=$(cat /sys/kernel/security/lsm)
  echo "LSM 列表：$LSM"
  if printf '%s' "$LSM" | tr ',' '\n' | grep -qx 'landlock'; then
    ok "landlock 在 LSM 列表中 → 路径级管控可用"
  else
    warn "landlock 不在 LSM 列表中 → **降级**：路径管控改用只读挂载（不阻塞 Wave 1）"
  fi
else
  bad "/sys/kernel/security/lsm 不可读（缺 securityfs？）"
fi

# 判定 2：内核编译开关
CFG="/boot/config-$KREL"
if [[ -r "$CFG" ]]; then
  V=$(grep -E '^CONFIG_SECURITY_LANDLOCK=' "$CFG" || echo "CONFIG_SECURITY_LANDLOCK=<未找到>")
  echo "内核配置：$V"
  if [[ "$V" == "CONFIG_SECURITY_LANDLOCK=y" ]]; then
    ok "内核编译已启用 LANDLOCK"
  else
    warn "内核未启用 LANDLOCK（$V）→ 同上，降级处理"
  fi
else
  warn "读不到 $CFG（内核配置未落盘），跳过编译开关检查"
fi

echo "汇总：PASS=$PASS WARN=$WARN FAIL=$FAIL"
# 本项不阻塞：只要没有硬失败即通过
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
