#!/bin/bash
# 启动 worker（S5 e2e 用）：先确认 judge_main 存在，再 setsid 拉起
# 用法：start_worker.sh [WORKER_ID]（默认 w1）；并发验证用 start_worker.sh w2
set -u
WID="${1:-w1}"
# 查找 judge_main（Wave 1 沙箱产物）
JM=$(find ~/work/dati/sandbox -name judge_main -type f 2>/dev/null | head -n 1)
if [ -z "$JM" ]; then
  echo "!! 找不到 judge_main，先构建 sandbox"; exit 1
fi
echo "JUDGE_MAIN=$JM WORKER_ID=$WID"
# 只杀同名 WID 的旧进程（并发验证：重启 w2 不能杀掉 w1）
pkill -f "WORKER_ID=$WID" 2>/dev/null; sleep 1
cd ~/work/dati/worker
sudo -n env JUDGE_MAIN="$JM" WORKER_ID="$WID" setsid nohup ./build/judge_worker >> /tmp/worker_$WID.log 2>&1 &
sleep 2
echo "--- worker 日志 ---"
tail -n 5 /tmp/worker_$WID.log
ps aux | grep judge_worker | grep -v grep
