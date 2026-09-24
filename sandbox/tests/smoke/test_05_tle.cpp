// Wave 1 · 测试 05：TLE（死循环超时）
// 验收：while(1) 死循环，time_limit_ms=1000，应在 1.2 秒内被 SIGKILL，状态 = TLE
//
// 面试知识点：
// - 墙钟超时：父进程用 steady_clock 计时，超过 time_limit_ms * 1.2 后 kill(-pgid, SIGKILL)
// - 杀整个进程组（负 pid），防止 fork 出的子进程逃逸
// - TLE 和 MLE 的区分：oom_kill=0 且被 SIGKILL → TLE；oom_kill>0 → MLE
// - RLIMIT_CPU 是兜底（CPU 时间超限），但墙钟超时更直接
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
    std::printf("== 测试 05：TLE（死循环，限 1 秒）==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_05_tle\n");
        return 2;
    }

    // 1. 写一个死循环程序
    const char* src_path = "/tmp/w1_tle.cpp";
    const char* bin_path = "/tmp/w1_tle";
    {
        std::ofstream f(src_path);
        f << "int main() {\n"
          << "    while (1) {}\n"  // 纯 CPU 死循环
          << "    return 0;\n"
          << "}\n";
    }

    // 2. 动态编译
    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        bad("编译 TLE 测试程序失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("TLE 测试程序编译成功");
    // 3. 在沙箱里运行：限 1 秒
    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin_path;
    cfg.input = "";
    cfg.time_limit_ms = 1000;     // 限 1 秒
    cfg.memory_limit_mb = 64;
    cfg.pids_limit = 16;
    cfg.use_rootfs = true;

    auto t0 = std::chrono::steady_clock::now();
    sandbox::JudgeResult result = sandbox::run(cfg);
    auto t1 = std::chrono::steady_clock::now();
    double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 4. 断言
    if (result.status == sandbox::Status::TLE) {
        ok("沙箱运行状态 = TLE（死循环被超时杀死）");
    } else {
        bad(("沙箱运行状态 != TLE，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.stderr_output.empty())
            std::printf("  stderr: %s\n", result.stderr_output.c_str());
    }

    if (result.signal == 9) {
        ok("进程被 SIGKILL(9) 杀死");
    } else {
        bad(("signal != 9，实际: " + std::to_string(result.signal)).c_str());
    }

    // 墙钟时间应该在 1.0s ~ 3.0s 之间（1.2s 超时 + 清理开销）
    if (wall_ms > 1000 && wall_ms < 3000) {
        ok(("墙钟时间 " + std::to_string((int)wall_ms) + "ms（1.0~3.0s 范围内）").c_str());
    } else {
        bad(("墙钟时间异常: " + std::to_string((int)wall_ms) + "ms").c_str());
    }
    // CPU 计量非零：TLE 场景下进程被 SIGKILL，cgroup 的 cpu.stat 可能尚未结算，
    // 此时由 wait4 的 rusage 兜底补上。这里只断言"最终上报的 CPU 时间非零"，
    // 不断言它来自哪条路——两条路任一失效都会让这里变红。
    if(result.cpu_time_us > 0){
        ok(("cpu_time_us = " + std::to_string(result.cpu_time_us) + " > 0（CPU 计量非零）").c_str());
    } else{
        bad("cpu_time_us = 0（CPU 计量失效：cgroup 读数与 rusage 兜底均未产出）");
    }
    std::printf("  [INFO] exit_code=%d signal=%d cpu_time_us=%ld wall_ms=%.0f\n",
                result.exit_code, result.signal, result.cpu_time_us, wall_ms);

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
