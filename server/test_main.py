"""Wave 3 台阶 1 · server 提交 API 单元测试（本机可跑，无需 MySQL）

策略：mock 掉 pymysql.connect（不碰真库），验证三件事——
1. 事务边界：两条 INSERT（submission + outbox）同事务，COMMIT 只在两写之后；
   任一条失败必须 ROLLBACK（不留半成品）。
2. 接口校验：>64KB / 语言白名单 → 400；DB 故障 → 500。
3. 返回契约：正常提交 → 201 + {"submission_id": N}。
"""
import pymysql
import pytest
from fastapi import HTTPException
from fastapi.testclient import TestClient

import main
from main import MAX_CODE_BYTES, SubmitRequest, create_submission

# K6 契约测试用：DB 状态机的全部终态。
# ★ 权威来源 = worker/src/db.cpp 的 judge_status_to_string switch——直接解析
#   源码生成，不做手工副本（手工副本本身就是 K6 要消灭的那类漂移）。
# 前端 TERMINAL/STATUS_TEXT 必须与它对齐。
def _db_statuses_from_db_cpp() -> dict:
    import re
    from pathlib import Path

    src = (Path(__file__).resolve().parent.parent
           / "worker" / "src" / "db.cpp").read_text(encoding="utf-8")
    m = re.search(
        r"const char\* judge_status_to_string\(JudgeStatus s\) \{(.*?)\n\}",
        src, re.S)
    assert m, "db.cpp 里找不到 judge_status_to_string（被重构？同步本函数）"
    pairs = re.findall(r'case JudgeStatus::(\w+):\s*return "([a-z_]+)"', m.group(1))
    assert pairs, "switch 里一个 case 都没解析到——正则与代码风格漂移了"
    mapping = dict(pairs)
    assert len(mapping) == len(pairs), "枚举值到字符串必须一一对应（出现重复）"
    return mapping


_DB_STATUS_MAP = _db_statuses_from_db_cpp()
# 终态 = 全部状态 - 两个非终态（pending/judging 是状态机的中间态）
DB_TERMINAL_STATUSES = set(_DB_STATUS_MAP.values()) - {"pending", "judging"}


class FakeCursor:
    """记录 execute 的 SQL，可在 outbox INSERT 时注入失败（模拟第二条写崩）。

    results 队列供 fetchone/fetchall 依次出队（模拟 SELECT 结果），
    不预填时 fetchone 返回 None（模拟"查无此行"）。
    """

    def __init__(self, fail_on_outbox: bool = False):
        self.executed: list = []
        self.lastrowid = 42  # 模拟 AUTO_INCREMENT 返回
        self.fail_on_outbox = fail_on_outbox
        self.results: list = []

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def execute(self, sql: str, args=None):
        self.executed.append((sql, args))
        if self.fail_on_outbox and sql.startswith("INSERT INTO outbox"):
            raise pymysql.OperationalError("simulated outbox insert failure")

    def fetchone(self):
        return self.results.pop(0) if self.results else None

    def fetchall(self):
        out, self.results = self.results, []
        return out


class FakeConn:
    def __init__(self, fail_on_outbox: bool = False):
        self.cursor_ = FakeCursor(fail_on_outbox=fail_on_outbox)
        self.committed = False
        self.rolled_back = False
        self.closed = False

    def cursor(self):
        return self.cursor_

    def commit(self):
        self.committed = True

    def rollback(self):
        self.rolled_back = True

    def close(self):
        self.closed = True


@pytest.fixture
def fake_conn(monkeypatch):
    """把 main.pymysql.connect 替换成 FakeConn，测试后自动还原。"""
    conn = FakeConn()
    monkeypatch.setattr(main.pymysql, "connect", lambda **kw: conn)
    return conn


@pytest.fixture
def client():
    return TestClient(main.app)


# ---------- 事务边界 ----------

def test_create_submission_writes_both_tables_in_one_transaction(fake_conn):
    sid = create_submission("int main(){}", "cpp")

    assert sid == 42  # 返回 lastrowid
    sqls = [sql for sql, _ in fake_conn.cursor_.executed]
    assert len(sqls) == 2, "必须且只写两条 INSERT"
    assert sqls[0].startswith("INSERT INTO submission"), "第一条是 submission"
    assert sqls[1].startswith("INSERT INTO outbox"), "第二条是 outbox"
    assert "status" in sqls[0] and "'pending'" in sqls[0], "提交初始状态 pending"
    assert "problem_id" in sqls[0], "INSERT 带 problem_id 列"
    assert "user_id" in sqls[0], "INSERT 带 user_id 列（台阶 2 认证后落库）"
    sub_args = fake_conn.cursor_.executed[0][1]
    assert sub_args[0] == 1, "默认 problem_id=1（限额从 problem 表读）"
    assert sub_args[1] == "int main(){}" and sub_args[2] == "cpp", "code/language 顺序正确"
    assert sub_args[3] is None, "默认无认证信息时 user_id 落 NULL（老提交语义）"
    outbox_args = fake_conn.cursor_.executed[1][1]
    assert outbox_args == (42,), "outbox 关联刚插入的 submission_id"
    assert "judged" in sqls[1] and "0" in sqls[1], "event=judged, delivered=0"
    assert fake_conn.committed and not fake_conn.rolled_back, "成功路径只 COMMIT 不 ROLLBACK"
    assert fake_conn.closed, "连接必须关闭"


def test_create_submission_rolls_back_when_outbox_insert_fails(monkeypatch):
    conn = FakeConn(fail_on_outbox=True)
    monkeypatch.setattr(main.pymysql, "connect", lambda **kw: conn)

    with pytest.raises(pymysql.OperationalError):
        create_submission("int main(){}", "cpp")

    assert conn.rolled_back, "第二条 INSERT 失败必须整批 ROLLBACK"
    assert not conn.committed, "失败路径绝不能 COMMIT（否则留下只有 submission 的半成品）"
    assert conn.closed


# ---------- 接口校验 ----------

def test_submit_returns_201_with_submission_id(client, monkeypatch, auth_ctx):
    monkeypatch.setattr(main, "problem_exists", lambda pid: True)
    monkeypatch.setattr(main, "create_submission",
                        lambda code, lang, pid, uid: 42)

    resp = client.post("/api/submit", json={"code": "int main(){}", "language": "cpp"},
                       headers=auth_ctx)

    assert resp.status_code == 201
    assert resp.json() == {"submission_id": 42}


def test_submit_404_when_problem_missing(client, monkeypatch, auth_ctx):
    """题目不存在必须入口处 404——不能等 worker 判题时落 system_error。"""
    monkeypatch.setattr(main, "problem_exists", lambda pid: False)
    monkeypatch.setattr(main, "create_submission",
                        lambda code, lang, pid, uid: (_ for _ in ()).throw(AssertionError(
                            "题目不存在时绝不能写 submission")))

    resp = client.post("/api/submit",
                       json={"code": "int main(){}", "language": "cpp", "problem_id": 99},
                       headers=auth_ctx)

    assert resp.status_code == 404
    assert "99" in resp.json()["detail"]


def test_submit_422_when_problem_id_not_positive(client, auth_ctx):
    """problem_id ge=1：0/负数是参数错，pydantic 422 拒绝。"""
    resp = client.post("/api/submit",
                       json={"code": "int main(){}", "language": "cpp", "problem_id": 0},
                       headers=auth_ctx)
    assert resp.status_code == 422


def test_submit_rejects_code_over_64kb(client, auth_ctx):
    resp = client.post(
        "/api/submit",
        json={"code": "x" * (MAX_CODE_BYTES + 1), "language": "cpp"},
        headers=auth_ctx,
    )
    assert resp.status_code == 400


def test_submit_rejects_unsupported_language(client, auth_ctx):
    resp = client.post("/api/submit", json={"code": "int main(){}", "language": "java"},
                       headers=auth_ctx)
    assert resp.status_code == 400


def test_submit_returns_500_when_db_unavailable(client, monkeypatch, auth_ctx):
    def boom(code, lang, pid, uid):
        raise pymysql.OperationalError("db is down")

    monkeypatch.setattr(main, "create_submission", boom)

    resp = client.post("/api/submit", json={"code": "int main(){}", "language": "cpp"},
                       headers=auth_ctx)
    assert resp.status_code == 500
    assert "数据库" in resp.json()["detail"], "500 要给用户可行动的信息，不是裸错误"


# ---------- 查询接口 ----------

FULL_ROW = {
    "id": 28,
    "language": "cpp",
    "status": "accepted",
    "result": "accepted",
    "stdout": "hi\n",
    "stderr": "",
    "passed_cases": 3,
    "total_cases": 3,
    "created_at": None,
}


def test_get_submission_returns_200_with_full_fields(client, monkeypatch):
    monkeypatch.setattr(main, "get_submission", lambda sid: FULL_ROW)

    resp = client.get("/api/submission/28")

    assert resp.status_code == 200
    body = resp.json()
    assert body["submission_id"] == 28
    assert body["status"] == "accepted"
    assert body["result"] == "accepted"
    assert body["stdout"] == "hi\n"
    assert body["stderr"] == ""
    assert body["passed_cases"] == 3 and body["total_cases"] == 3
    assert "language" in body and "created_at" in body


def test_get_submission_404_when_not_exists(client, monkeypatch):
    monkeypatch.setattr(main, "get_submission", lambda sid: None)

    resp = client.get("/api/submission/999")
    assert resp.status_code == 404, "id 不存在必须 404——前端凭据错的信号"


def test_get_submission_200_while_pending(client, monkeypatch):
    """在途提交（pending）返回 200 而不是 404——区分"不存在"和"没判完"。"""
    pending_row = dict(FULL_ROW, status="pending", result=None, stdout=None)
    monkeypatch.setattr(main, "get_submission", lambda sid: pending_row)

    resp = client.get("/api/submission/28")
    assert resp.status_code == 200
    assert resp.json()["status"] == "pending"


def test_get_submission_500_when_db_unavailable(client, monkeypatch):
    def boom(sid):
        raise pymysql.OperationalError("db is down")

    monkeypatch.setattr(main, "get_submission", boom)

    resp = client.get("/api/submission/28")
    assert resp.status_code == 500


def test_get_submission_rejects_non_numeric_id(client):
    resp = client.get("/api/submission/abc")
    assert resp.status_code == 422, "sid 声明为 int，pydantic 自动拒绝非数字（免费的类型校验）"


# ---------- Wave 4 台阶 1：题目管理 API ----------

def test_create_problem_endpoint_201(client, monkeypatch):
    monkeypatch.setattr(main, "create_problem", lambda *a: 7)
    resp = client.post("/api/problems", json={"title": "A+B"})
    assert resp.status_code == 201
    assert resp.json() == {"problem_id": 7}


def test_create_problem_endpoint_400_invalid_title(client):
    resp = client.post("/api/problems", json={"title": "   "})
    assert resp.status_code == 400, "空标题是参数错，400 而非 201"


def test_create_problem_endpoint_400_invalid_limits(client):
    resp = client.post("/api/problems", json={"title": "A", "time_limit_ms": 99999})
    assert resp.status_code == 400
    assert "time_limit_ms" in resp.json()["detail"], "错误信息要指出哪个字段越界"


def test_list_problems_endpoint_200(client, monkeypatch):
    monkeypatch.setattr(main, "list_problems",
                        lambda: [{"id": 1, "title": "A+B", "time_limit_ms": 1000,
                                  "mem_limit_mb": 256, "output_limit_mb": 64}])
    resp = client.get("/api/problems")
    assert resp.status_code == 200
    assert resp.json()[0]["title"] == "A+B"


def test_problem_detail_200_with_testcases(client, monkeypatch):
    detail = {
        "id": 1, "title": "A+B", "time_limit_ms": 1000,
        "mem_limit_mb": 256, "output_limit_mb": 64,
        "testcases": [{"id": 1, "ordinal": 0, "input": "1 2", "score": 1}],
    }
    monkeypatch.setattr(main, "get_problem_with_testcases", lambda pid: detail)
    resp = client.get("/api/problems/1")
    assert resp.status_code == 200
    assert len(resp.json()["testcases"]) == 1, "详情必须带用例（判题逐用例跑的输入来源）"
    assert "expected_output" not in resp.json()["testcases"][0], \
        "详情不得下发标准答案（防泄题）——判题侧 worker 直读 DB"


def test_get_problem_with_testcases_sql_excludes_expected_output(fake_conn):
    """SQL 层面就不查答案列：结构上杜绝泄露，而不是靠返回后过滤。"""
    fake_conn.cursor_.results = [
        {"id": 1, "title": "A+B", "time_limit_ms": 1000,
         "mem_limit_mb": 256, "output_limit_mb": 64},
        [{"id": 5, "ordinal": 0, "input": "1 2", "score": 1}],
    ]

    row = main.get_problem_with_testcases(1)

    tc_sql = [sql for sql, _ in fake_conn.cursor_.executed
              if "FROM testcase" in sql][0]
    assert "expected_output" not in tc_sql, "testcase 的 SELECT 不得包含答案列"
    assert "expected_output" not in row["testcases"][0]


def test_problem_detail_404_when_not_exists(client, monkeypatch):
    monkeypatch.setattr(main, "get_problem_with_testcases", lambda pid: None)
    resp = client.get("/api/problems/999")
    assert resp.status_code == 404


def test_add_testcases_201(client, monkeypatch):
    monkeypatch.setattr(main, "add_testcases", lambda pid, tcs: 2)
    resp = client.post(
        "/api/problems/1/testcases",
        json={"testcases": [{"input": "1 2", "expected_output": "3"},
                            {"input": "2 3", "expected_output": "5"}]},
    )
    assert resp.status_code == 201
    assert resp.json() == {"problem_id": 1, "count": 2}, "count = 插入条数"


def test_add_testcases_404_when_problem_missing(client, monkeypatch):
    monkeypatch.setattr(main, "add_testcases", lambda pid, tcs: None)
    resp = client.post("/api/problems/999/testcases",
                       json={"testcases": [{"input": "1", "expected_output": "1"}]})
    assert resp.status_code == 404, "题目不存在挂用例必须 404——防止孤儿用例静默写入"


def test_add_testcases_400_empty_list(client):
    resp = client.post("/api/problems/1/testcases", json={"testcases": []})
    assert resp.status_code == 400, "空用例数组没有业务意义，400 拒绝"


def test_add_testcases_400_oversize_input(client):
    """单条用例超 256KB 必须 400——DB 列是 MEDIUMTEXT，不设限会被撑爆。"""
    big = "x" * (main.MAX_CASE_BYTES + 1)
    resp = client.post(
        "/api/problems/1/testcases",
        json={"testcases": [{"input": big, "expected_output": "1"}]},
    )
    assert resp.status_code == 400
    assert "256KB" in resp.json()["detail"]


def test_add_testcases_400_too_many(client):
    resp = client.post(
        "/api/problems/1/testcases",
        json={"testcases": [{"input": "1", "expected_output": "1"}]
              * (main.MAX_TESTCASES + 1)},
    )
    assert resp.status_code == 400
    assert "100" in resp.json()["detail"]


def test_add_testcases_400_bad_score(client):
    resp = client.post(
        "/api/problems/1/testcases",
        json={"testcases": [{"input": "1", "expected_output": "1", "score": 0}]},
    )
    assert resp.status_code == 400
    assert "score" in resp.json()["detail"]


# ================= Wave 4 收尾 · K6 词汇表契约测试 =================

def test_frontend_terminal_set_covers_all_db_terminal_statuses():
    """前端 TERMINAL 集合必须 ⊇ DB 状态机全部终态（K6 契约）。

    起因（Wave 4 收尾 P0）：前端曾用短码 wa/re/ce/se，而 API 返回全名
    wrong_answer/...——4 个终态永远匹配不上 → 轮询永不停止。状态词汇表
    横跨四层手工同步（DB 枚举 → judge_status_to_string → API JSON →
    前端 TERMINAL），本测试把"前端文件内容"与"DB 终态集合"锁在一起，
    任何一层漂移（含前端改回短码、新增状态忘同步）立刻红。
    """
    import re
    from pathlib import Path

    index_html = (Path(__file__).parent / "static" / "index.html").read_text(
        encoding="utf-8")

    m = re.search(r"const TERMINAL = new Set\(\[(.*?)\]\)", index_html, re.S)
    assert m, "index.html 里找不到 TERMINAL 集合（被改名/重构？同步本测试）"
    frontend_terminal = {
        s.strip().strip("'\"") for s in m.group(1).split(",") if s.strip()
    }

    missing = DB_TERMINAL_STATUSES - frontend_terminal
    assert not missing, (
        f"前端 TERMINAL 缺少终态 {sorted(missing)}——这些状态会无限轮询。"
        "词汇表以 worker/src/db.cpp judge_status_to_string 为准（本文件顶部"
        "直接解析源码生成，无手工副本），四层必须同步。"
    )


def test_frontend_status_text_covers_all_statuses():
    """STATUS_TEXT 必须覆盖全部状态（终态 + pending/judging），否则渲染 undefined。

    与 TERMINAL 同源的词汇表漂移风险：STATUS_TEXT 缺键时前端显示
    undefined，用户看不出是哪种终态——同一类契约，一并锁死。
    """
    import re
    from pathlib import Path

    index_html = (Path(__file__).parent / "static" / "index.html").read_text(
        encoding="utf-8")

    m = re.search(r"const STATUS_TEXT = \{(.*?)\};", index_html, re.S)
    assert m, "index.html 里找不到 STATUS_TEXT 映射（被改名/重构？同步本测试）"
    status_text_keys = set(re.findall(r"'([a-z_]+)'\s*:", m.group(1)))

    all_statuses = DB_TERMINAL_STATUSES | {"pending", "judging"}
    missing = all_statuses - status_text_keys
    assert not missing, (
        f"前端 STATUS_TEXT 缺少状态 {sorted(missing)}——这些状态会渲染成 undefined。"
    )


# ================= Wave 5 台阶 1 · 认证（users / scrypt / Redis session） =================

class FakeRedis:
    """mock main.REDIS：记录 setex/delete/get 调用，不碰真 Redis。

    与 FakeConn 同理：测试只验证 server 对 Redis 的"行为契约"
    （key 格式 / TTL / 删除幂等），不验证 Redis 本身。
    """

    def __init__(self):
        self.store = {}
        self.calls = []
        self.eval_result = [1, 0]   # Lua 默认返回"放行"；限流测试改为 [0, wait]

    def setex(self, name, time, value):
        self.calls.append(("setex", name, time, value))
        self.store[name] = value

    def delete(self, name):
        self.calls.append(("delete", name))
        self.store.pop(name, None)

    def get(self, name):
        return self.store.get(name)

    def eval(self, script, numkeys, *args):
        self.calls.append(("eval", numkeys) + args)
        return self.eval_result


@pytest.fixture
def fake_redis():
    return FakeRedis()


@pytest.fixture
def auth_ctx(monkeypatch, fake_redis):
    """已登录上下文：Redis 里有 session:tok→7，返回可直接传 headers 的字典。

    为什么不用 monkeypatch require_auth 本身：FastAPI 注册路由时已捕获依赖
    函数引用，monkeypatch 模块属性对已注册路由不生效——必须 mock 底层 REDIS。
    """
    fake_redis.store["session:tok"] = "7"
    monkeypatch.setattr(main, "REDIS", fake_redis)
    return {"Authorization": "Bearer tok"}


# 供 login 测试用的"已注册用户"：模块导入时真跑一次 scrypt（~70ms，可接受）
USER_ROW = {"id": 7, "username": "alice",
            "password_hash": main.hash_password("secret123")}


# ---------- 密码哈希（纯函数，不碰库） ----------

def test_hash_password_self_describing_format():
    """scrypt 格式必须自包含：重建哈希所需参数（N/R/P/盐）全在串里。

    为什么是 6 段：scrypt$N$R$P$<salt_b64>$<hash_b64>——没有第 6 段哈希，
    verify 就无从比对；没有前 4 段参数，verify 就无从重算。
    """
    h = main.hash_password("secret123")
    parts = h.split("$")
    assert len(parts) == 6, "scrypt$N$R$P$salt$hash 共 6 段"
    assert parts[0] == "scrypt"
    assert parts[1] == str(main.SCRYPT_N)
    assert parts[2] == str(main.SCRYPT_R)
    assert parts[3] == str(main.SCRYPT_P)
    # 盐和哈希是合法 base64
    import base64
    assert base64.b64decode(parts[4]) and base64.b64decode(parts[5])


def test_hash_password_salt_randomizes_output():
    h1 = main.hash_password("secret123")
    h2 = main.hash_password("secret123")
    assert h1 != h2, "盐随机：同密码两次哈希必须不同（否则相同密码可被识别）"
    assert main.verify_password("secret123", h1)
    assert main.verify_password("secret123", h2)


def test_verify_password_rejects_wrong_password():
    h = main.hash_password("secret123")
    assert not main.verify_password("wrong456", h), "错密码必须 False"


def test_verify_password_rejects_garbage():
    """脏数据（格式非法）必须返回 False 而不是抛异常——login 无需区分
    "密码错"和"库被写坏"，两种都按 401 处理。"""
    assert not main.verify_password("secret123", "not-a-hash")
    assert not main.verify_password("secret123", "")
    assert not main.verify_password("secret123", "md5$abc")
    assert not main.verify_password("secret123", "scrypt$16384$8$1$!!bad$!!bad")


# ---------- 注册 ----------

def test_register_201(client, monkeypatch):
    monkeypatch.setattr(main, "create_user", lambda u, h: 1)
    resp = client.post("/api/register",
                       json={"username": "alice", "password": "secret123"})
    assert resp.status_code == 201
    assert resp.json() == {"username": "alice"}


def test_register_409_duplicate_username(client, monkeypatch):
    """重复用户名是资源冲突（409），不是请求格式错（400）——语义分层。"""
    def dup(u, h):
        raise pymysql.IntegrityError("Duplicate entry 'alice' for key 'username'")

    monkeypatch.setattr(main, "create_user", dup)
    resp = client.post("/api/register",
                       json={"username": "alice", "password": "secret123"})
    assert resp.status_code == 409
    assert "alice" in resp.json()["detail"]


def test_register_400_invalid_username(client):
    """用户名白名单 ^[a-zA-Z0-9_]{3,20}$：太短/非法字符/超长全拒。"""
    for bad in ["ab", "a b!", "中文名", "toolongtoolongtoolongname1"]:
        resp = client.post("/api/register",
                           json={"username": bad, "password": "secret123"})
        assert resp.status_code == 400, f"用户名 {bad!r} 应被 400 拒绝"


def test_register_400_short_password(client):
    resp = client.post("/api/register",
                       json={"username": "alice", "password": "1234567"})
    assert resp.status_code == 400, "密码 <8 位是参数错，400"


def test_register_500_when_db_unavailable(client, monkeypatch):
    def boom(u, h):
        raise pymysql.OperationalError("db is down")

    monkeypatch.setattr(main, "create_user", boom)
    resp = client.post("/api/register",
                       json={"username": "alice", "password": "secret123"})
    assert resp.status_code == 500


# ---------- 登录 ----------

def test_login_200_with_token(client, monkeypatch, fake_redis):
    monkeypatch.setattr(main, "get_user_by_name", lambda name: USER_ROW)
    monkeypatch.setattr(main, "REDIS", fake_redis)

    resp = client.post("/api/login",
                       json={"username": "alice", "password": "secret123"})

    assert resp.status_code == 200
    body = resp.json()
    assert body["username"] == "alice"
    assert body["token"], "登录必须返回 token"
    # Redis 写入契约：key=session:{token}、TTL=24h、value=user_id 字符串
    assert ("setex", "session:" + body["token"], main.SESSION_TTL, "7") \
        in fake_redis.calls


def test_login_401_wrong_password(client, monkeypatch):
    monkeypatch.setattr(main, "get_user_by_name", lambda name: USER_ROW)
    resp = client.post("/api/login",
                       json={"username": "alice", "password": "wrong456"})
    assert resp.status_code == 401


def test_login_401_same_response_for_unknown_user_and_wrong_password(client, monkeypatch):
    """用户不存在 / 密码错，两条 401 响应必须逐字节一致——防用户枚举。

    若两者 detail 不同或状态码不同，攻击者注册一个用户名再请求登录，
    靠响应差异即可探测"该用户名是否存在"。"""
    monkeypatch.setattr(main, "get_user_by_name", lambda name: None)
    r_unknown = client.post("/api/login",
                            json={"username": "nobody", "password": "secret123"})
    monkeypatch.setattr(main, "get_user_by_name", lambda name: USER_ROW)
    r_wrong = client.post("/api/login",
                          json={"username": "alice", "password": "wrong456"})
    assert r_unknown.status_code == r_wrong.status_code == 401
    assert r_unknown.json() == r_wrong.json(), \
        "两条 401 响应必须一致，否则泄露用户是否存在"


def test_login_400_invalid_format(client):
    """登录也做格式校验（与注册同规格）——格式错是参数错，400。"""
    resp = client.post("/api/login",
                       json={"username": "ab", "password": "secret123"})
    assert resp.status_code == 400


# ---------- 登出 ----------

def test_logout_200_deletes_key(client, monkeypatch, fake_redis):
    fake_redis.store["session:abc"] = "7"
    monkeypatch.setattr(main, "REDIS", fake_redis)

    resp = client.post("/api/logout",
                       headers={"Authorization": "Bearer abc"})

    assert resp.status_code == 200
    assert ("delete", "session:abc") in fake_redis.calls
    assert "session:abc" not in fake_redis.store, "session key 必须被删掉"


def test_logout_200_idempotent_missing_key(client, monkeypatch, fake_redis):
    """token 不存在/已过期：DEL 0 行也应 200——用户登出两次不该报错。"""
    monkeypatch.setattr(main, "REDIS", fake_redis)
    resp = client.post("/api/logout",
                       headers={"Authorization": "Bearer ghost"})
    assert resp.status_code == 200
    assert ("delete", "session:ghost") in fake_redis.calls


def test_logout_401_missing_header(client):
    resp = client.post("/api/logout")
    assert resp.status_code == 401, "无 Authorization 头 = 未登录，401"


# ================= Wave 5 台阶 2 · 认证保护（submit）+ 令牌桶限流 =================

def test_require_auth_returns_user_context():
    """依赖函数可直接单测（不经过 HTTP）：头合法 + session 存在 → 用户上下文。"""
    fake = FakeRedis()
    fake.store["session:tok"] = "7"
    old = main.REDIS
    main.REDIS = fake
    try:
        ctx = main.require_auth("Bearer tok")
    finally:
        main.REDIS = old
    assert ctx == {"user_id": 7, "token": "tok"}


def test_require_auth_rejects_missing_header():
    with pytest.raises(HTTPException) as e:
        main.require_auth(None)
    assert e.value.status_code == 401


def test_require_auth_rejects_bad_scheme():
    with pytest.raises(HTTPException):
        main.require_auth("Basic abc123")


def test_require_auth_rejects_unknown_token(monkeypatch):
    """session 不存在的 token = 登录已过期，401。"""
    monkeypatch.setattr(main, "REDIS", FakeRedis())
    with pytest.raises(HTTPException):
        main.require_auth("Bearer ghost")


def test_submit_401_without_token(client):
    """写接口必须认证：无 Authorization 头 → 401（台阶 2 显式决策）。"""
    resp = client.post("/api/submit",
                       json={"code": "int main(){}", "language": "cpp"})
    assert resp.status_code == 401


def test_submit_401_with_invalid_token(client, monkeypatch):
    monkeypatch.setattr(main, "REDIS", FakeRedis())  # Redis 里没有 session
    resp = client.post("/api/submit",
                       json={"code": "int main(){}", "language": "cpp"},
                       headers={"Authorization": "Bearer ghost"})
    assert resp.status_code == 401


def test_submit_429_when_rate_limited(client, auth_ctx, fake_redis):
    """令牌桶耗尽 → 429 + Retry-After 头（HTTP 标准：告诉客户端等几秒）。"""
    fake_redis.eval_result = [0, 5]  # Lua 返回"限流，等 5 秒"
    resp = client.post("/api/submit",
                       json={"code": "int main(){}", "language": "cpp"},
                       headers=auth_ctx)
    assert resp.status_code == 429
    assert resp.headers.get("Retry-After") == "5", "Retry-After 必须带秒数"
    assert "5 秒" in resp.json()["detail"]


def test_check_rate_limit_allows():
    fake = FakeRedis()
    fake.eval_result = [1, 0]
    old = main.REDIS
    main.REDIS = fake
    try:
        assert main.check_rate_limit("1.2.3.4") is None, "放行返回 None"
    finally:
        main.REDIS = old
    assert fake.calls and fake.calls[0][0] == "eval", "限流必须走 Redis Lua（原子性）"


def test_check_rate_limit_rejects_with_wait_seconds():
    fake = FakeRedis()
    fake.eval_result = [0, 5]
    old = main.REDIS
    main.REDIS = fake
    try:
        assert main.check_rate_limit("1.2.3.4") == 5, "限流返回等待秒数"
    finally:
        main.REDIS = old


def test_create_submission_with_user_id(fake_conn):
    """认证后的提交必须带 user_id 落库（排行榜按用户聚合的数据基础）。"""
    main.create_submission("int main(){}", "cpp", user_id=7)
    args = fake_conn.cursor_.executed[0][1]
    assert args[3] == 7, "user_id 必须写入 submission INSERT"


def test_submit_passes_user_id_to_create_submission(client, monkeypatch, auth_ctx):
    """端到端：认证上下文里的 user_id 必须透传到 create_submission。"""
    captured = {}
    monkeypatch.setattr(main, "problem_exists", lambda pid: True)

    def spy(code, lang, pid, uid):
        captured["uid"] = uid
        return 42

    monkeypatch.setattr(main, "create_submission", spy)
    resp = client.post("/api/submit",
                       json={"code": "int main(){}", "language": "cpp"},
                       headers=auth_ctx)
    assert resp.status_code == 201
    assert captured["uid"] == 7, "认证上下文 user_id=7 必须透传到落库"
