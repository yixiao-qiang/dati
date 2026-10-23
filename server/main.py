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

import pymysql
from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

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


def create_submission(code: str, language: str) -> int:
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
                "INSERT INTO submission (code, language, status) VALUES (%s, %s, 'pending')",
                (code, language),
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
        sid = create_submission(req.code, req.language)
    except pymysql.MySQLError:
        raise HTTPException(status_code=500, detail="数据库不可用，请稍后重试")

    return {"submission_id": sid}


def get_submission(sid: int) -> dict | None:
    """读 submission 一行。不存在返回 None，调用方决定 404。

    为什么查询走 DB 不走 Redis（面试点）：
    - 状态唯一权威在 MySQL：worker 的 claim/finish/rescue 都写它
    - Redis 只存"消息谁在处理"（PEL），不存状态——问状态永远问 DB
    """
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, language, status, result, stdout, stderr, created_at "
                "FROM submission WHERE id=%s",
                (sid,),
            )
            row = cur.fetchone()
    finally:
        conn.close()
    return row


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
        "created_at": row["created_at"].isoformat() if row["created_at"] else None,
    }


# 静态页面（先丑后美，Wave 4 换 Vue3）
app.mount(
    "/",
    StaticFiles(directory=os.path.join(os.path.dirname(__file__), "static"), html=True),
    name="static",
)
