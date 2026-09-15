// Wave 0 · 任务 0.6：验证 cgroup 迁移无竞态（管道同步）
// 验收：子进程在"被放行执行"之前就已处于目标 cgroup 内，不依赖 sleep 猜时序
// RED 信号：子进程已继续执行才迁 cgroup（用 sleep 猜测时高概率发生）
//
// 原理：子进程 fork 后立刻阻塞在管道 read 上；父进程趁其阻塞时写 cgroup.procs 完成迁移，
//       迁移完成后才写管道放行。子进程被放行后自报 cgroup 归属，父进程据此判定。
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

static int g_pass = 0, g_fail = 0;
static void ok(const char* m)  { std::printf("  [PASS] %s\n", m); ++g_pass; }
static void bad(const char* m) { std::printf("  [FAIL] %s\n", m); ++g_fail; }

static const char* kCg = "/sys/fs/cgroup/judge-migrate-test";

int main() {
    std::printf("== 任务 0.6：cgroup 迁移无竞态（管道同步）==\n");

    if (geteuid() != 0) { std::printf("需要 root：sudo ./test_cgroup_migrate\n"); return 2; }

    // 预备：清理残留并创建测试 cgroup
    rmdir(kCg);
    if (mkdir(kCg, 0755) != 0) {
        std::printf("  mkdir %s 失败：%s\n", kCg, std::strerror(errno));
        bad("创建测试 cgroup 失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 2;
    }
    ok("已创建测试 cgroup");

    char procs_path[256];
    std::snprintf(procs_path, sizeof procs_path, "%s/cgroup.procs", kCg);
    int cgfd = open(procs_path, O_WRONLY);
    if (cgfd < 0) {
        bad("打开 cgroup.procs 失败");
        rmdir(kCg);
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 2;
    }

    int sync_pipe[2], res_pipe[2];
    if (pipe(sync_pipe) != 0 || pipe(res_pipe) != 0) {
        bad("pipe 创建失败"); close(cgfd); rmdir(kCg); return 2;
    }

    pid_t child = fork();
    if (child < 0) { bad("fork 失败"); close(cgfd); rmdir(kCg); return 2; }

    if (child == 0) {
        // ---- 子进程 ----
        close(sync_pipe[1]); close(res_pipe[0]);

        char b;
        if (read(sync_pipe[0], &b, 1) != 1) _exit(3);   // 阻塞等放行
        close(sync_pipe[0]);

        // 被放行后，报告自己的 cgroup 归属（cgroup v2 为单行：0::/path）
        char path[512] = {0};
        int fd = open("/proc/self/cgroup", O_RDONLY);
        if (fd >= 0) {
            char buf[1024] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                const char* slash = std::strchr(buf, '/');
                if (slash) {
                    size_t i = 0;
                    while (slash[i] && slash[i] != '\n' && i < sizeof(path) - 1) {
                        path[i] = slash[i]; ++i;
                    }
                    path[i] = '\0';
                }
            }
        }
        ssize_t w = write(res_pipe[1], path, std::strlen(path));
        (void)w;
        close(res_pipe[1]);
        _exit(0);
    }

    // ---- 父进程 ----
    close(sync_pipe[0]); close(res_pipe[1]);

    // 此刻子进程阻塞在 read 上、尚未继续执行 —— 这就是安全的迁移窗口
    char pidbuf[32];
    int n = std::snprintf(pidbuf, sizeof pidbuf, "%d", (int)child);
    if (write(cgfd, pidbuf, n) != n) {
        bad("写 cgroup.procs 失败（迁移未完成）");
    } else {
        ok("子进程已迁入目标 cgroup（此刻它仍处于阻塞态）");
    }
    close(cgfd);

    // 迁移完成后才放行
    char x = 'x';
    ssize_t wr = write(sync_pipe[1], &x, 1);
    (void)wr;
    close(sync_pipe[1]);

    char path[512] = {0};
    ssize_t rn = read(res_pipe[0], path, sizeof(path) - 1);
    close(res_pipe[0]);
    if (rn < 0) rn = 0;
    path[rn] = '\0';

    int st = 0;
    waitpid(child, &st, 0);

    std::printf("  子进程自报 cgroup：%s\n", path[0] ? path : "(空)");

    if (std::strstr(path, "judge-migrate-test")) {
        ok("子进程在被放行前已在目标 cgroup 内（无竞态）");
    } else {
        bad("子进程不在目标 cgroup —— 迁移与执行发生竞态");
    }

    if (rmdir(kCg) == 0) ok("已清理测试 cgroup");
    else bad("清理测试 cgroup 失败（可能有残留进程）");

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
