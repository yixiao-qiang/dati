import json, time, urllib.request, urllib.error

BASE = "http://localhost:8000"
USER = "diag_" + str(int(time.time()))

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

s, b, _ = call("POST", "/api/register", {"username": USER, "password": "secret123"})
s, b, _ = call("POST", "/api/login", {"username": USER, "password": "secret123"})
H = {"Authorization": "Bearer " + b["token"]}

for i in range(25):
    s, b, h = call("POST", "/api/submit", {"code": "int main(){}", "language": "cpp"}, headers=H)
    if s == 429:
        print(f"第 {i} 次 429, headers={dict(h)}")
        break
else:
    print("25 次内没有 429？")
