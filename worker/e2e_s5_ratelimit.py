import json, time, urllib.request, urllib.error, random

BASE = "http://localhost:8000"

def call(method, path, body=None, headers=None):
    req = urllib.request.Request(
        BASE + path, method=method,
        data=json.dumps(body).encode() if body else None,
        headers={"Content-Type": "application/json", **(headers or {})},
    )
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, dict(r.headers), json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), json.loads(e.read().decode())

# ---- 1. 注册限流：burst=3，第 4 次必 429 ----
print("== 注册限流（burst=3）==")
codes = []
for i in range(4):
    u = f"rl_{random.randint(100000, 999999)}"
    s, h, b = call("POST", "/api/register", {"username": u, "password": "pass1234"})
    codes.append(s)
    print(f"  #{i+1} -> {s}")
assert codes[:3] == [201, 201, 201], f"前 3 次应 201, got {codes}"
assert codes[3] == 429, f"第 4 次应 429（burst=3 耗尽）, got {codes[3]}"
# Retry-After 头（e2e 用小写匹配——uvicorn 输出小写，pytest 的 Headers 大小写不敏感
# 掩盖过这个差异，真实链路必须大小写不敏感查）
ra = h.get("Retry-After") or h.get("retry-after")
assert ra, f"429 必须带 Retry-After 头, got {h}"
print(f"  第 4 次 429 ✅ Retry-After={ra}")

# ---- 2. 登录限流：burst=5，第 6 次必 429（register 桶与 login 桶独立，不受上面影响）----
print("== 登录限流（burst=5，独立桶）==")
codes2 = []
for i in range(6):
    s, h, b = call("POST", "/api/login", {"username": "nobody", "password": "wrong123"})
    codes2.append(s)
print(f"  连续 6 次 -> {codes2}")
assert codes2[:5] == [401] * 5, f"前 5 次应 401（凭据错）, got {codes2[:5]}"
assert codes2[5] == 429, f"第 6 次应 429, got {codes2[5]}"
ra2 = h.get("Retry-After") or h.get("retry-after")
assert ra2, "login 429 也必须带 Retry-After"
print(f"  第 6 次 429 ✅ Retry-After={ra2}（前 5 次 401 = 凭据错语义未被限流污染）")

# ---- 3. 桶隔离：register 桶此刻已耗尽，但 submit/login 桶应不受影响 ----
# （register 3 次已花完，但 submit 桶是独立的——不验证 submit 了，验证 register
#   桶恢复需要等 60s，跳过；隔离已由单测 test_rate_limit_key_is_scoped 锁死）

print("E2E S5 P2 限流全部通过 ✅  注册 burst=3 / 登录 burst=5 / 独立桶 / Retry-After 头")
