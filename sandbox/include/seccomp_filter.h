// Wave 1 · seccomp 系统调用过滤
// 对应知识点：任务 1.3 seccomp 双阶段白名单；Wave 0 任务 0.3 seccomp 验证
//
// 面试要点：
// - seccomp-bpf 是内核态过滤器，比 ptrace 性能高一个数量级
// - 白名单模式（默认拒绝）比黑名单更安全
// - 默认 ERRNO(ENOSYS) 便于开发调试，KILL 更严但难排查
// - 运行期白名单必须包含 glibc 启动期必调：arch_prctl/set_robust_list/rseq
// - seccomp_load 后规则不可变，必须在 execve 用户程序前加载
// - seccomp 不能做路径级管控（不解析文件路径字符串），需 Landlock/只读挂载配合
#pragma once

namespace sandbox {
namespace seccomp {

// 运行期白名单（严格）：在子进程 execve 用户程序前调用
// 允许 glibc 启动必调 + 常见 IO/内存 syscall，禁止 fork/socket/execve/ptrace 等
// 返回 0 成功，-1 失败
int apply_runtime_filter();

// 编译期白名单（宽松）：预留接口，当前编译在沙箱外暂不调用
// 后续沙箱内编译时启用，输入为 Wave 0 任务 0.4 产物 gpp_syscalls.txt
// 返回 0 成功，-1 失败
int apply_compile_filter();

} // namespace seccomp
} // namespace sandbox
