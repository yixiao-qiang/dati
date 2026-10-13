#!/bin/bash
# 台阶 2 崩溃恢复验证 —— B 层：端到端（worker 自动接管 + 完整判题）
# 前提：main.cpp 已临时压缩 kClaimIdleMs=1000(1s)、kClaimScanMs=500(500ms)
# 场景：消息被读走不 ACK（模拟崩溃）→ 启动 worker
#       → 500ms 内主循环扫到死任务 → XAUTOCLAIM 接管 → process_one 完整判题
#       → DB 写终态 → XACK。全程无人工介入。

cd ~/work/dati/worker

echo "========== 0. 插入新提交 =========="
mysql -uoj -poj123456 -e "
USE oj;
INSERT INTO submission(code, language) VALUES (
'#include <cstdio>
int main(){puts(\"recovered by claim\");}',
'cpp');" 2>&1 | grep -v "Using a password"
NEW_ID=$(mysql -uoj -poj123456 -N -e "USE oj; SELECT MAX(id) FROM submission;" 2>/dev/null)
echo "submission_id=$NEW_ID"

echo ""
echo "========== 1. 重建流 + 建组 + XADD =========="
redis-cli DEL judge:stream
redis-cli XGROUP CREATE judge:stream workers "$" MKSTREAM
MSG_ID=$(redis-cli XADD judge:stream "*" submission_id "$NEW_ID")
echo "消息 ID: $MSG_ID"

echo ""
echo "========== 2. 模拟 worker 崩溃：读走不 ACK =========="
redis-cli XREADGROUP GROUP workers crashed COUNT 10 STREAMS judge:stream ">"
echo "--- XPENDING（应 1 条，consumer=crashed）---"
redis-cli XPENDING judge:stream workers

echo ""
echo "========== 3. 等 2 秒让 idle 超过 1s 判死阈值 =========="
sleep 2
echo "--- XPENDING（idle 应 >1000ms）---"
redis-cli XPENDING judge:stream workers - + 10

echo ""
echo "========== 4. 启动 worker（8 秒，应自动接管并判题）=========="
export JUDGE_MAIN=~/work/dati/sandbox/tests/smoke/build/judge_main
timeout 8 ./build/judge_worker 2>&1 | grep -v "MYSQL_OPT_RECONNECT"
echo "worker exit: $?"

echo ""
echo "========== 5. DB 结果（应 accepted + 'recovered by claim'）=========="
mysql -uoj -poj123456 -e "USE oj; SELECT id, status, stdout FROM submission WHERE id=$NEW_ID;" 2>&1 | grep -v "Using a password"

echo ""
echo "========== 6. XPENDING（应 0，已被接管并 ACK）=========="
redis-cli XPENDING judge:stream workers

echo ""
echo "========== B 层完成：崩溃恢复端到端 ✅ =========="
