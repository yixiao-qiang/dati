// Wave 1 · 测试 11：管道 EOF（验收表第 16 条）
// 验收：父进程关闭管道写端 → 子进程正常退出后读端收到 EOF，正常程序不会被误判 TLE
//
// 面试知识点：
// - 管道 EOF 语义：读端收到 EOF 的唯一条件 = 所有写端都关闭
// - 父进程 pipe() 后也持有写端 fd。若父进程不 close(out_pipe[1])，
//   子进程退出后写端引用计数仍为 1 → 读端永远等不到 EOF → 正常程序
//   被拖到墙钟超时，误判 TLE（计划 RED 信号：父进程没关写端 → 正常程序也 TLE）
// - 大输出用例（> 管道默认容量 64KB）：子进程写会阻塞，父进程必须持续 read；
//   子进程退出后 EOF 到达，且管道残留数据必须读完才能拿到完整 stdout
//   ——这一个用例同时验证"EOF 到达"和"读完残留数据"两条路径
//
// 为什么分两个用例：
// - 小输出：只验证"EOF 快速返回"。若父进程没关写端，正常程序被超时兜底
//   判 TLE，墙钟 ≈ time_limit，断言 3 变红
// - 大输出：验证"EOF 到达 + 数据完整性"。若父进程在子进程退出后没把
//   管道读空就 break，stdout 长度不足，断言 6 变红
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

// 编译一个被测程序，失败返回 false
static bool build(const char* src_path, const char* bin_path, const char* body) {
    {
        std::ofstream f(src_path);
        f << body;
    }
    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    return system(cmd.c_str()) == 0;
}

// ============================================================
// 用例 A：小输出——验证 EOF 快速返回
// ============================================================
static void test_small_output() {
    std::printf("-- A. 小输出（printf 后立即退出）--\n");

    const char* src = "/tmp/w1_eof_a.cpp";
    const char* bin = "/tmp/w1_eof_a";
    if (!build(src, bin,
               "#include <cstdio>\n"
               "int main() {\n"
               "    printf(\"Hello, EOF!\\n\");\n"
               "    return 0;\n"
               "}\n")) {
        bad("小输出程序编译失败");
        return;
    }
    ok("小输出程序编译成功");

    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin;
    cfg.input = "";
    cfg.time_limit_ms = 1000;      // 限 1 秒——正常程序不该用到
    cfg.memory_limit_mb = 64;
    cfg.pids_limit = 16;
    cfg.use_rootfs = true;

    auto t0 = std::chrono::steady_clock::now();
    sandbox::JudgeResult result = sandbox::run(cfg);
    auto t1 = std::chrono::steady_clock::now();
    double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 断言 A1：正常程序必须判 OK，不是 TLE
    // 反证：父进程没关写端 → 读端等不到 EOF → 靠超时兜底 → TLE
    if (result.status == sandbox::Status::OK) {
        ok("status = OK（正常程序未被误判）");
    } else {
        bad(("status != OK，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.stderr_output.empty())
            std::printf("  stderr: %s\n", result.stderr_output.c_str());
    }

    // 断言 A2：墙钟必须远小于 1 秒（EOF 快速返回，不是超时兜底）
    // 反证：没关写端 → 程序跑满 1000ms 才被判 TLE
    if (wall_ms < 500) {
        ok(("墙钟 " + std::to_string((int)wall_ms) + "ms < 500ms（EOF 快速返回）").c_str());
    } else {
        bad(("墙钟 " + std::to_string((int)wall_ms) + "ms >= 500ms（疑似超时兜底）").c_str());
    }

    std::printf("  [INFO] status=%s exit=%d signal=%d wall_ms=%.0f stdout=[%s]\n",
                sandbox::status_to_string(result.status), result.exit_code, result.signal,
                wall_ms, result.stdout_output.c_str());
}

// ============================================================
// 用例 B：大输出（128KB > 管道容量 64KB）——验证 EOF + 数据完整性
// ============================================================
static void test_large_output() {
    std::printf("-- B. 大输出（128KB > 管道容量 64KB）--\n");

    const char* src = "/tmp/w1_eof_b.cpp";
    const char* bin = "/tmp/w1_eof_b";
    if (!build(src, bin,
               "#include <cstdio>\n"
               "int main() {\n"
               "    for (int i = 0; i < 128 * 1024; i++) putchar('A');\n"
               "    return 0;\n"
               "}\n")) {
        bad("大输出程序编译失败");
        return;
    }
    ok("大输出程序编译成功");

    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin;
    cfg.input = "";
    cfg.time_limit_ms = 3000;      // 给足时间，数据量 128KB 不需要 3 秒
    cfg.memory_limit_mb = 64;
    cfg.pids_limit = 16;
    cfg.use_rootfs = true;

    sandbox::JudgeResult result = sandbox::run(cfg);

    // 断言 B1：输出 128KB 的正常程序必须判 OK（不是 OLE——没超限，也不是 TLE）
    if (result.status == sandbox::Status::OK) {
        ok("status = OK（128KB 输出正常完成）");
    } else {
        bad(("status != OK，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
    }

    // 断言 B2：stdout 必须完整收到 128KB
    // 反证：父进程在子进程退出后没把管道读空就 break → 数据丢失、长度不足
    const size_t EXPECT = 128 * 1024;
    if (result.stdout_output.size() == EXPECT) {
        ok(("stdout 完整 = " + std::to_string(EXPECT) + " 字节（EOF 后残留数据也读完了）").c_str());
    } else {
        bad(("stdout 长度 != " + std::to_string(EXPECT) + "，实际: " + std::to_string(result.stdout_output.size())).c_str());
    }

    std::printf("  [INFO] status=%s exit=%d signal=%d stdout_size=%zu\n",
                sandbox::status_to_string(result.status), result.exit_code, result.signal,
                result.stdout_output.size());
}

int main() {
    std::printf("== 测试 11：管道 EOF（验收表第 16 条）==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_11_pipe_eof\n");
        return 2;
    }

    test_small_output();
    test_large_output();

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
