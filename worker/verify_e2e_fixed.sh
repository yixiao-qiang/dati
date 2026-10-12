#!/bin/bash
# 台阶1 端到端验证（修正版）：
# 顺序必须是：先建组 → 再 XADD。否则 $ 起点会跳过已存在的消息（注释里的坑）
set -e

echo "=== 1. 删掉旧流（含旧组）==="
redis-cli DEL judge:stream

echo "=== 2. 先建消费组（空流，$ 起点 OK）==="
redis-cli XGROUP CREATE judge:stream workers "$" MKSTREAM

echo "=== 3. 再 XADD 入队（此刻才是'新消息'）==="
redis-cli XADD judge:stream "*" submission_id 4

echo "=== 4. 确认 submission id=4 是 pending ==="
mysql -uoj -poj123456 -e "USE oj; SELECT id, status FROM submission WHERE id=4;" 2>&1 | grep -v "Using a password"

echo "=== 5. 启动 worker 消费（8 秒）==="
cd ~/work/dati/worker
export JUDGE_MAIN=~/work/dati/sandbox/tests/smoke/build/judge_main
timeout 8 ./build/judge_worker 2>&1 || true

echo ""
echo "=== 6. 验证 DB 状态（应变成 accepted）==="
mysql -uoj -poj123456 -e "USE oj; SELECT id, status, result, stdout, judge_started_at FROM submission WHERE id=4;" 2>&1 | grep -v "Using a password"

echo "=== 7. 验证 Redis（XPENDING 应=0）==="
redis-cli XPENDING judge:stream workers
redis-cli XINFO GROUPS judge:stream
