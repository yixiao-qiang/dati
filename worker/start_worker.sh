#!/bin/bash
# 启动 worker（S5 e2e 用）：先确认 judge_main 存在，再 setsid 拉起
set -u
# 查找 judge_main（Wave 1 沙箱产物）
JM=$(find ~/work/dati/sandbox -name judge_main -type f 2>/dev/null | head -n 1)
if [ -z "$JM" ]; then
  echo "!! 找不到 judge_main，先构建 sandbox"; exit 1
fi
echo "JUDGE_MAIN=$JM"
pkill -f 'judge_worker' 2>/dev/null; sleep 1
cd ~/work/dati/worker
sudo -n env JUDGE_MAIN="$JM" WORKER_ID=w1 setsid nohup ./build/judge_worker >> /tmp/worker.log 2>&1 &
sleep 2
echo "--- worker 日志 ---"
tail -n 5 /tmp/worker.log
ps aux | grep judge_worker | grep -v grep
