// Wave 1 · 测试 07：OLE 的 stderr 路径（审查驱动新增）
// 验收：只写 stderr 的超限程序也必须判 OLE，而非靠墙钟兜底判 TLE
//
// 为什么单独一个测试：
//   test_06 的内层程序只 printf 到 stdout。而 runner 曾把 OLE 阈值判断
//   嵌套在 stdout 的 if(n>0) 分支里，导致只写 stderr 的程序（stdout 管道
//   长期 EAGAIN）完全逃过 OLE 判定 —— 跑满 time_limit 被判 TLE，
//   且 stderr_buf 无上限增长。该缺陷被 Superpowers reviewer 发现，
//   实测复现：status=TLE wall=3003ms；修复后 status=OLE wall=292ms。
//   本测试钉住这条路径，防止回退。
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
    std::printf("== 测试 07：OLE 的 stderr 路径（只写 stderr 也须判 OLE）==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_07_ole_stderr\n");
        return 2;
    }

    // 1. 写一个只写 stderr、不写 stdout 的超限程序
    //    必须全速写（无 sleep）：受阈值 1MB 限制，写慢了根本到不了 limit，
    //    那样测的是"没超限"而不是"超限未判"。这是复现用例的设计要点。
    const char* src_path = "/tmp/w1_ole_stderr.cpp";
    const char* bin_path = "/tmp/w1_ole_stderr";
    {
        std::ofstream f(src_path);
        f << "#include <cstdio>\n"
          << "#include <cstring>\n"
          << "int main() {\n"
          << "    char blk[4096];\n"
          << "    memset(blk, 66, sizeof(blk));\n"
          << "    for(;;){ fwrite(blk, 1, sizeof(blk), stderr); }\n"
          << "    return 0;\n"
          << "}\n";
    }

    // 2. 动态编译
    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        bad("编译 stderr 测试程序失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("stderr 测试程序编译成功");

    // 3. 沙箱运行：输出限 1MB，时间限 3 秒（给足时间，让"超时兜底"能现形）
    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin_path;
    cfg.input = "";
    cfg.time_limit_ms = 3000;
    cfg.output_limit_mb = 1;
    cfg.memory_limit_mb = 64;
    cfg.pids_limit = 16;
    cfg.use_rootfs = true;

    auto t0 = std::chrono::steady_clock::now();
    sandbox::JudgeResult result = sandbox::run(cfg);
    auto t1 = std::chrono::steady_clock::now();
    double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 4. 断言
    // 4.1 状态必须是 OLE。若退化成 TLE，说明 OLE 判定又漏了 stderr 路径
    if (result.status == sandbox::Status::OLE) {
        ok("stderr-only 超限被判 OLE（而非 TLE）");
    } else {
        bad(("stderr-only 超限未判 OLE，实际: " +
             std::string(sandbox::status_to_string(result.status))).c_str());
    }

    // 4.2 必须快速触发。跑满 time_limit(3000ms) 说明靠超时兜底
    if (wall_ms < 1000) {
        ok(("墙钟 " + std::to_string((int)wall_ms) + "ms（远短于 3 秒上限，非超时兜底）").c_str());
    } else {
        bad(("墙钟 " + std::to_string((int)wall_ms) + "ms 接近/超过上限，疑似靠超时兜底").c_str());
    }

    // 4.3 stdout 应为空（程序不写 stdout），stderr 应被截断在 limit 附近
    if (result.stdout_output.empty()) {
        ok("stdout 为空（程序只写 stderr，符合预期）");
    } else {
        bad(("stdout 非空，实际 " + std::to_string(result.stdout_output.size()) + " 字节").c_str());
    }
    if (result.stderr_output.size() <= (size_t)(cfg.output_limit_mb * 1024 * 1024 + 4096)) {
        ok(("stderr 已截断，实际 " + std::to_string(result.stderr_output.size()) + " 字节").c_str());
    } else {
        bad(("stderr 未截断，实际 " + std::to_string(result.stderr_output.size()) + " 字节").c_str());
    }

    std::printf("  [INFO] exit_code=%d signal=%d wall_ms=%.0f stdout=%zu stderr=%zu\n",
                result.exit_code, result.signal, wall_ms,
                result.stdout_output.size(), result.stderr_output.size());

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
