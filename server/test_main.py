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


class FakeCursor:
    """记录 execute 的 SQL，可在 outbox INSERT 时注入失败（模拟第二条写崩）。"""

    def __init__(self, fail_on_outbox: bool = False):
        self.executed: list = []
        self.lastrowid = 42  # 模拟 AUTO_INCREMENT 返回
        self.fail_on_outbox = fail_on_outbox

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def execute(self, sql: str, args=None):
        self.executed.append((sql, args))
        if self.fail_on_outbox and sql.startswith("INSERT INTO outbox"):
            raise pymysql.OperationalError("simulated outbox insert failure")


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
    monkeypatch.setattr(main, "create_submission", lambda code, lang, pid: 42)

    resp = client.post("/api/submit", json={"code": "int main(){}", "language": "cpp"})

    assert resp.status_code == 201
    assert resp.json() == {"submission_id": 42}


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
