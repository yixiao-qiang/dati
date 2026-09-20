// Wave 1 · 测试 04：MLE（内存超限被 OOM Kill）
// 验收：分配 256MB 内存，沙箱限 64MB，应被 OOM Kill，状态 = MLE
//
// 面试知识点：
// - cgroup v2 memory.max 超过后，内核 OOM killer 杀进程
// - memory.events 里 oom_kill 计数 +1，这是区分 MLE 和 TLE 的关键
// - oom_kill > 0 → MLE；否则是父进程 SIGKILL → TLE
// - cgroup 杀的是整个进程组（OOM killer 挑 cgroup 里内存最大的进程杀）
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
    std::printf("== 测试 04：MLE（分配 256MB，限制 64MB）==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_04_mle\n");
        return 2;
    }

    // 1. 写一个分配大量内存的测试程序
    const char* src_path = "/tmp/w1_mle.cpp";
    const char* bin_path = "/tmp/w1_mle";
    {
        std::ofstream f(src_path);
        f << "#include <sys/mman.h>\n"
          << "#include <cstring>\n"
          << "#include <cstdio>\n"
          << "#include <cerrno>\n"
          << "#include <unistd.h>\n"
          << "int main() {\n"
          << "    // 一次性 mmap 256MB（glibc malloc 逐次分配会优雅返回 NULL，不触发 OOM）\n"
          << "    // mmap 大块匿名映射，memset 触碰所有页面时会逐页分配物理内存\n"
          << "    // 超过 cgroup memory.max 时，内核 OOM killer 直接杀进程\n"
          << "    const size_t SIZE = 256UL * 1024 * 1024;\n"
          << "    void* p = mmap(NULL, SIZE, PROT_READ|PROT_WRITE,\n"
          << "                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);\n"
          << "    if (p == MAP_FAILED) {\n"
          << "        printf(\"mmap failed: %s\\n\", strerror(errno));\n"
          << "        return 1;\n"
          << "    }\n"
          << "    // 逐页触碰（每页 4KB），每触一页分配一页物理内存\n"
          << "    for (size_t off = 0; off < SIZE; off += 4096) {\n"
          << "        ((char*)p)[off] = 1;\n"
          << "    }\n"
          << "    printf(\"这行不应该打印：没被 OOM，分配成功了\\n\");\n"
          << "    return 0;\n"
          << "}\n";
    }

    // 2. 动态编译
    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        bad("编译 MLE 测试程序失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("MLE 测试程序编译成功");

    // 3. 在沙箱里运行：限制 64MB，程序要分配 256MB
    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin_path;
    cfg.input = "";
    cfg.time_limit_ms = 5000;     // 给 5 秒，避免和 TLE 混淆
    cfg.memory_limit_mb = 64;     // 限 64MB
    cfg.pids_limit = 16;
    cfg.use_rootfs = true;

    sandbox::JudgeResult result = sandbox::run(cfg);

    // 4. 断言
    if (result.status == sandbox::Status::MLE) {
        ok("沙箱运行状态 = MLE（被 OOM Kill）");
    } else {
        bad(("沙箱运行状态 != MLE，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.stderr_output.empty())
            std::printf("  stderr: %s\n", result.stderr_output.c_str());
    }

    if (result.signal == 9) {
        ok("进程被 SIGKILL(9) 杀死（OOM killer 或父进程超时）");
    } else {
        bad(("signal != 9，实际: " + std::to_string(result.signal)).c_str());
    }

    // 注意：当前 runner.cpp 可能还没读 oom_kill，所以这条可能失败
    // 如果失败，说明需要在任务 1.4 里加 oom_kill 读取
    std::printf("  [INFO] exit_code=%d signal=%d cpu_time_us=%ld memory_peak=%ld\n",
                result.exit_code, result.signal, result.cpu_time_us, (long)result.memory_peak_bytes);

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
