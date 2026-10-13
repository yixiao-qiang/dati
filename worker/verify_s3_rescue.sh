#!/bin/bash
# 台阶 3 rescue 验证 —— 场景 A：judging 卡死恢复；场景 B：重试超限转死信
# 前提：worker 启动即扫一次 rescue（last_rescue_scan_ms=0），无需等 60s

cd ~/work/dati/worker

echo "================ 场景 A：judging 卡死 → rescue 恢复 ================"
echo "--- 0. 插入提交 ---"
mysql -uoj -poj123456 -e "
USE oj;
INSERT INTO submission(code, language) VALUES (
'#include <cstdio>
int main(){puts(\"rescue me\");}',
'cpp');" 2>&1 | grep -v "Using a password"
A_ID=$(mysql -uoj -poj123456 -N -e "USE oj; SELECT MAX(id) FROM submission;" 2>/dev/null)
echo "submission_id=$A_ID"

echo "--- 1. 手动制造卡死：status=judging + judge_started_at 30 分钟前 ---"
mysql -uoj -poj123456 -e "USE oj; UPDATE submission SET status='judging', judge_started_at=NOW() - INTERVAL 30 MINUTE WHERE id=$A_ID;" 2>&1 | grep -v "Using a password"
mysql -uoj -poj123456 -e "USE oj; SELECT id, status, retry_count FROM submission WHERE id=$A_ID;" 2>&1 | grep -v "Using a password"

echo "--- 2. 启动 worker（8 秒）---"
export JUDGE_MAIN=~/work/dati/sandbox/tests/smoke/build/judge_main
timeout 8 ./build/judge_worker 2>&1 | grep -v "MYSQL_OPT_RECONNECT"
echo "worker exit: $?"

echo "--- 3. DB 结果（应 accepted + retry_count=1）---"
mysql -uoj -poj123456 -e "USE oj; SELECT id, status, retry_count, stdout FROM submission WHERE id=$A_ID;" 2>&1 | grep -v "Using a password"

echo ""
echo "================ 场景 B：重试超限 → 死信（dead）================"
echo "--- 0. 插入提交 ---"
mysql -uoj -poj123456 -e "
USE oj;
INSERT INTO submission(code, language) VALUES (
'#include <cstdio>
int main(){puts(\"dead letter\");}',
'cpp');" 2>&1 | grep -v "Using a password"
B_ID=$(mysql -uoj -poj123456 -N -e "USE oj; SELECT MAX(id) FROM submission;" 2>/dev/null)
echo "submission_id=$B_ID"

echo "--- 1. 制造卡死 + retry_count=3（已达上限）---"
mysql -uoj -poj123456 -e "USE oj; UPDATE submission SET status='judging', retry_count=3, judge_started_at=NOW() - INTERVAL 30 MINUTE WHERE id=$B_ID;" 2>&1 | grep -v "Using a password"

echo "--- 2. 启动 worker（8 秒）---"
timeout 8 ./build/judge_worker 2>&1 | grep -v "MYSQL_OPT_RECONNECT"
echo "worker exit: $?"

echo "--- 3. DB 结果（应 dead + retry_count 保持 3，不再自动重判）---"
mysql -uoj -poj123456 -e "USE oj; SELECT id, status, retry_count FROM submission WHERE id=$B_ID;" 2>&1 | grep -v "Using a password"

echo ""
echo "================ 台阶 3 验证完成 ================"
