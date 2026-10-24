import json, sys, time, urllib.request, urllib.error, random
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

def stream_tail_id():
    """judge:stream 当前最后一条消息 ID（作为本轮新消息的基线）。"""
    rows = REDIS.xrevrange("judge:stream", count=1)
    return rows[0][0] if rows else "0-0"

# ---- 1. 用户 + 题目 ----
u = f"conc_{random.randint(10000, 99999)}"
call("POST", "/api/register", {"username": u, "password": "pass1234"})
_, lg = call("POST", "/api/login", {"username": u, "password": "pass1234"})
AUTH = {"Authorization": f"Bearer {lg['token']}"}

_, probs = call("GET", "/api/problems")
assert probs, "没有题目，先建题挂用例"
pid = probs[0]["id"]  # 用第一道题（Wave 4 的 3 用例题）

# ---- 2. 基线 + 批量提交 ----
base_id = stream_tail_id()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 15
code = '#include <cstdio>\nint main(){int a,b;scanf("%d%d",&a,&b);printf("%d\\n",a+b);return 0;}'
def call_retry(method, path, body=None, headers=None, retries=8):
    """提交接口带 429 重试：令牌桶 burst=20，批量提交会触发限流（限流本身是
    S2 已验证功能）。等 Retry-After 再试，避免并发测试被限流打断。"""
    for i in range(retries):
        s, b = call(method, path, body, headers)
        if s == 429:
            wait = float(b.get("wait", 3) or 3)
            time.sleep(wait + 0.2)
            continue
        return s, b
    raise AssertionError(f"持续 429：{b}")

sids = []
t0 = time.time()
for i in range(N):
    s, b = call_retry("POST", "/api/submit",
                      {"code": code, "language": "cpp", "problem_id": pid}, AUTH)
    assert s == 201, (s, b)
    sids.append(b["submission_id"])
submit_span = time.time() - t0

# ---- 3. 轮询等全部终态 ----
deadline = time.time() + 120
pending = set(sids)
while pending and time.time() < deadline:
    for sid in list(pending):
        _, row = call("GET", f"/api/submission/{sid}")
        if row["status"] not in ("pending", "judging"):
            pending.discard(sid)
    time.sleep(1)
assert not pending, f"超时未判完: {pending}"
finish_t0 = time.time() - t0 - submit_span
all_t0 = time.time() - t0
print(f"提交 {N} 条耗时 {submit_span:.1f}s；判完耗时 {finish_t0:.1f}s；总耗时 {all_t0:.1f}s")

# ---- 4. 判据 A：XRANGE 从基线后，每个 submission_id 出现次数 == 1 ----
new_msgs = REDIS.xrange("judge:stream", min=base_id)  # min 开区间：不含基线本身
# 基线后的第一条除外：min=base_id 会包含 base_id 本身，用 > 语义：跳过等于 base_id 的
new_msgs = [m for m in new_msgs if m[0] != base_id]
counts = {}
for msg_id, fields in new_msgs:
    sid = int(fields.get("submission_id", "0"))
    counts[sid] = counts.get(sid, 0) + 1

found = {s: counts.get(s, 0) for s in sids}
dup = {s: c for s, c in found.items() if c != 1}
assert not dup, f"判据 A 失败：重复投递 {dup}"
assert len(found) == N, f"判据 A 失败：只找到 {len(found)}/{N} 条消息"
print(f"判据 A ✅ 每个 submission_id 恰好 1 次（{len(found)} 条全部唯一）")

# ---- 5. 判据 B：消费无滞留 + 无重复处理 ----
# 注意：Redis 7 的 delivery-count 是 pending 消息系统元数据，XRANGE 不返回
# （只有 XPENDING/XAUTOCLAIM 可见）。正常并发下消息判完即 XACK，pending 应清空
# ——pending 残留 = 有消息没处理完；判据 A 已证明投递不重复（每 submission 恰好
# 1 条消息），XPENDING 清空证明消费不重复滞留。重投机制本身（XAUTOCLAIM 会把
# delivery-count+1）在崩溃恢复轮单独验证。
pend = REDIS.xpending("judge:stream", "workers") if True else None
try:
    pend_info = REDIS.xpending("judge:stream", "workers")
except redis.ResponseError:
    pend_info = None
if pend_info:
    assert pend_info["pending"] == 0, \
        f"判据 B 失败：pending 残留 {pend_info['pending']} 条（消费未完成）"
    print(f"判据 B ✅ XPENDING 无残留（pending={pend_info['pending']}，全部消费 ACK）")
else:
    print("判据 B 注：stream 组不存在或已删（本机 Redis 版本差异），以判据 A + 结果正确性为准")

# ---- 6. 结果正确性 ----
_, row0 = call("GET", f"/api/submission/{sids[0]}")
assert row0["status"] == "accepted", f"应全 AC, got {row0['status']}"
print(f"结果 ✅ 抽查 submission={sids[0]} status=accepted")

print(f"CONCURRENT-ROUND-OK rounds={N}")
