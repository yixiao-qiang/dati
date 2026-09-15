#!/usr/bin/env bash
# Wave 0 · 任务 0.8：验证 Redis Stream 生产-消费-ACK 全流程
# 验收：完成全流程，XPENDING 显示 0 条未确认
# 用法：sudo bash 08_redis_stream.sh
#
# ⚠ 与计划原文的顺序差异（有意为之）：
#   计划示例是「先 XADD，再 XGROUP CREATE ... '$'」——那样消费组从流末尾开始，
#   刚 XADD 的消息不会被投递，XREADGROUP 返回空，XPENDING 也自然是 0（**假通过**）。
#   本脚本改为「先建组(from 0, MKSTREAM)，再 XADD」，并**断言 XREADGROUP 真的读到消息**。
set -uo pipefail

STREAM="judge:smoke:stream"
GROUP="workers"
CONSUMER="w1"

PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

echo "== 任务 0.8：Redis Stream =="

command -v redis-cli >/dev/null 2>&1 || { echo "缺少 redis-cli"; exit 2; }
if ! redis-cli PING >/dev/null 2>&1; then
  echo "Redis 不可达（redis-cli PING 失败）——先启动：sudo systemctl start redis-server"; exit 2
fi

VER=$(redis-cli INFO server | awk -F: '/^redis_version/{print $2}' | tr -d '\r')
echo "Redis 版本：$VER"

redis-cli DEL "$STREAM" >/dev/null 2>&1

# 1) 先建消费组（从 0 开始），确保后续 XADD 的消息可被投递
if redis-cli XGROUP CREATE "$STREAM" "$GROUP" 0 MKSTREAM >/dev/null 2>&1; then
  ok "XGROUP CREATE $GROUP（from 0, MKSTREAM）"
else
  bad "XGROUP CREATE 失败"; echo "汇总：PASS=$PASS FAIL=$FAIL"; exit 1
fi

# 2) 生产
ID=$(redis-cli XADD "$STREAM" '*' submission_id 1 | tr -d '\r')
if [[ -n "$ID" ]]; then ok "XADD → $ID"; else bad "XADD 失败"; fi

# 3) 消费（关键：必须真的读到）
OUT=$(redis-cli XREADGROUP GROUP "$GROUP" "$CONSUMER" COUNT 1 STREAMS "$STREAM" '>' 2>&1)
if printf '%s' "$OUT" | grep -q "$ID"; then
  ok "XREADGROUP 读到消息 $ID"
else
  bad "XREADGROUP 未读到消息（输出：$(printf '%s' "$OUT" | tr '\n' ' ')）"
fi

# 4) ACK
ACKED=$(redis-cli XACK "$STREAM" "$GROUP" "$ID" | tr -d '\r')
if [[ "$ACKED" == "1" ]]; then ok "XACK 成功"; else bad "XACK 返回 $ACKED"; fi

# 5) XPENDING 应为 0
PEND=$(redis-cli XPENDING "$STREAM" "$GROUP" 2>/dev/null | head -1 | tr -d '\r')
if [[ "$PEND" == "0" ]]; then
  ok "XPENDING = 0（无未确认消息）"
else
  bad "XPENDING = ${PEND:-空}（应为 0）"
fi

# 6) 附加：XAUTOCLAIM 可用性（Wave 2 任务 2.2 前置）
if redis-cli XAUTOCLAIM "$STREAM" "$GROUP" "$CONSUMER" 60000 0-0 COUNT 10 >/dev/null 2>&1; then
  ok "XAUTOCLAIM 可用（Wave 2 任务 2.2 前置）"
else
  bad "XAUTOCLAIM 不可用（Redis < 6.2？）"
fi

redis-cli DEL "$STREAM" >/dev/null 2>&1

echo "汇总：PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
