// Wave 1 · cgroup v2 管理
// 对应知识点：任务 1.1 子进程自停+父迁移；任务 1.4 oom_kill；任务 1.6 计量
#pragma once

#include <string>
#include <cstdint>

namespace sandbox {
namespace cgroup {

// 创建 cgroup 子树，设 memory.max / pids.max
// 返回 cgroup 路径（如 /sys/fs/cgroup/sandbox-XXXXXX），失败返回空串
std::string create(int memory_limit_mb, int pids_limit);

// 把 PID 迁入 cgroup（写 cgroup.procs）
bool migrate_pid(int pid, const std::string& cg_path);

// 读 memory.peak（字节）
int64_t read_memory_peak(const std::string& cg_path);

// 读 cpu.stat 的 usage_usec（微秒）
int64_t read_cpu_usage(const std::string& cg_path);

// 读 memory.events 的 oom_kill 计数
int read_oom_kill(const std::string& cg_path);

// 清理 cgroup（先把进程移走，再 rmdir）
void cleanup(const std::string& cg_path);

} // namespace cgroup
} // namespace sandbox
