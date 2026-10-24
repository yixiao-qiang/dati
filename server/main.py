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

import hashlib
import hmac
import base64
import secrets
import re
import redis

import pymysql
from fastapi import FastAPI, HTTPException, Header
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

# ============ 认证（Wave 5 台阶 1）============
# scrypt 慢哈希参数：n=2**14 把单次验证拉到 ~70ms，爆破成本 ×10^6
SCRYPT_N = 2 ** 14
SCRYPT_R = 8
SCRYPT_P = 1
SCRYPT_DKLEN = 32

SESSION_TTL = 24 * 3600        # session 24h 过期——Redis TTL 天然到期删除
SESSION_PREFIX = "session:"    # key 命名空间，防止与其他 key 撞名

# session 存 Redis 而不是 DB 表（为什么，面试点）：
#   TTL 天然过期（不用写清理任务）/ DEL 原子读删（logout 幂等）/ 多实例共享
REDIS = redis.Redis(
    host=os.environ.get("OJ_REDIS_HOST", "127.0.0.1"),
    port=6379, db=0,
    decode_responses=True,     # 返回值直接是 str，省去每处 bytes.decode()
)

USERNAME_RE = re.compile(r"^[a-zA-Z0-9_]{3,20}$")  # 白名单：防奇怪字符进排行榜/注册
MIN_PASSWORD_LEN = 8

app = FastAPI(title="OJ 判题 server（Wave 3 异步判题）")

# 语言白名单 + 代码上限（与 init.sql 的 language 列、MEDIUMTEXT 对齐）
ALLOWED_LANGS = {"cpp", "c"}
MAX_CODE_BYTES = 64 * 1024  # 64KB
# 用例输入/期望输出上限与批次上限：DB 列是 MEDIUMTEXT(16MB)，接口若不设限，
# 单条超大用例会撑爆存储与判题管道（FastAPI 对请求体大小默认无限制）
MAX_CASE_BYTES = 256 * 1024   # 单条 input/expected_output ≤ 256KB
MAX_TESTCASES = 100           # 单次挂载 ≤ 100 条


class SubmitRequest(BaseModel):
    code: str
    language: str = "cpp"
    # 关联题目（限额从 problem 表读）。ge=1 挡 0/负数——题目 id 从 1 起
    problem_id: int = Field(default=1, ge=1)

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

def hash_password(password: str) -> str:
    """scrypt 慢哈希，返回自描述格式：盐和参数全部内嵌在串里。

    为什么自己定格式（面试点）：bcrypt 把盐内嵌在哈希串（$2b$...），
    scrypt 不内嵌——盐、n/r/p 不存进串里，verify 时就无从重建。
    存 scrypt$16384$8$1$<salt_b64>$<hash_b64>：重建哈希所需参数全在串里。
    """
    salt = os.urandom(16)   # 每次随机 → 同一个密码两次哈希结果不同
    digest = hashlib.scrypt(
        password.encode("utf-8"),
        salt=salt,
        n=SCRYPT_N,
        r=SCRYPT_R,
        p=SCRYPT_P,
        dklen=SCRYPT_DKLEN,
    )
    return "scrypt${}${}${}${}${}".format(
        SCRYPT_N,
        SCRYPT_R,
        SCRYPT_P,
        base64.b64encode(salt).decode("ascii"),
        base64.b64encode(digest).decode("ascii"),
    )


def verify_password(password: str, stored: str) -> bool:
    """解析自描述格式 → 重算哈希 → 常数时间比较。

    为什么 try/except 包全函数：格式非法（段数不对/解码失败）是脏数据，
    返回 False 而不是抛异常——login 不需要区分"密码错"和"库被写坏"。
    为什么 hmac.compare_digest：普通 == 按字节短路返回，攻击者可以按
    耗时差异猜出哈希前缀——常数时间比较没有这个侧信道（面试点）。
    """
    try:
        scheme, n, r, p, salt_b64, hash_b64 = stored.split("$")
        if scheme != "scrypt":
            return False
        salt = base64.b64decode(salt_b64)
        expected = base64.b64decode(hash_b64)
        actual = hashlib.scrypt(
            password.encode("utf-8"),
            salt=salt,
            n=int(n),
            r=int(r),
            p=int(p),
            dklen=len(expected),
        )
        return hmac.compare_digest(actual, expected)
    except Exception:
        return False

def get_user_by_name(username: str) -> Optional[dict]:
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, username, password_hash FROM users WHERE username=%s",
                (username,),
            )
            return cur.fetchone()
    finally:
        conn.close()

def create_user(username: str, password_hash: str) -> int:
    """INSERT users，返回新 id。唯一键冲突（IntegrityError）由调用方转 409。

    参数名必须是 password_hash 不是 password：这里存的是哈希串，
    叫 password 会误导读代码的人以为存了明文（安全语义问题）。
    """
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO users (username, password_hash) VALUES (%s, %s)",
                (username, password_hash),
            )
        conn.commit()
        return cur.lastrowid
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()

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


def problem_exists(pid: int) -> bool:
    """题目存在性检查（提交接口用）。

    为什么提交前必须查：submission.problem_id 无外键约束，不查会静默插入
    孤儿提交，直到 worker 判题时才发现题目没有用例，落一个用户看不懂的
    system_error——错误应该在入口处以 404 拒绝，而不是判题末端报系统错误。
    （检查与插入之间存在竞态窗口：题目恰好被删仍可能入库——学习期接受，
    生产用外键约束兜底。）
    """
    conn = pymysql.connect(**DB_CONFIG)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT 1 FROM problem WHERE id=%s", (pid,))
            return cur.fetchone() is not None
    finally:
        conn.close()


class AuthRequest(BaseModel):
    username: str
    password: str

@app.post("/api/register", status_code=201)
def register(req: AuthRequest):
    if not USERNAME_RE.match(req.username):
        raise HTTPException(status_code=400, detail="用户名需 3-20 位字母/数字/下划线")
    if len(req.password) < MIN_PASSWORD_LEN:
        raise HTTPException(status_code=400, detail="密码需 8 位以上")
    try:
        create_user(req.username, hash_password(req.password))
    except pymysql.IntegrityError:
        raise HTTPException(status_code=409, detail=f"用户名已存在: {req.username}")
    except pymysql.MySQLError:
        raise HTTPException(status_code=500, detail="数据库不可用，请稍后重试")
    return {"username": req.username}

@app.post("/api/login")
def login(req: AuthRequest):
    # 格式校验同样做（400）；凭据错统一 401——不泄露"用户名是否存在"
    if not USERNAME_RE.match(req.username) or len(req.password) < MIN_PASSWORD_LEN:
        raise HTTPException(status_code=400, detail="用户名或密码格式错误")
    user = get_user_by_name(req.username)
    if user is None or not verify_password(req.password, user["password_hash"]):
        # 用户不存在 / 密码错共用同一条 401 同文案：响应逐字节一致，
        # 攻击者无法靠"哪个先返回/文案差异"探测用户是否注册（面试点）
        raise HTTPException(status_code=401, detail="用户名或密码错误")
    token = secrets.token_hex(32)   # 256 bit 熵，不可预测
    REDIS.setex(f"{SESSION_PREFIX}{token}", SESSION_TTL, str(user["id"]))
    return {"token": token, "username": req.username}

@app.post("/api/logout")
def logout(authorization: str = Header(None)):
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="未登录")
    token = authorization[len("Bearer "):].strip()
    REDIS.delete(f"{SESSION_PREFIX}{token}")
    return {"ok": True}

@app.post("/api/submit", status_code=201)
def submit(req: SubmitRequest):
    # 校验前置：参数错是用户的错（400），题目不存在是凭据错（404），
    # DB 错是环境的错（500）——三者必须分开（面试点）
    if len(req.code.encode("utf-8")) > MAX_CODE_BYTES:
        raise HTTPException(status_code=400, detail="代码超过 64KB")
    if req.language not in ALLOWED_LANGS:
        raise HTTPException(status_code=400, detail=f"不支持的语言: {req.language}")

    try:
        if not problem_exists(req.problem_id):
            raise HTTPException(status_code=404, detail=f"题目不存在: {req.problem_id}")
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
    ★ 判题答案（expected_output）不下发：OJ 的标准答案经 API 泄露等于
      题目作废，判题侧 worker 直接读 DB 拿答案，不经本接口。
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
                "SELECT id, ordinal, input, score "
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
    if len(req.testcases) > MAX_TESTCASES:
        raise HTTPException(
            status_code=400, detail=f"单次最多挂载 {MAX_TESTCASES} 条用例")
    for i, tc in enumerate(req.testcases):
        if (len(tc.input.encode("utf-8")) > MAX_CASE_BYTES
                or len(tc.expected_output.encode("utf-8")) > MAX_CASE_BYTES):
            raise HTTPException(
                status_code=400,
                detail=f"第 {i} 条用例的 input/expected_output 超过 256KB 上限")
        if not (1 <= tc.score <= 100):
            raise HTTPException(
                status_code=400, detail=f"第 {i} 条用例的 score 需在 1-100 之间")
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
