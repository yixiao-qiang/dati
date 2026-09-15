// Wave 0 · 任务 0.3：验证 seccomp 白名单
// 验收：装载规则后 socket() 返回 -1，errno = ENOSYS
// RED 信号：socket 正常返回 fd（规则未生效——通常是忘了 seccomp_load）
// 构建：见同目录 CMakeLists.txt（需 libseccomp-dev）
#include <seccomp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

static int g_pass = 0, g_fail = 0;
static void ok(const char* m)  { std::printf("  [PASS] %s\n", m); ++g_pass; }
static void bad(const char* m) { std::printf("  [FAIL] %s\n", m); ++g_fail; }

int main() {
    std::printf("== 任务 0.3：seccomp 白名单 ==\n");

    // --- 对照组：装载前 socket 应当正常 ---
    // 先确认能看见"成功"，再确认能看见"失败"；只测失败等于没测。
    int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    if (probe >= 0) {
        ok("对照组：装载前 socket() 正常返回 fd");
        ::close(probe);
    } else {
        bad("对照组：装载前 socket() 就失败了——环境异常，本测试结论不可信");
    }

    // --- 建 seccomp 上下文：默认全放行，只把 socket 改成返回 ENOSYS ---
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (!ctx) {
        bad("seccomp_init 失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 2;
    }

    if (seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(socket), 0) != 0) {
        bad("seccomp_rule_add 失败");
        seccomp_release(ctx);
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 2;
    }
    ok("seccomp_rule_add：socket → SCMP_ACT_ERRNO(ENOSYS)");

    // --- 装载（漏了这步规则不生效）---
    if (seccomp_load(ctx) != 0) {
        bad("seccomp_load 失败——规则未生效");
        seccomp_release(ctx);
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 2;
    }
    ok("seccomp_load 成功");

    // --- 验证 ---
    errno = 0;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int e = errno;
    std::printf("  socket() 返回 %d，errno=%d (%s)\n", fd, e, std::strerror(e));

    if (fd == -1 && e == ENOSYS) {
        ok("socket() 被拦截：返回 -1 且 errno=ENOSYS");
    } else {
        bad("socket() 未被拦截——RED 信号：规则未生效（检查 seccomp_load）");
    }

    seccomp_release(ctx);
    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
