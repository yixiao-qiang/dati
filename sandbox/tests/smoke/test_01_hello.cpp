// Wave 1 · 测试 01：hello world 能在沙箱里编译并运行
// 验收：状态 OK，stdout 包含 "Hello, World!"
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
    std::printf("== 测试 01：沙箱内运行 hello world ==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_01_hello\n");
        return 2;
    }

    // 1. 写 hello world 源码
    const char* src_path = "/tmp/w1_hello.cpp";
    const char* bin_path = "/tmp/w1_hello";
    {
        std::ofstream f(src_path);
        f << "#include <cstdio>\n"
          << "int main() {\n"
          << "    printf(\"Hello, World!\\n\");\n"
          << "    return 0;\n"
          << "}\n";
    }

    // 2. 动态编译（默认动态链接，验证 rootfs 的动态库 bind mount 是否生效）
    //    面试要点：动态程序运行时需要 ld-linux 加载 .so，沙箱必须提供 /lib /lib64 /usr/lib
    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        bad("编译 hello world 失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("hello world 编译成功（动态链接）");

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
        ok("沙箱运行状态 = OK");
    } else {
        bad(("沙箱运行状态 != OK，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.error_msg.empty())
            std::printf("  错误: %s\n", result.error_msg.c_str());
        if (!result.stderr_output.empty())
            std::printf("  stderr: %s\n", result.stderr_output.c_str());
    }

    if (result.stdout_output.find("Hello, World!") != std::string::npos) {
        ok("stdout 包含 \"Hello, World!\"");
    } else {
        bad(("stdout 不包含 \"Hello, World!\"，实际: " + result.stdout_output).c_str());
    }

    std::printf("  资源用量: CPU=%ldus 内存峰值=%ldB 墙钟=%ldus\n",
                (long)result.cpu_time_us,
                (long)result.memory_peak_bytes,
                (long)result.wall_time_us);

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
