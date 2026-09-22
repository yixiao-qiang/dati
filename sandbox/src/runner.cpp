// Wave 1 · 沙箱主流程
// 对应知识点：clone flags / 管道同步迁移 / 进程组 / wait4 / 管道 IO
//
// 注意：clone(CLONE_NEWPID) 的子进程是新 pidns 的 PID 1（init），
// PID 1 对未安装 handler 的信号免疫（包括 SIGSTOP），所以不能用 SIGSTOP/SIGCONT 同步，
// 改用管道 read 阻塞（Wave 0 任务 0.6 的方案，计划允许二选一）。
#include "sandbox.h"
#include "cgroup.h"
#include "rootfs.h"
#include "seccomp_filter.h"

#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <sys/resource.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <chrono>

namespace sandbox {

// 传给子进程的参数
struct ChildArgs {
    std::string binary_path;
    int sync_read_fd;  // 同步管道读端（等父进程迁移 cgroup 后放行）
    int in_fd;         // stdin 读端
    int out_fd;        // stdout 写端
    int err_fd;        // stderr 写端
    bool use_rootfs;
    int cpu_limit_sec; // RLIMIT_CPU 兜底（秒），由父进程按 time_limit_ms 换算
};

// 子进程入口（clone 后执行）
static int child_func(void* arg) {
    ChildArgs* args = static_cast<ChildArgs*>(arg);

    // === 知识点2：管道同步——阻塞等父进程迁移 cgroup ===
    // PID 1 对 SIGSTOP 免疫，所以用 read 阻塞代替 raise(SIGSTOP)
    char sync_byte;
    if (read(args->sync_read_fd, &sync_byte, 1) != 1) {
        _exit(127);  // 管道异常
    }
    close(args->sync_read_fd);

    // === 知识点4：自建进程组，pgid = 自己的 PID ===
    // 之后 fork 的子孙自动继承这个进程组
    setpgid(0, 0);

    // 重定向 stdin/stdout/stderr 到管道
    dup2(args->in_fd, 0);
    dup2(args->out_fd, 1);
    dup2(args->err_fd, 2);

    // 关闭不需要的 fd
    close(args->in_fd);
    close(args->out_fd);
    close(args->err_fd);

    // === 知识点3：pivot_root 切根 ===
    std::string exec_path = args->binary_path;
    if (args->use_rootfs) {
        if (rootfs::setup(args->binary_path) != 0) {
            std::fprintf(stderr, "沙箱：rootfs setup 失败\n");
            _exit(127);
        }
        exec_path = "/bin/prog";  // pivot_root 后二进制在 /bin/prog
    }

    // === 知识点8：seccomp 运行期白名单（必须在 execve 前加载，加载后不可变）===
    // 面试要点：默认 ERRNO(ENOSYS)，白名单只放行 glibc 启动必调 + 常见 IO/内存
    // 禁止 fork/socket/execve/ptrace 等危险调用
    if (seccomp::apply_runtime_filter() != 0) {
        std::fprintf(stderr, "沙箱：seccomp 加载失败\n");
        _exit(127);
    }

    // execve 运行用户程序
    char* argv[] = { strdup(exec_path.c_str()), nullptr };
    char* envp[] = { nullptr };
    execve(exec_path.c_str(), argv, envp);

    // execve 失败（比如文件不存在、没有执行权限）
    std::fprintf(stderr, "沙箱：execve 失败: %s\n", std::strerror(errno));
    _exit(127);
}

const char* status_to_string(Status s) {
    switch (s) {
        case Status::OK:             return "OK";
        case Status::NONZERO_EXIT:   return "NONZERO_EXIT";
        case Status::TLE:            return "TLE";
        case Status::MLE:            return "MLE";
        case Status::RUNTIME_ERROR:  return "RUNTIME_ERROR";
        case Status::OLE:            return "OLE";
        case Status::INTERNAL_ERROR: return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

JudgeResult run(const SandboxConfig& config) {
    JudgeResult result;

    // === 1. 创建 cgroup ===
    std::string cg_path = cgroup::create(config.memory_limit_mb, config.pids_limit);
    if (cg_path.empty()) {
        result.error_msg = "创建 cgroup 失败（需要 root）";
        return result;
    }

    // === 2. 创建管道：同步 / stdin / stdout / stderr ===
    int sync_pipe[2], in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(sync_pipe) != 0 || pipe(in_pipe) != 0 ||
        pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        result.error_msg = "创建管道失败";
        cgroup::cleanup(cg_path);
        return result;
    }

    // === 3. 准备子进程参数 ===
    ChildArgs args;
    args.binary_path = config.binary_path;
    args.sync_read_fd = sync_pipe[0];
    args.in_fd = in_pipe[0];
    args.out_fd = out_pipe[1];
    args.err_fd = err_pipe[1];
    args.use_rootfs = config.use_rootfs;

    // === 4. clone 创建隔离子进程 ===
    // 知识点1：CLONE_NEWNS|NEWPID|NEWNET|NEWUTS|NEWIPC 一次性建多重隔离
    const size_t STACK_SIZE = 1024 * 1024;  // 1MB 栈
    char* stack = new char[STACK_SIZE];
    int child_pid = clone(
        child_func,
        stack + STACK_SIZE,  // x86_64 栈向下增长，传栈顶
        CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD,
        &args
    );

    if (child_pid < 0) {
        result.error_msg = std::string("clone 失败: ") + std::strerror(errno);
        close(sync_pipe[0]); close(sync_pipe[1]);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        delete[] stack;
        cgroup::cleanup(cg_path);
        return result;
    }

    // === 5. 父进程关闭不需要的管道端 ===
    close(sync_pipe[0]);  // 父不读同步管道
    close(in_pipe[0]);    // 父不读 stdin
    close(out_pipe[1]);   // 父不写 stdout（关键！否则读端永不 EOF）
    close(err_pipe[1]);   // 父不写 stderr

    // === 6. 迁移子进程进 cgroup（子进程此刻阻塞在 sync read 上）===
    // cgroup 迁移是原子操作，不要求子进程停止；管道保证子进程在 execve 前等待
    if (!cgroup::migrate_pid(child_pid, cg_path)) {
        result.error_msg = "迁移子进程进 cgroup 失败";
        // 放行让子进程退出，然后回收
        char x = 'x';
        write(sync_pipe[1], &x, 1);
        close(sync_pipe[1]);
        waitpid(child_pid, nullptr, 0);
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]);
        delete[] stack;
        cgroup::cleanup(cg_path);
        return result;
    }

    // === 7. 记录开始时间，写同步管道放行子进程 ===
    auto start = std::chrono::steady_clock::now();
    char go = 'x';
    write(sync_pipe[1], &go, 1);
    close(sync_pipe[1]);

    // === 8. 写 stdin，写完关写端（子进程 read 得到 EOF）===
    if (!config.input.empty()) {
        write(in_pipe[1], config.input.data(), config.input.size());
    }
    close(in_pipe[1]);

    // === 9. 设置 stdout/stderr 读端为非阻塞 ===
    int flags = fcntl(out_pipe[0], F_GETFL);
    fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(err_pipe[0], F_GETFL);
    fcntl(err_pipe[0], F_SETFL, flags | O_NONBLOCK);

    // === 10. 主循环：非阻塞读输出 + 超时监控 ===
    std::string stdout_buf, stderr_buf;
    bool timed_out = false;
    bool child_done = false;
    bool ole_triggered = false;
    int64_t output_limit_bytes = (int64_t)config.output_limit_mb *1024 *1024;
    int status = 0;
    char buf[4096];

    while (!child_done) {
        // 读 stdout
        ssize_t n = read(out_pipe[0], buf, sizeof(buf));
        // n == 0: EOF；n == -1 && errno == EAGAIN: 暂时没数据
        if(n > 0){
            // 未超限才 append；append 后可能超一块（最多 4095 字节），由下面的判断捕获
            if(stdout_buf.size() < output_limit_bytes){
                stdout_buf.append(buf, n);
            }
            // OLE 判断：stdout + stderr 总量超限即触发
            // >= 与 > 在此等价：要触发 OLE 必须 size > limit（恰好等于时程序已正常退出）
            // 保留 >= 仅为语义直观
            if(stdout_buf.size() + stderr_buf.size() >= output_limit_bytes){
                ole_triggered = true;
                // 立即杀进程组并 break，不靠超时兜底
                // kill 后管道写端关闭，后面的 drain read 会得到 EOF，不矛盾
                kill(-child_pid, SIGKILL);
                break;
            }
        }
        // 读 stderr
        // OLE 触发后 stderr 永久丢弃：总量已超限，再收无意义
        // 注意：判断用 stdout+stderr 之和，但 stderr 触发 OLE 后被冻结，
        // 此时 stdout 继续涨仍会让总和 >= limit，逻辑自洽
        n = read(err_pipe[0], buf, sizeof(buf));
        if (n > 0 && !ole_triggered) stderr_buf.append(buf, n);

        // 检查墙钟超时
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (elapsed_ms > config.time_limit_ms) {
            timed_out = true;
            // === 知识点4：杀进程组，不是只杀一个 PID ===
            kill(-child_pid, SIGKILL);
            break;
        }

        // 非阻塞检查子进程是否退出
        pid_t w = waitpid(child_pid, &status, WNOHANG);
        if (w == child_pid) {
            child_done = true;
        } else if (w == 0) {
            usleep(1000);  // 1ms，避免 CPU 100%
        }
        // w == -1 (ECHILD) 时继续循环，超时逻辑兜底
    }

    // 超时 break 后，等子进程真正退出
    if (timed_out || ole_triggered) {
        waitpid(child_pid, &status, 0);
    }

    // drain 剩余输出：把管道里残留的数据读完，确保管道缓冲区清空
    // OLE 触发后只 read 不 append（已超限，再收无意义），但必须 read 否则管道可能满
    while (true) {
        ssize_t n = read(out_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (!ole_triggered) stdout_buf.append(buf, n);
    }
    while (true) {
        ssize_t n = read(err_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (!ole_triggered) stderr_buf.append(buf, n);
    }

    // === 11. 读 cgroup 计量 ===
    result.cpu_time_us = cgroup::read_cpu_usage(cg_path);
    result.memory_peak_bytes = cgroup::read_memory_peak(cg_path);
    int oom_kill = cgroup::read_oom_kill(cg_path);

    // === 12. 判题状态 ===
    // 优先级：OLE > TLE > MLE > 信号 > 正常退出
    // OLE 优先：输出超限是最明确的判题结果，即使程序同时超时/被杀也优先报 OLE
    if(ole_triggered){
        result.signal = 9;
        result.status = Status::OLE;
        // 注：OLE 时不读 exit_code——输出已截断，退出码不反映真实程序行为
    }
    else if (timed_out) {
        result.signal = 9;  // SIGKILL（父进程杀的）
        result.status = (oom_kill > 0) ? Status::MLE : Status::TLE;
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
        result.status = (oom_kill > 0) ? Status::MLE : Status::RUNTIME_ERROR;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.status = (result.exit_code == 0) ? Status::OK : Status::NONZERO_EXIT;
    }

    result.stdout_output = stdout_buf;
    result.stderr_output = stderr_buf;

    // 墙钟时间
    auto end = std::chrono::steady_clock::now();
    result.wall_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    // === 13. 清理 ===
    close(out_pipe[0]);
    close(err_pipe[0]);
    delete[] stack;
    cgroup::cleanup(cg_path);

    return result;
}

} // namespace sandbox
