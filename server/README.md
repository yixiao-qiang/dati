# OJ 判题 server（Wave 3 · 异步判题）

链路：浏览器提交代码 → `POST /api/submit` → 一个事务写 `submission` + `outbox` → 返回 `submission_id`。
判题由 worker 侧投递器扫 outbox → XADD 入 Redis → worker 判题 → 写回 DB（Wave 2 全套机制，本 server 不再判题）。

## 快速开始（三行）

```bash
# 1. 装依赖（Python 3.10+）
python3 -m pip install -r server/requirements.txt

# 2. 建表（worker/init.sql：oj 库 + submission + outbox + oj 账号）
sudo mysql < worker/init.sql

# 3. 启动（无需 JUDGE_MAIN——判题已移到 worker）
sudo python3 -m uvicorn server.main:app --host 0.0.0.0 --port 8000
```

> 本机若同时有多个 Python，务必用 `python3 -m pip` 而不是裸 `pip`，避免装到别的解释器。

## 接口

| 接口 | 方法 | 入参 | 返回 | 错误 |
|---|---|---|---|---|
| `/api/submit` | POST | `{code, language}` | `201 {"submission_id": N}` | 400 参数错（>64KB / 语言不在白名单）、500 DB 不可用 |

`submission_id` 是查询凭据：前端拿它轮询 `/api/submission/{id}`（台阶 2 实现）。

## 测试（本机可跑，无需 MySQL——pymysql 被 mock）

```bash
python3 -m pip install -r server/requirements-dev.txt
cd server && python3 -m pytest test_main.py -v
```

覆盖：事务边界（两条 INSERT 同事务、异常回滚）、接口校验（400/500）、返回契约（201 + submission_id）。

## 架构决策速记（面试）

- 语言按层选型：沙箱/worker 用 C++（系统调用、资源计量），server 用 Python（纯 IO 编排）
- 提交接口不碰 Redis：outbox 解耦登记与投递，Redis 挂了不阻塞提交（本地事务 + 最终一致）
- 同事务写 submission+outbox：拆开会导致"登记了但永不投递"的无声丢失
- `autocommit=False`：PyMySQL 默认每条语句自动提交，必须关掉才能控制事务边界
- 错误分类：参数错 = 用户的错（400）；DB 错 = 环境的错（500，前端提示重试）
