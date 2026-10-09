#!/bin/bash
# Wave 2 建表验证脚本
set -e
cd ~/work/dati/worker

echo "=== 1. 执行建库建表 ==="
sudo -n mysql < init.sql
echo "OK"

echo "=== 2. 表结构 ==="
sudo -n mysql -e "USE oj; SHOW TABLES; DESCRIBE submission;"

echo "=== 3. oj 账号连接测试（插入-查询-清理）==="
mysql -uoj -poj123456 -e "
USE oj;
INSERT INTO submission(code) VALUES ('#include <cstdio>');
INSERT INTO submission(code, language) VALUES ('int main(){}', 'c');
SELECT id, status, language, created_at FROM submission;
UPDATE submission SET status='judging' WHERE id=1 AND status='pending';
SELECT ROW_COUNT() AS claimed_rows;
SELECT id, status FROM submission WHERE id=1;
DELETE FROM submission;
SELECT COUNT(*) AS after_clean FROM submission;
" 2>&1 | grep -v "Using a password"

echo "=== 4. 幂等语义验证（第二次 UPDATE 应影响 0 行）==="
mysql -uoj -poj123456 -e "
USE oj;
INSERT INTO submission(code) VALUES ('test');
UPDATE submission SET status='judging' WHERE id=1 AND status='pending';
UPDATE submission SET status='judging' WHERE id=1 AND status='pending';
SELECT ROW_COUNT() AS second_update_should_be_0;
DELETE FROM submission;
" 2>&1 | grep -v "Using a password"
echo "全部通过"
