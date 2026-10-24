import json, subprocess, time, urllib.request, urllib.error

BASE = "http://localhost:8000"
USER = "e2es2_" + str(int(time.time()))

def call(method, path, body=None, headers=None):
    req = urllib.request.Request(
        BASE + path, method=method,
        data=json.dumps(body).encode() if body else None,
        headers={"Content-Type": "application/json", **(headers or {})},
    )
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read().decode()), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode()), dict(e.headers)


def hget(h, name):
    """urllib 的 headers 保留原始大小写（uvicorn 输出小写 retry-after），
    统一不敏感匹配——pytest 里 starlette Headers 天生不敏感所以没暴露。"""
    for k, v in h.items():
        if k.lower() == name.lower():
            return v
    return None

# 1. 注册 + 登录
s, b, _ = call("POST", "/api/register", {"username": USER, "password": "secret123"})
assert s == 201, (s, b)
s, b, _ = call("POST", "/api/login", {"username": USER, "password": "secret123"})
assert s == 200, (s, b)
token = b["token"]
H = {"Authorization": "Bearer " + token}

# 2. 未登录提交 → 401（写接口必须认证，台阶 2 显式决策）
s, b, _ = call("POST", "/api/submit", {"code": "int main(){}", "language": "cpp"})
assert s == 401, (s, b)

# 3. 带 token 提交 → 201
s, b, _ = call("POST", "/api/submit", {"code": "int main(){}", "language": "cpp"}, headers=H)
assert s == 201, (s, b)
sid = b["submission_id"]

# 4. 连发 25 次：桶耗尽后必须 429 + Retry-After。
#    注意：桶按 IP 共享（127.0.0.1），此前运行已消耗令牌，429 出现位置不定，
#    因此断言"出现过 201、出现过 429、429 带 Retry-After"而非精确计数。
codes = []
for _ in range(25):
    s, _, _ = call("POST", "/api/submit", {"code": "int main(){}", "language": "cpp"}, headers=H)
    codes.append(s)
assert 201 in codes, f"至少应有一次放行, got {codes}"
assert 429 in codes, f"超过 burst 必须 429, got {codes}"
s, b, h = call("POST", "/api/submit", {"code": "int main(){}", "language": "cpp"}, headers=H)
assert s == 429, (s, b)
assert hget(h, "Retry-After"), f"429 必须带 Retry-After 头, got {h}"

# 5. user_id 落库（排行榜聚合的数据基础）
out = subprocess.run(
    ["mysql", "-uoj", "-poj123456", "oj", "-N", "-e",
     "SELECT user_id FROM submission WHERE id=" + str(sid) + ";"],
    capture_output=True, text=True).stdout.strip()
assert out, f"user_id 必须非空落库, got {out!r}"

print("E2E S2 全部通过 ✅  401 未登录 / 201 带 token / 429+Retry-After 限流 / user_id 落库")
