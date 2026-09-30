"""
垂直切片 · FastAPI 判题 server

链路：浏览器提交代码 → 本 server → g++ 编译 → judge_main 判题 → JSON 返回

为什么用子进程调 judge_main 而不是直接 link 沙箱库：
1. 权限隔离：judge_main 需要 root（clone/cgroup/pivot_root），server 不需要
2. 崩溃隔离：判题进程崩了不影响 server 主进程
3. 语言解耦：Python 只管 HTTP 和进程编排，判题逻辑全在 C++

开发期以 root 运行（sudo python3 main.py），judge_main 直接调用。
生产期（Wave 5）用 cgroup delegation + 非 root，judge_main 通过 sudo -n 调用。
"""
import json
import os
import subprocess
import tempfile
import time
import uuid

from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

# judge_main 可执行文件路径（构建产物，由 run_all.sh 的 CMake 构建产生）
# 默认空串 + 启动时校验：不写死路径，clone 仓库的人必须自己配
JUDGE_MAIN = os.environ.get("JUDGE_MAIN", "")

app = FastAPI(title="OJ 判题 server（垂直切片）")

# 启动时校验 JUDGE_MAIN（放在路由挂载之前，配错直接拒绝启动）
if not JUDGE_MAIN:
    raise RuntimeError(
        "JUDGE_MAIN 未配置：请设置环境变量指向 judge_main 可执行文件，"
        "例如 export JUDGE_MAIN=$PWD/sandbox/tests/smoke/build/judge_main"
    )
if not os.path.isfile(JUDGE_MAIN):
    raise RuntimeError(f"JUDGE_MAIN 路径不存在或不是文件: {JUDGE_MAIN}")

# 语言白名单（与计划 Wave 3 任务 3.4 的参数校验前置对齐）
ALLOWED_LANGS = {"cpp", "c"}
MAX_CODE_BYTES = 64 * 1024  # 64KB，与计划一致
COMPILE_TIMEOUT_SEC = 30
JUDGE_TIMEOUT_MS = 3000  # 判题墙钟（ms）

class SubmitRequest(BaseModel):
    code: str
    language: str = "cpp"


def compile_code(code: str, lang: str) -> str:
    """沙箱外编译：写源码到临时文件，g++ 编译成二进制。

    为什么编译在沙箱外（架构决策，面试点）：
    - 编译器依赖大量 syscall 与头文件，沙箱内编译要么白名单过宽（等于不隔离），要么维护成本高
    - 运行用户二进制才是风险面，编译不是

    清理职责（谁创建谁清理）：
    - src_path 是本函数创建，本函数负责删——编译成功、失败、超时三条路径都不留源码
    - bin_path 返回给调用方，由 submit() 的 finally 删（判题后清理）
    """
    src_path = f"/tmp/oj_{uuid.uuid4().hex}.{lang}"
    bin_path = f"/tmp/oj_{uuid.uuid4().hex}"

    with open(src_path, "w", encoding="utf-8") as f:
        f.write(code)

    try:
        cmd = ["g++", "-O2", "-o", bin_path, src_path]
        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=COMPILE_TIMEOUT_SEC,
            )
        except subprocess.TimeoutExpired:
            # 超时也要清掉已写出的源码和可能的半成品二进制
            for p in (src_path, bin_path):
                try:
                    os.unlink(p)
                except OSError:
                    pass
            raise HTTPException(status_code=400, detail="编译超时（>30s）")

        if proc.returncode != 0:
            # 编译错误：把编译器输出回给用户（OJ 的标准行为）
            for p in (src_path,):
                try:
                    os.unlink(p)
                except OSError:
                    pass
            raise HTTPException(
                status_code=400,
                detail=json.dumps({"status": "compile_error", "error": proc.stderr}),
            )
    except BaseException:
        # 任何异常路径都不留源码
        try:
            os.unlink(src_path)
        except OSError:
            pass
        raise

    return bin_path


def judge(bin_path: str) -> dict:
    """调用 judge_main 判题，返回 JSON 结果。

    参数：<binary> <time_ms> <mem_mb> <output_mb>
    """
    cmd = [
        JUDGE_MAIN,
        bin_path,
        str(JUDGE_TIMEOUT_MS),
        "64",   # memory_limit_mb
        "1",    # output_limit_mb
    ]
    # judge_main 需要 root：开发期 server 以 root 跑；若不以 root 跑则用 sudo -n
    if os.geteuid() != 0:
        cmd = ["sudo", "-n"] + cmd

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=JUDGE_TIMEOUT_MS / 1000 + 5,  # 给沙箱内部超时留余量
        )
    except subprocess.TimeoutExpired:
        return {"status": "system_error", "error": "judge_main 无响应"}

    if proc.returncode != 0:
        return {"status": "system_error", "error": f"judge_main 退出码 {proc.returncode}: {proc.stderr}"}

    # judge_main 输出一行 JSON
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        return {"status": "system_error", "error": f"judge_main 输出非法 JSON: {proc.stdout}"}


@app.post("/api/submit")
def submit(req: SubmitRequest):
    # 参数校验前置（Wave 3 任务 3.4 的口径，垂直切片先落实）
    if len(req.code.encode("utf-8")) > MAX_CODE_BYTES:
        raise HTTPException(status_code=400, detail="代码超过 64KB")
    if req.language not in ALLOWED_LANGS:
        raise HTTPException(status_code=400, detail=f"不支持的语言: {req.language}")

    # 编译（沙箱外）
    try:
        bin_path = compile_code(req.code, req.language)
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"编译内部错误: {e}")

    try:
        # 判题
        result = judge(bin_path)
    finally:
        # 清理临时文件
        for p in (bin_path,):
            try:
                os.unlink(p)
            except OSError:
                pass

    # 垂直切片同步返回；Wave 2 换成 Redis Stream 异步队列
    return result


# 静态页面（先丑后美，Wave 4 换 Vue3）
app.mount("/", StaticFiles(directory=os.path.join(os.path.dirname(__file__), "static"), html=True), name="static")
