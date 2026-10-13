#!/bin/bash
# 台阶 2 崩溃恢复验证 —— A 层：队列层（XAUTOCLAIM 判死 + XCLAIM 心跳）
# 不用改代码，手动 redis-cli 模拟：
#   读走不 ACK（模拟崩溃）→ 判死接管 → 心跳归零 → PEL 状态流转
# 时间尺度压缩：判死阈值用 1ms（真实配置 10min），不等 10 分钟

cd ~/work/dati/worker

echo "========== 0. 准备：插入新提交 =========="
mysql -uoj -poj123456 -e "
USE oj;
INSERT INTO submission(code, language) VALUES (
'#include <cstdio>
int main(){puts(\"recovered by claim\");}',
'cpp');" 2>&1 | grep -v "Using a password"
NEW_ID=$(mysql -uoj -poj123456 -N -e "USE oj; SELECT MAX(id) FROM submission;" 2>/dev/null)
echo "submission_id=$NEW_ID"

echo ""
echo "========== 1. 重建流 + 建组 + XADD（K1 纪律：先建组再 XADD）=========="
redis-cli DEL judge:stream
redis-cli XGROUP CREATE judge:stream workers "$" MKSTREAM
MSG_ID=$(redis-cli XADD judge:stream "*" submission_id "$NEW_ID")
echo "消息 ID: $MSG_ID"

echo ""
echo "========== 2. 模拟 worker 崩溃：XREADGROUP 读走但不 ACK =========="
redis-cli XREADGROUP GROUP workers crashed COUNT 10 STREAMS judge:stream ">"
echo "--- XPENDING 明细（应 1 条，consumer=crashed，idle 开始涨）---"
redis-cli XPENDING judge:stream workers - + 10

echo ""
echo "========== 3. 判死接管：XAUTOCLAIM（阈值 1ms ≈ 判死）=========="
echo "--- 返回 [游标, 被接管消息] ---"
redis-cli XAUTOCLAIM judge:stream workers w1 1 0 COUNT 10
echo "--- XPENDING 明细（消息应已归 w1，delivery=2）---"
redis-cli XPENDING judge:stream workers - + 10

echo ""
echo "========== 4. 心跳续命：XCLAIM 把 idle 重置为 0 =========="
echo "--- XCLAIM 前 idle（上一步刚接管，应已很小）---"
redis-cli XPENDING judge:stream workers - + 10
echo "--- 执行 XCLAIM IDLE 0 ---"
redis-cli XCLAIM judge:stream workers w1 0 "$MSG_ID" IDLE 0
echo "--- XCLAIM 后 idle（应 ≈0，证明续命生效）---"
redis-cli XPENDING judge:stream workers - + 10

echo ""
echo "========== 5. 清理：ACK 掉这条模拟消息（避免污染后续测试）=========="
redis-cli XACK judge:stream workers "$MSG_ID"
echo "--- 最终 XPENDING（应 0）---"
redis-cli XPENDING judge:stream workers
echo ""
echo "========== A 层完成：队列命令全部验证 ✅ =========="
