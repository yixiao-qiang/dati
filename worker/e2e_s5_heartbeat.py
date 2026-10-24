import json, time, urllib.request, urllib.error, random
import redis

BASE = "http://localhost:8000"
REDIS = redis.Redis(decode_responses=True)

def call(method, path, body=None, headers=None):
    req = urllib.request.Request(
        BASE + path, method=method,
        data=json.dumps(body).encode() if body else None,
        headers={"Content-Type": "application/json", **(headers or {})},
    )
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode())

# ---- 1. 用户 + 长任务题 ----
u = f"hb_{random.randint(10000, 99999)}"
call("POST", "/api/register", {"username": u, "password": "pass1234"})
_, lg = call("POST", "/api/login", {"username": u, "password": "pass1234"})
AUTH = {"Authorization": f"Bearer {lg['token']}"}

s, b = call("POST", "/api/problems",
            {"title": "long-task", "time_limit_ms": 60000,
             "mem_limit_mb": 256, "output_limit_mb": 64}, AUTH)
assert s == 201, (s, b)
pid = b["problem_id"]
s, b = call("POST", f"/api/problems/{pid}/testcases",
            {"testcases": [{"input": "", "expected_output": "done\n", "score": 1}]}, AUTH)
assert s == 201, (s, b)

# ---- 2. 提交 sleep 55s 的程序（> 心跳周期 30s，判死阈值压缩 40s 可区分）----
# 有心跳：idle 在 t≈30s 被 reset_idle 归零，峰值 30s < 40s 阈值 → 无重投
# 无心跳：t=40s 时 idle=40s ≥ 阈值 → XAUTOCLAIM 重投 → 消息出现 2 条
code = '#include <unistd.h>\n#include <cstdio>\nint main(){sleep(55);printf("done\\n");return 0;}'
s, b = call("POST", "/api/submit", {"code": code, "language": "cpp", "problem_id": pid}, AUTH)
assert s == 201, (s, b)
sid = b["submission_id"]

# ---- 3. 轮询到终态（最多 90s：55s 判题 + 余量）----
deadline = time.time() + 90
status = None
while time.time() < deadline:
    _, row = call("GET", f"/api/submission/{sid}")
    status = row["status"]
    if status not in ("pending", "judging"):
        break
    time.sleep(2)
print(f"长任务终态: {status}（等待 {time.time() - deadline + 110:.0f}s）")

# ---- 4. 断言：AC 且未被重投 ----
assert status == "accepted", f"长任务应 accepted（心跳续命成功），got {status} err={row.get('error_msg')}"

# 判据 A 复核：该 submission 在 stream 中恰好 1 条消息（无 XAUTOCLAIM 重投）
msgs = [m for m in REDIS.xrange("judge:stream")
        if str(sid) == m[1].get("submission_id")]
assert len(msgs) == 1, f"长任务消息被重投：{len(msgs)} 条——心跳未生效！"
print(f"心跳判据 ✅ 长任务消息恰好 1 条（心跳每 30s reset_idle，idle 未过 40s 判死阈值）")

# XPENDING 无残留
pend = REDIS.xpending("judge:stream", "workers")
assert pend["pending"] == 0, f"pending 残留 {pend['pending']}"
print("XPENDING ✅ 无残留")

print("E2E S5 心跳续命全部通过 ✅  sleep(55) 长任务在压缩判死阈值(40s)下未被重投")
