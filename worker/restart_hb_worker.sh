#!/bin/bash
# 心跳验证环境：w1 带压缩判死参数，w2 停掉
set -u
pkill -f 'WORKER_ID=w2' 2>/dev/null; pkill -f 'WORKER_ID=w1' 2>/dev/null; sleep 1
JM=$(find ~/work/dati/sandbox -name judge_main -type f 2>/dev/null | head -n 1)
cd ~/work/dati/worker
# 判死阈值 40s、扫描 1s：心跳 30s 一跳，idle 峰值 30s < 40s → 无重投；
# 若无心跳则 t=40s 必重投（sleep 55s 任务 t=40s 时仍在跑）——可严格区分
sudo -n env JUDGE_MAIN="$JM" WORKER_ID=w1 OJ_CLAIM_IDLE_MS=40000 OJ_CLAIM_SCAN_MS=1000 \
  setsid nohup ./build/judge_worker >> /tmp/worker_hb.log 2>&1 &
sleep 2
tail -n 3 /tmp/worker_hb.log
ps aux | grep judge_worker | grep -v grep
