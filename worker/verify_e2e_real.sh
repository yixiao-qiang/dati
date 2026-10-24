#!/bin/bash
# 台阶1 端到端真判题验证：judge_main 单点 + worker 全链路
set -e
cd ~/work/dati

echo "========== 1. judge_main 单点测试（静态 hello）=========="
cat > /tmp/hello.cpp <<'EOF'
#include <cstdio>
int main(){ printf("Hello, World!\n"); return 0; }
EOF
g++ -O2 -static -o /tmp/hello /tmp/hello.cpp
sudo ./sandbox/tests/smoke/build/judge_main /tmp/hello 1000 256 64 < /dev/null
echo ""

echo "========== 2. 插入真实代码提交 =========="
mysql -uoj -poj123456 -e "
USE oj;
INSERT INTO submission(code, language) VALUES (
'#include <cstdio>
int main(){puts(\"hi from sandbox\");}',
'cpp');
SELECT id, status FROM submission;
" 2>&1 | grep -v "Using a password"
NEW_ID=$(mysql -uoj -poj123456 -N -e "USE oj; SELECT MAX(id) FROM submission;" 2>/dev/null)
echo "submission_id=$NEW_ID"

echo ""
echo "========== 3. 重建流 + 建组 + XADD（先建组再入队，K1 纪律）=========="
redis-cli DEL judge:stream
redis-cli XGROUP CREATE judge:stream workers "$" MKSTREAM
redis-cli XADD judge:stream "*" submission_id "$NEW_ID"

echo ""
echo "========== 4. 启动 worker（12 秒，真判题）=========="
cd ~/work/dati/worker
export JUDGE_MAIN=~/work/dati/sandbox/tests/smoke/build/judge_main
timeout 12 ./build/judge_worker 2>&1 || true

echo ""
echo "========== 5. 验证 DB（应 accepted + stdout）=========="
mysql -uoj -poj123456 -e "USE oj; SELECT id, status, result, stdout FROM submission WHERE id=$NEW_ID;" 2>&1 | grep -v "Using a password"

echo "========== 6. Redis XPENDING（应 0）=========="
redis-cli XPENDING judge:stream workers
echo ""
echo "========== 7. 队列流长度 =========="
redis-cli XLEN judge:stream
