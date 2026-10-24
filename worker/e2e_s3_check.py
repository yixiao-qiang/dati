import json, urllib.request, urllib.error

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

# 1. 无 token 也能查排行榜（查询开放）
s, b = call("GET", "/api/rank")
assert s == 200, (s, b)

# 2. 口径验证：rankA 2/40 排第一，rankB 1/20 排第二（rank_test.sql 造的确定性数据）
rank = {r["username"]: r for r in b}
assert rank["rankA"]["ac_count"] == 2, f"rankA 应 AC=2, got {rank['rankA']}"
assert rank["rankA"]["penalty_minutes"] == 40, f"rankA 罚时应 40, got {rank['rankA']}"
assert rank["rankB"]["ac_count"] == 1, f"rankB 应 AC=1, got {rank['rankB']}"
assert rank["rankB"]["penalty_minutes"] == 20, f"rankB 罚时应 20, got {rank['rankB']}"
assert rank["rankC"]["ac_count"] == 0, f"rankC 应 AC=0, got {rank['rankC']}"

# 3. 排序：AC 降序，前两名必须是 rankA、rankB
assert b[0]["username"] == "rankA" and b[1]["username"] == "rankB", \
    f"前两名顺序错, got {[r['username'] for r in b[:2]]}"

# 4. 老提交（user_id NULL 的 accepted）不能给任何用户加 AC
total_ac = sum(r["ac_count"] for r in b)
assert total_ac == 3, f"老提交不应进榜, 总 AC 应=3, got {total_ac}"

print("E2E S3 全部通过 ✅  无 token 可查 / rankA 2,40 第一 / rankB 1,20 第二 / 老提交不进榜")
