// Wave 1 · 测试 06：OLE（Output Limit Exceeded）
// 验收：程序疯狂输出，output_limit_mb=1，应在短时间内判 OLE
//
// 面试知识点：
// - 输出限制是判题正确性：用户狂 printf 1GB，必须判 OLE 而不是让父进程内存爆炸
// - 关键区分：OLE 应该很快触发（输出 1MB 就停），不是跑满 time_limit 才判
// - OLE 后继续 read drain 管道，防止子进程 write 阻塞死锁
#include "sandbox.h"

#include <chrono>
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
    std::printf("== 测试 06：OLE（疯狂输出，限 1MB）==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_06_ole\n");
        return 2;
    }

    // 1. 写一个疯狂输出的测试程序（被沙箱运行的内层程序）
    const char* src_path = "/tmp/w1_ole.cpp";
    const char* bin_path = "/tmp/w1_ole";
    {
        std::ofstream f(src_path);
        f << "#include <cstdio>\n"
          << "int main() {\n"
          // while(1) 死循环狂 printf，每次输出一大块（150 字节）
          << "    while(1) printf(\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"\n"
          << "                       \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"\n"
          << "                       \"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\");\n"
          << "    return 0;\n"
          << "}\n";
    }

    // 2. 动态编译
    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        bad("编译 OLE 测试程序失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("OLE 测试程序编译成功");

    // 3. 在沙箱里运行：输出限 1MB，时间限 1 秒
    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin_path;
    cfg.input = "";
    cfg.time_limit_ms = 1000;      // 时间上限 1 秒
    cfg.output_limit_mb = 1;      // ★ 输出限 1MB（关键：小于程序输出量）
    cfg.memory_limit_mb = 64;
    cfg.pids_limit = 16;
    cfg.use_rootfs = true;

    auto t0 = std::chrono::steady_clock::now();
    sandbox::JudgeResult result = sandbox::run(cfg);
    auto t1 = std::chrono::steady_clock::now();
    double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 4. 断言
    if (result.status == sandbox::Status::OLE) {
        ok("沙箱运行状态 = OLE（输出超限被识别）");
    } else {
        bad(("沙箱运行状态 != OLE，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.stderr_output.empty())
            std::printf("  stderr: %s\n", result.stderr_output.c_str());
    }

    // 关键：OLE 应该很快触发，不应该跑满 time_limit_ms
    // 输出 1MB 很快（毫秒级），如果跑了 1 秒才结束，说明 OLE 没生效，是超时兜底的
    if (wall_ms < cfg.time_limit_ms) {
        ok(("墙钟时间 " + std::to_string((int)wall_ms) + "ms（短于 1 秒，OLE 快速触发）").c_str());
    } else {
        bad(("墙钟 " + std::to_string((int)wall_ms) + "ms >= 1000ms，OLE 没生效，靠超时兜底").c_str());
    }

    // stdout_output 应该被截断在 1MB 左右（不超过 output_limit_mb + 缓冲块大小）
    if (result.stdout_output.size() <= (size_t)(cfg.output_limit_mb * 1024 * 1024 + 4096)) {
        ok(("stdout 已截断，实际 " + std::to_string(result.stdout_output.size()) + " 字节").c_str());
    } else {
        bad(("stdout 未截断，实际 " + std::to_string(result.stdout_output.size()) + " 字节").c_str());
    }

    std::printf("  [INFO] exit_code=%d signal=%d wall_ms=%.0f stdout_size=%zu stderr_size=%zu\n",
                result.exit_code, result.signal, wall_ms,
                result.stdout_output.size(), result.stderr_output.size());

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
