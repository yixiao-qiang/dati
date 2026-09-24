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
    std::printf("== 测试 08：网络隔离（沙箱内不能联网） ==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_08_net\n");
        return 2;
    }

    // 1. 写一个调用 socket() 的测试程序
    const char* src_path = "/tmp/w1_net_isolation.cpp";
    const char* bin_path = "/tmp/w1_net_isolation";
    {
        std::ofstream f(src_path);
        f << "#include <sys/socket.h>\n"
          << "#include <netinet/in.h>\n"
          << "#include <arpa/inet.h>\n"
          << "#include <cstdio>\n"
          << "#include <cerrno>\n"
          << "#include <cstring>\n"
          << "int main() {\n"
          << "    int fd = socket(AF_INET, SOCK_STREAM, 0);\n"
          << "    if (fd == -1) {\n"
          << "        printf(\"socket failed errno=%d\\n\", errno);\n"
          << "        return 0;\n"  // socket 被拦截，正常退出
          << "    }\n"
          << "    struct sockaddr_in addr;\n"
          << "    addr.sin_family = AF_INET;\n"
          << "    addr.sin_port = htons(80);\n"
          << "    addr.sin_addr.s_addr = inet_addr(\"8.8.8.8\");\n"
          << "    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));\n"
          << "    printf(\"connect rc=%d errno=%d\\n\", rc, errno);\n"
          << "    return 0;\n"
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
        ok("沙箱运行状态 = OK（程序正确处理互联网失败）");
    } else {
        bad(("沙箱运行状态 != OK，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.stderr_output.empty())
            std::printf("  stderr: %s\n", result.stderr_output.c_str());
    }

    if (result.stdout_output.find("errno=38") != std::string::npos) {
        ok("socket() 返回 -1 且 errno=ENOSYS(38)（网络隔离生效）");
    } else {
        bad(("stdout 不包含 errno=38，实际: " + result.stdout_output).c_str());
    }

    if (result.stdout_output.find("connect rc=") == std::string::npos) {
        ok("socket() 失败后未走到 connect（seccomp 拦截在 connect 之前）");
    } else {
        bad("stdout 包含 connect rc=（socket() 成功了，不应该）");
    }

    std::printf("  资源用量: CPU=%ldus 内存峰值=%ldB 墙钟=%ldus\n",
                (long)result.cpu_time_us,
                (long)result.memory_peak_bytes,
                (long)result.wall_time_us);

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
