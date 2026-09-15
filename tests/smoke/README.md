# Wave 0 冒烟测试套件

> **对应**：《第三版执行计划》Wave 0「环境验证与隔离冒烟测试」的 8 条任务（0.1~0.8）。
> **运行环境**：**必须在 Ubuntu VM 内跑**（Windows 上跑不了——无 Linux 内核能力）。VM 实测基线见《学习笔记/环境体检.md》第七节。
> **状态**：⏳ **本套件尚未在 VM 内执行过**。脚本本身经过 `bash -n` 语法校验，但**判定逻辑未经真机验证**——首次运行时如遇误判，属预期内，需按实情修正。

---

## 怎么跑

```bash
# 在 VM 内
cd tests/smoke
sudo bash run_all.sh          # 跑全部 8 条，最后给汇总
sudo bash run_all.sh 0.1 0.3  # 只跑指定几条
```

单条也可以独立跑：

```bash
sudo bash 01_namespace.sh
```

---

## 8 条对照（计划原文的验收标准）

| 编号 | 测试项 | 实现的文件 | 通过标准 |
|---|---|---|---|
| 0.1 | namespace 隔离 | `01_namespace.sh` | 隔离内看不到宿主挂载；退出后无残留 |
| 0.2 | cgroup v2 可写 | `02_cgroup_v2.sh` | 能读到 `memory.peak` / `cpu.stat` 的 `usage_usec` |
| 0.3 | seccomp 白名单 | `03_seccomp_socket.cpp` | `socket()` 返回 -1，errno=ENOSYS |
| 0.4 | g++ syscall 序列 | `04_strace_gpp.sh` | 确认 `clone3/statx/rseq` 存在，产物落盘 |
| 0.5 | pivot_root + umount | `05_pivot_root.cpp` | `ls /` 只见 tmpfs |
| 0.6 | cgroup 迁移同步 | `06_cgroup_migrate.cpp` | 子进程 exec 前已在目标 cgroup（迁后放行） |
| 0.7 | Landlock 可用 | `07_landlock.sh` | LSM 列表含 `landlock`（否则标注降级） |
| 0.8 | Redis Stream | `08_redis_stream.sh` | XACK 后 `XPENDING` 为 0 |

**8/8 全绿才进入 Wave 1**（计划硬门禁）。

---

## 产物落盘位置

| 产物 | 由谁产生 | 用途 |
|---|---|---|
| `artifacts/gpp_trace.txt` | 0.4 | g++ 完整 syscall trace |
| `artifacts/gpp_syscalls.txt` | 0.4 | 去重后的 syscall 列表——**Wave 1 编译期白名单的输入** |
| `artifacts/run.log` | `run_all.sh` | 本次全量运行日志 |

> `artifacts/` 下的产物是**证据留档**，按计划要求保留，不要删。

---

## 为什么 shell 和 C++ 混用

- **能用 shell 的用 shell**（0.1/0.2/0.4/0.7/0.8）：好读、好查、无需编译。
- **必须用 C++ 的用 C++**（0.3/0.5/0.6）：要链 `libseccomp`、直接调 `pivot_root`/`clone` 系统调用，shell 表达不了。

C++ 部分用 CMake 构建：

```bash
cmake -B build -S . && cmake --build build
# 产物在 build/ 下：test_seccomp / test_pivot_root / test_cgroup_migrate
```

---

## 判定与退出码约定

- 每个脚本/程序：`exit 0` = PASS，非 0 = FAIL。
- `run_all.sh` 汇总各条结果并给出总退出码（全绿才 0）。
- 输出里 `[PASS]` / `[FAIL]` 便于肉眼扫读。
