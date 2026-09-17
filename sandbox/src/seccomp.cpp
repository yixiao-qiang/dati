// Wave 1 · seccomp 系统调用过滤实现
//
// 面试要点：
// - libseccomp 三段式：seccomp_init → seccomp_rule_add → seccomp_load
// - 默认动作 SCMP_ACT_ERRNO(ENOSYS)：被拦的 syscall 返回 -1, errno=ENOSYS，程序继续
// - 白名单模式：只放行明确允许的 syscall，其余全部拒绝
// - 运行期白名单必须包含 glibc 启动期必调，否则程序一启动就死
#include "seccomp_filter.h"

#include <seccomp.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace sandbox {
namespace seccomp {

// 运行期白名单：允许 glibc 启动必调 + 常见 IO/内存 syscall
// 禁止（默认 ERRNO 拒绝，不需 explicitly 列）：
//   fork/vfork/clone/clone3（防 fork 炸弹）
//   execve（防执行别的程序）
//   socket/connect/bind/listen/accept（防网络访问）
//   ptrace（防调试逃逸）
//   mount/pivot_root/chroot（防文件系统操作）
//   setuid/setgid/capset（防提权）
//   process_vm_readv/process_vm_writev（防跨进程内存读写）
int apply_runtime_filter() {
    // 默认动作：返回 ENOSYS（Function not implemented）
    // 面试要点：开发期用 ERRNO 便于调试，程序能看到哪个 syscall 被拦
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ERRNO(ENOSYS));
    if (!ctx) {
        std::fprintf(stderr, "seccomp: seccomp_init 失败\n");
        return -1;
    }

    // 允许的 syscall 列表（白名单）
    // 面试要点：必须包含 glibc 启动期必调，否则程序在 main() 之前就崩溃
    int allowed[] = {
        // === 进程/线程（glibc 启动必调）===
        SCMP_SYS(exit),
        SCMP_SYS(exit_group),
        SCMP_SYS(execve),             // ★ 必须允许：子进程加载 seccomp 后自己要 execve 启动用户程序
        SCMP_SYS(set_tid_address),    // 设置 TID 地址，glibc 启动必调
        SCMP_SYS(set_robust_list),    // robust mutex 列表，glibc 启动必调
        SCMP_SYS(rseq),               // restartable sequences，glibc 2.35+ 必调
        SCMP_SYS(arch_prctl),         // 设置 FS 寄存器（TLS 基址），glibc 启动必调
        SCMP_SYS(sched_getaffinity),  // 获取 CPU 亲和性，glibc 启动可能调

        // === 内存管理 ===
        SCMP_SYS(mmap),
        SCMP_SYS(munmap),
        SCMP_SYS(mprotect),
        SCMP_SYS(brk),
        SCMP_SYS(madvise),

        // === 文件 IO ===
        SCMP_SYS(read),
        SCMP_SYS(write),
        SCMP_SYS(openat),             // 64位系统上 open 被封装成 openat
        SCMP_SYS(close),
        SCMP_SYS(fstat),
        SCMP_SYS(newfstatat),
        SCMP_SYS(lseek),
        SCMP_SYS(pread64),
        SCMP_SYS(pwrite64),
        SCMP_SYS(readv),
        SCMP_SYS(writev),

        // === 信号 ===
        SCMP_SYS(rt_sigaction),
        SCMP_SYS(rt_sigprocmask),
        SCMP_SYS(rt_sigreturn),

        // === 其他常见 ===
        SCMP_SYS(getrandom),
        SCMP_SYS(ioctl),              // 终端相关，printf 可能调
        SCMP_SYS(close_range),
        SCMP_SYS(uname),
        SCMP_SYS(prlimit64),
        SCMP_SYS(getuid), SCMP_SYS(getgid),
        SCMP_SYS(geteuid), SCMP_SYS(getegid),
        SCMP_SYS(clock_gettime),
        SCMP_SYS(clock_nanosleep),
        SCMP_SYS(nanosleep),
        SCMP_SYS(restart_syscall),
    };

    int count = sizeof(allowed) / sizeof(allowed[0]);
    for (int i = 0; i < count; ++i) {
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, allowed[i], 0) != 0) {
            std::fprintf(stderr, "seccomp: rule_add 失败 (syscall index=%d)\n", i);
            seccomp_release(ctx);
            return -1;
        }
    }

    // 加载过滤器（加载后不可变，必须在 execve 前调用）
    if (seccomp_load(ctx) != 0) {
        std::fprintf(stderr, "seccomp: seccomp_load 失败: %s\n", std::strerror(errno));
        seccomp_release(ctx);
        return -1;
    }

    seccomp_release(ctx);
    return 0;
}

// 编译期白名单（预留接口）
// 当前架构是"沙箱外编译、沙箱内运行"，编译不在沙箱内，暂不调用
// 后续沙箱内编译时，从 gpp_syscalls.txt 读取实测 syscall 列表构建白名单
int apply_compile_filter() {
    // TODO: 从 gpp_syscalls.txt 读取，排除 socket/connect 等危险调用
    return 0;
}

} // namespace seccomp
} // namespace sandbox
