"""Wave 3 台阶 1 · server 提交 API 单元测试（本机可跑，无需 MySQL）

策略：mock 掉 pymysql.connect（不碰真库），验证三件事——
1. 事务边界：两条 INSERT（submission + outbox）同事务，COMMIT 只在两写之后；
   任一条失败必须 ROLLBACK（不留半成品）。
2. 接口校验：>64KB / 语言白名单 → 400；DB 故障 → 500。
3. 返回契约：正常提交 → 201 + {"submission_id": N}。
"""
import pymysql
import pytest
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
    sub_args = fake_conn.cursor_.executed[0][1]
    assert sub_args[0] == 1, "默认 problem_id=1（限额从 problem 表读）"
    assert sub_args[1] == "int main(){}" and sub_args[2] == "cpp", "code/language 顺序正确"
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

def test_submit_returns_201_with_submission_id(client, monkeypatch):
    monkeypatch.setattr(main, "problem_exists", lambda pid: True)
    monkeypatch.setattr(main, "create_submission", lambda code, lang, pid: 42)

    resp = client.post("/api/submit", json={"code": "int main(){}", "language": "cpp"})

    assert resp.status_code == 201
    assert resp.json() == {"submission_id": 42}


def test_submit_404_when_problem_missing(client, monkeypatch):
    """题目不存在必须入口处 404——不能等 worker 判题时落 system_error。"""
    monkeypatch.setattr(main, "problem_exists", lambda pid: False)
    monkeypatch.setattr(main, "create_submission",
                        lambda code, lang, pid: (_ for _ in ()).throw(AssertionError(
                            "题目不存在时绝不能写 submission")))

    resp = client.post("/api/submit",
                       json={"code": "int main(){}", "language": "cpp", "problem_id": 99})

    assert resp.status_code == 404
    assert "99" in resp.json()["detail"]


def test_submit_422_when_problem_id_not_positive(client):
    """problem_id ge=1：0/负数是参数错，pydantic 422 拒绝。"""
    resp = client.post("/api/submit",
                       json={"code": "int main(){}", "language": "cpp", "problem_id": 0})
    assert resp.status_code == 422


def test_submit_rejects_code_over_64kb(client):
    resp = client.post(
        "/api/submit",
        json={"code": "x" * (MAX_CODE_BYTES + 1), "language": "cpp"},
    )
    assert resp.status_code == 400


def test_submit_rejects_unsupported_language(client):
    resp = client.post("/api/submit", json={"code": "int main(){}", "language": "java"})
    assert resp.status_code == 400


def test_submit_returns_500_when_db_unavailable(client, monkeypatch):
    def boom(code, lang, pid):
        raise pymysql.OperationalError("db is down")

    monkeypatch.setattr(main, "create_submission", boom)

    resp = client.post("/api/submit", json={"code": "int main(){}", "language": "cpp"})
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
