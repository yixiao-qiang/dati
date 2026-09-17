// Wave 1 · 测试 02：seccomp 运行期白名单拦截 socket()
// 验收：沙箱内调用 socket() 返回 -1 且 errno=ENOSYS(38)，程序正常退出
#include "sandbox.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void ok(const char* m)  { std::printf("  [PASS] %s\n", m); ++g_pass; }
static void bad(const char* m) { std::printf("  [FAIL] %s\n", m); ++g_fail; }

int main() {
    std::printf("== 测试 02：seccomp 运行期白名单拦截 socket() ==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_02_seccomp\n");
        return 2;
    }

    // 1. 写一个调用 socket() 的测试程序
    const char* src_path = "/tmp/w1_seccomp_socket.cpp";
    const char* bin_path = "/tmp/w1_seccomp_socket";
    {
        std::ofstream f(src_path);
        f << "#include <sys/socket.h>\n"
          << "#include <cstdio>\n"
          << "#include <cerrno>\n"
          << "#include <cstring>\n"
          << "int main() {\n"
          << "    int fd = socket(AF_INET, SOCK_STREAM, 0);\n"
          << "    printf(\"socket returned %d, errno=%d (%s)\\n\", fd, errno, strerror(errno));\n"
          << "    // 预期：fd=-1, errno=ENOSYS(38)，说明 seccomp 拦截成功\n"
          << "    return (fd == -1 && errno == ENOSYS) ? 0 : 1;\n"
          << "}\n";
    }

    // 2. 动态编译
    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        bad("编译 socket 测试程序失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("socket 测试程序编译成功（动态链接）");

    // 3. 在沙箱里运行
    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin_path;
    cfg.input = "";
    cfg.time_limit_ms = 2000;
    cfg.memory_limit_mb = 64;
    cfg.pids_limit = 16;
    cfg.use_rootfs = true;

    sandbox::JudgeResult result = sandbox::run(cfg);

    // 4. 断言
    if (result.status == sandbox::Status::OK) {
        ok("沙箱运行状态 = OK（socket 被拦截后程序正常退出，退出码 0）");
    } else {
        bad(("沙箱运行状态 != OK，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.stderr_output.empty())
            std::printf("  stderr: %s\n", result.stderr_output.c_str());
    }

    if (result.stdout_output.find("errno=38") != std::string::npos) {
        ok("socket() 返回 -1 且 errno=ENOSYS(38)（seccomp 拦截成功）");
    } else {
        bad(("stdout 不包含 errno=38，实际: " + result.stdout_output).c_str());
    }

    if (result.stdout_output.find("Function not implemented") != std::string::npos) {
        ok("错误信息为 'Function not implemented'（ENOSYS 对应描述）");
    } else {
        bad("stdout 不包含 'Function not implemented'");
    }

    std::printf("  资源用量: CPU=%ldus 内存峰值=%ldB 墙钟=%ldus\n",
                (long)result.cpu_time_us,
                (long)result.memory_peak_bytes,
                (long)result.wall_time_us);

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
