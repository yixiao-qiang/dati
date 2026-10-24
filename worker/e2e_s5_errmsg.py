import json, time, urllib.request, urllib.error, urllib.parse

BASE = "http://localhost:8000"

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

# 1. 注册 + 登录（拿 token）
import random
u = f"errmsg_{random.randint(10000, 99999)}"
s, b = call("POST", "/api/register", {"username": u, "password": "pass1234"})
assert s == 201, (s, b)
s, b = call("POST", "/api/login", {"username": u, "password": "pass1234"})
assert s == 200, (s, b)
token = b["token"]
AUTH = {"Authorization": f"Bearer {token}"}

# 2. 建一道题，但【不挂测试用例】——判题时 fetch_testcases 空 → SystemError
s, b = call("POST", "/api/problems",
            {"title": "no-cases-problem", "time_limit_ms": 1000,
             "mem_limit_mb": 256, "output_limit_mb": 64}, AUTH)
assert s == 201, (s, b)
pid = b["problem_id"]

# 3. 提交（带 token + problem_id）
s, b = call("POST", "/api/submit",
            {"code": "#include <cstdio>\nint main(){printf(\"hi\\n\");return 0;}",
             "language": "cpp", "problem_id": pid}, AUTH)
assert s == 201, (s, b)
sid = b["submission_id"]

# 4. 轮询到终态（worker 判题：无用例 → SystemError + error_msg）
deadline = time.time() + 30
while time.time() < deadline:
    s, row = call("GET", f"/api/submission/{sid}")
    assert s == 200, (s, row)
    if row["status"] in ("accepted", "wrong_answer", "tle", "mle",
                         "runtime_error", "compile_error", "system_error", "dead"):
        break
    time.sleep(1)
else:
    raise AssertionError("轮询超时未到终态")

# 5. 断言：system_error + error_msg 全链路透传
assert row["status"] == "system_error", f"无用例应 system_error, got {row['status']}"
assert row["error_msg"] == "题目没有测试用例", f"error_msg 应=题目没有测试用例, got {row['error_msg']!r}"
assert row["passed_cases"] == 0 and row["total_cases"] == 0

print(f"E2E S5 error_msg 链路全部通过 ✅  submission={sid} status={row['status']} error_msg={row['error_msg']!r}")
