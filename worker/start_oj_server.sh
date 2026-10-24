#!/bin/bash
# 启动/重启 OJ server（Wave 5 台阶 1 验证用）
# 自排除模式 [m]ain：pkill -f 会匹配到命令行含 'uvicorn main:app' 的 ssh 会话自身
pkill -f 'uvicorn [m]ain:app' 2>/dev/null
sleep 1
cd ~/work/dati/server || exit 1
sudo nohup python3 -m uvicorn main:app --host 0.0.0.0 --port 8000 > /tmp/uvicorn.log 2>&1 < /dev/null &
sleep 5
python3 - <<'PYEOF'
import socket
s = socket.socket()
r = s.connect_ex(("127.0.0.1", 8000))
print("port 8000:", "LISTENING" if r == 0 else "DOWN")
PYEOF
