#!/bin/bash
# 台阶 4 Outbox 验证
# 场景：模拟 server 用"一个事务"写 submission + outbox（原子）
#       → worker 投递器 2s 内扫到 → XADD → mark_delivered → 消费判题
#       → 再跑一次 worker 应无重复投递（delivered=1 防重）

cd ~/work/dati/worker

echo "========== 0. 模拟 server 事务写（submission + outbox 原子）=========="
mysql -uoj -poj123456 -e "
USE oj;
START TRANSACTION;
INSERT INTO submission(code, language) VALUES (
'#include <cstdio>
int main(){puts(\"via outbox\");}',
'cpp');
SET @sid = LAST_INSERT_ID();
INSERT INTO outbox(submission_id, event, payload) VALUES (@sid, 'judge', '{}');
COMMIT;
SELECT @sid AS sid;
" 2>&1 | grep -v "Using a password"
SID=$(mysql -uoj -poj123456 -N -e "USE oj; SELECT MAX(id) FROM submission;" 2>/dev/null)
echo "submission_id=$SID"

echo "--- outbox 表（应 1 条 delivered=0 待投递）---"
mysql -uoj -poj123456 -e "USE oj; SELECT id, submission_id, delivered FROM outbox WHERE submission_id=$SID;" 2>&1 | grep -v "Using a password"

echo ""
echo "========== 1. 启动 worker（8 秒，投递器应自动投递）=========="
export JUDGE_MAIN=~/work/dati/sandbox/tests/smoke/build/judge_main
timeout 8 ./build/judge_worker 2>&1 | grep -v "MYSQL_OPT_RECONNECT"
echo "worker exit: $?"

echo ""
echo "========== 2. DB 判题结果（应 accepted + 'via outbox'）=========="
mysql -uoj -poj123456 -e "USE oj; SELECT id, status, stdout FROM submission WHERE id=$SID;" 2>&1 | grep -v "Using a password"

echo ""
echo "========== 3. outbox 标记（应 delivered=1 已投递）=========="
mysql -uoj -poj123456 -e "USE oj; SELECT id, submission_id, delivered FROM outbox WHERE submission_id=$SID;" 2>&1 | grep -v "Using a password"

echo ""
echo "========== 4. XPENDING（应 0，已 ACK）=========="
redis-cli XPENDING judge:stream workers

echo ""
echo "========== 5. 再跑一次 worker（应无 outbox 投递日志 = 不重复投递）=========="
timeout 4 ./build/judge_worker 2>&1 | grep -v "MYSQL_OPT_RECONNECT" | grep "outbox" || echo "（无 outbox 投递日志 ✅ delivered=1 防重复生效）"
echo ""
echo "========== 台阶 4 验证完成 =========="
