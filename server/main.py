"""
OJ 判题 server · Wave 3 台阶 1：提交接口登记化（异步判题闭环第一段）

链路：浏览器提交代码 → POST /api/submit → 一个事务写 submission + outbox → 返回 submission_id
判题不再由本 server 完成：worker 侧投递器扫 outbox → XADD 入 Redis → worker 判题 → 写回 DB。

为什么 server 用 Python（架构决策，面试点）：
- server 是纯 IO 编排：收 HTTP → 校验 → 写 DB → 返回 JSON，无性能敏感路径
- 沙箱/worker 是系统调用敏感层用 C++，Web 编排层用 Python——语言按层选型

为什么提交接口不碰 Redis（outbox 模式）：
- 提交只写本地 DB（submission + outbox 同事务），投递由投递器异步完成
- Redis 挂了不阻塞提交，恢复后投递器补投——本地事务 + 最终一致
"""
import os
from typing import List, Optional

import pymysql
from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

DB_CONFIG = {
    "host": os.environ.get("OJ_DB_HOST", "127.0.0.1"),
    "user": os.environ.get("OJ_DB_USER", "oj"),
    "password": os.environ.get("OJ_DB_PASSWORD", "oj123456"),
    "database": "oj",
    "charset": "utf8mb4",
    "autocommit": False,  # 必须手动关：两张 INSERT 要落在同一个事务里
    "cursorclass": pymysql.cursors.DictCursor,
}

app = FastAPI(title="OJ 判题 server（Wave 3 异步判题）")

# 语言白名单 + 代码上限（与 init.sql 的 language 列、MEDIUMTEXT 对齐）
ALLOWED_LANGS = {"cpp", "c"}
MAX_CODE_BYTES = 64 * 1024  # 64KB


class SubmitRequest(BaseModel):
    code: str
    language: str = "cpp"
    problem_id: int = 1  # 关联题目（限额从 problem 表读；学习期默认题 1）

class CreateProblemRequest(BaseModel):
    title: str
    time_limit_ms: int = 1000
    mem_limit_mb: int = 256
    output_limit_mb: int = 64

class TestcaseItem(BaseModel):
    input: str = ""
    expected_output: str = ""
    score: int = 1

class AddTestcasesRequest(BaseModel):
    testcases: List[TestcaseItem]

def create_submission(code: str, language: str, problem_id: int = 1) -> int:
    """一个事务写 submission + outbox，返回 submission_id。

    为什么同事务（台阶 1 核心设计决策）：
    - 拆两个事务 → submission 成功、outbox 失败 → 提交"登记了"但永远不会被投递，
      pending 一辈子（与 worker P0-2 同款的无声丢失）
    - 同事务保证要么都成功要么都不发生——用户拿不到 id 即可安全重试
    """
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO submission (problem_id, code, language, status) "
                "VALUES (%s, %s, %s, 'pending')",
                (problem_id, code, language),
            )
            sid = cur.lastrowid
            cur.execute(
                "INSERT INTO outbox (submission_id, event, delivered) VALUES (%s, 'judged', 0)",
                (sid,),
            )
        conn.commit()
        return sid
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


@app.post("/api/submit", status_code=201)
def submit(req: SubmitRequest):
    # 校验前置：参数错是用户的错（400），DB 错是环境的错（500）
    if len(req.code.encode("utf-8")) > MAX_CODE_BYTES:
        raise HTTPException(status_code=400, detail="代码超过 64KB")
    if req.language not in ALLOWED_LANGS:
        raise HTTPException(status_code=400, detail=f"不支持的语言: {req.language}")

    try:
        sid = create_submission(req.code, req.language, req.problem_id)
    except pymysql.MySQLError:
        raise HTTPException(status_code=500, detail="数据库不可用，请稍后重试")

    return {"submission_id": sid}


def get_submission(sid: int) -> Optional[dict]:
    """根据 submission_id 查询提交状态。"""
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, language, status, result, stdout, stderr, "
                "passed_cases, total_cases, created_at "
                "FROM submission WHERE id=%s",
                (sid,),
            )
            row = cur.fetchone()
    finally:
        conn.close()
    return row


def create_problem(title, time_limit_ms, mem_limit_mb, output_limit_mb) -> int:
    """创建题目：INSERT problem → 返回新 id。"""
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO problem (title, time_limit_ms, mem_limit_mb, output_limit_mb) "
                "VALUES (%s, %s, %s, %s)",
                (title, time_limit_ms, mem_limit_mb, output_limit_mb),
            )
            conn.commit()
            return cur.lastrowid
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()

def list_problems() -> list:
    """列出所有题目（不含用例，列表页用）。"""
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, title, time_limit_ms, mem_limit_mb, output_limit_mb "
                "FROM problem"
            )
            rows = cur.fetchall()
    finally:
        conn.close()
    return rows

def get_problem_with_testcases(pid) -> Optional[dict]:
    """根据题目 ID 查询题目及所有测试用例。

    None = 题目不存在（HTTP 层据此 404）。
    """
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, title, time_limit_ms, mem_limit_mb, output_limit_mb "
                "FROM problem WHERE id=%s",
                (pid,),
            )
            row = cur.fetchone()
            if row is None:
                return None
            # 用例按业务序（ordinal）排：判题逐用例跑，顺序就是执行顺序
            cur.execute(
                "SELECT id, ordinal, input, expected_output, score "
                "FROM testcase WHERE problem_id=%s ORDER BY ordinal",
                (pid,),
            )
            row["testcases"] = cur.fetchall()
    finally:
        conn.close()
    return row

def add_testcases(pid, testcases) -> Optional[int]:
    """给题目挂载测试用例。

    None = 题目不存在（HTTP 层据此 404）；否则返回插入条数。
    """
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            # ① 存在性检查：不查就 INSERT → 孤儿用例（表无外键约束时静默产生）
            cur.execute("SELECT id FROM problem WHERE id=%s", (pid,))
            if cur.fetchone() is None:
                return None
            # ② executemany 一次往返批量插入（N 次 execute = N 次网络往返）
            #    ordinal = 数组下标（业务顺序 = 数组顺序，与详情接口排序一致）
            cur.executemany(
                "INSERT INTO testcase (problem_id, input, expected_output, ordinal, score) "
                "VALUES (%s, %s, %s, %s, %s)",
                [(pid, tc.input, tc.expected_output, i, tc.score)
                 for i, tc in enumerate(testcases)],
            )
        conn.commit()
        return len(testcases)
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()

@app.get("/api/submission/{sid}")
def submission_status(sid: int):
    try:
        row = get_submission(sid)
    except pymysql.MySQLError:
        raise HTTPException(status_code=500, detail="数据库不可用，请稍后重试")

    if row is None:
        # 404 = 凭据错；200+pending = 正常在途。两者必须分开（面试点）
        raise HTTPException(status_code=404, detail=f"提交不存在: {sid}")

    return {
        "submission_id": row["id"],
        "language": row["language"],
        "status": row["status"],          # pending/judging/终态/dead——前端轮询的终止条件
        "result": row["result"],
        "stdout": row["stdout"],
        "stderr": row["stderr"],
        "passed_cases": row["passed_cases"],   # 台阶3：通过/总用例数（展示+部分分基础）
        "total_cases": row["total_cases"],
        "created_at": row["created_at"].isoformat() if row["created_at"] else None,
    }

@app.post("/api/problems", status_code=201)
def create_problem_endpoint(req: CreateProblemRequest):
    # 业务校验（pydantic 只保证类型，范围要手动）：参数错是用户的错（400）
    if not req.title.strip():
        raise HTTPException(status_code=400, detail="标题不能为空")
    if not (1 <= req.time_limit_ms <= 60000):
        raise HTTPException(status_code=400, detail="time_limit_ms 需在 1-60000 之间")
    if not (16 <= req.mem_limit_mb <= 1024):
        raise HTTPException(status_code=400, detail="mem_limit_mb 需在 16-1024 之间")
    if not (1 <= req.output_limit_mb <= 256):
        raise HTTPException(status_code=400, detail="output_limit_mb 需在 1-256 之间")

    try:
        pid = create_problem(req.title, req.time_limit_ms, req.mem_limit_mb, req.output_limit_mb)
    except pymysql.MySQLError:
        raise HTTPException(status_code=500, detail="数据库不可用，请稍后重试")
    return {"problem_id": pid}

@app.get("/api/problems")
def list_problems_endpoint():
    try:
        return list_problems()
    except pymysql.MySQLError:
        raise HTTPException(status_code=500, detail="数据库不可用，请稍后重试")

@app.get("/api/problems/{problem_id}")
def problem_detail(problem_id: int):
    try:
        row = get_problem_with_testcases(problem_id)
    except pymysql.MySQLError:
        raise HTTPException(status_code=500, detail="数据库不可用，请稍后重试")
    if row is None:
        raise HTTPException(status_code=404, detail=f"题目不存在: {problem_id}")
    return row

@app.post("/api/problems/{problem_id}/testcases", status_code=201)
def add_testcases_endpoint(problem_id: int, req: AddTestcasesRequest):
    if not req.testcases:
        raise HTTPException(status_code=400, detail="用例列表不能为空")
    try:
        n = add_testcases(problem_id, req.testcases)
    except pymysql.MySQLError:
        raise HTTPException(status_code=500, detail="数据库不可用，请稍后重试")
    if n is None:
        raise HTTPException(status_code=404, detail=f"题目不存在: {problem_id}")
    return {"problem_id": problem_id, "count": n}


# 静态页面（先丑后美，Wave 4 换 Vue3）
app.mount(
    "/",
    StaticFiles(directory=os.path.join(os.path.dirname(__file__), "static"), html=True),
    name="static",
)
