import json, subprocess, time, urllib.request, urllib.error

BASE = "http://localhost:8000"
USERNAME = "e2e_" + str(int(time.time()))  # 随机后缀：脚本可重复运行

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

# 1. 注册 → 201
s, b = call("POST", "/api/register", {"username": USERNAME, "password": "secret123"})
assert s == 201, (s, b)

# 2. 重复注册 → 409
s, b = call("POST", "/api/register", {"username": USERNAME, "password": "secret123"})
assert s == 409, (s, b)

# 3. 登录 → 200 + token
s, b = call("POST", "/api/login", {"username": USERNAME, "password": "secret123"})
assert s == 200, (s, b)
token = b["token"]

# 4. Redis session 存在（redis-cli 管道模式：存在=值，不存在=空串）
out = subprocess.run(["redis-cli", "GET", "session:" + token],
                     capture_output=True, text=True).stdout.strip()
assert out, f"redis session 应为非空, got {out!r}"

# 5. 登出 → 200
s, b = call("POST", "/api/logout", headers={"Authorization": "Bearer " + token})
assert s == 200, (s, b)

# 6. Redis session 消失（key 已删 → GET 返回空串）
out = subprocess.run(["redis-cli", "GET", "session:" + token],
                     capture_output=True, text=True).stdout.strip()
assert out == "", f"logout 后 session 应消失, got {out!r}"

# 7. 错密码登录 → 401（同一 401 语义）
s, b = call("POST", "/api/login", {"username": USERNAME, "password": "wrong123"})
assert s == 401, (s, b)

print("E2E 全部通过 ✅  register 201 / dup 409 / login 200 / redis session / logout 200 / gone / wrong pw 401")
