# OJ 判题 server（垂直切片）

判题全链路：浏览器提交代码 → 本 server → g++ 编译 → judge_main（C++ 沙箱）→ JSON 返回。

## 快速开始（三行）

```bash
# 1. 装依赖
pip install -r server/requirements.txt

# 2. 构建沙箱（产出 judge_main，本机需 Ubuntu + root 权限）
sudo bash sandbox/tests/smoke/run_all.sh 10

# 3. 启动（JUDGE_MAIN 指向构建产物）
JUDGE_MAIN=$PWD/sandbox/tests/smoke/build/judge_main \
  sudo python3 -m uvicorn server.main:app --host 0.0.0.0 --port 8000
```

## 为什么必须配 JUDGE_MAIN

judge_main 是 C++ 沙箱的 CLI 判题入口（clone/cgroup/pivot_root，需要 root）。
server 不写死它的路径——克隆仓库的人环境不同，启动时未配置会直接拒绝启动。

## 测试

```bash
# 沙箱冒烟（12 测试 64 断言）
sudo bash sandbox/tests/smoke/run_all.sh

# API 冒烟（VM 无 curl 时用 Python urllib，见仓库学习笔记/垂直切片_对接文档.md）
```

## 架构决策速记（面试）

- 编译在沙箱外：编译器依赖大量 syscall/头文件，运行用户二进制才是风险面
- 子进程调 judge_main 而非 link 库：权限/崩溃隔离、语言解耦
- 参数校验前置：pydantic 管格式，业务代码管语义
