// Wave 1 · 测试 09：pids.max fork 炸弹隔离
// 验收项：16 条验收表第 10 条「pids.max fork 炸弹」——曾「无测试、无断言」
//
// 面试知识点：
// - cgroup v2 pids.max 封顶进程数，超出后 fork() 返回 -1 且 errno=EAGAIN(11)，
//   不是 EPERM/ENOMEM——内核有意让这种失败"可预期、可恢复"。
// - fork 炸弹在沙箱里有**两道独立防线**，且它们先后触发：
//     防线1（seccomp）：白名单不含 fork/clone/vfork → fork() 返回 -1, errno=ENOSYS(38)
//     防线2（cgroup pids.max）：进程数封顶 → fork() 返回 -1, errno=EAGAIN(11)
//   **关键**：防线1 先于防线2 生效，所以走完整沙箱时永远看不到 pids.max 触发。
// - 因此要验证「pids.max 真的有效」，必须**绕过 seccomp 直接测 cgroup 机制**；
//   而"用户提交 fork 炸弹会怎样"则要走完整沙箱。本测试分 A/B 两部分分别覆盖。
//
// 为什么必须分开测（本测试的存在理由）：
// 曾把两部分揉成一条断言，结果只看到 errno=38 就绿灯——那测的是 seccomp，
// 不是验收表写的 pids.max。这正是 E4 型假绿："有绿灯，但绿灯的原因不对"。
#include "sandbox.h"
#include "cgroup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;
static void ok(const char* m)  { std::printf("  [PASS] %s\n", m); ++g_pass; }
static void bad(const char* m) { std::printf("  [FAIL] %s\n", m); ++g_fail; }

// ============================================================
// 部分 A：直接验证 cgroup pids.max 机制（**不经过 seccomp**）
// ============================================================
// 做法：自己建 cgroup、把自己迁进去、然后连着 fork。
// 这条路不碰沙箱，seccomp 不介入，能干净地观察到 pids.max 封顶。
static void test_pids_max_mechanism() {
    std::printf("-- A. cgroup pids.max 机制（不经 seccomp）--\n");

    if (geteuid() != 0) {
        std::printf("   跳过：需要 root\n");
        return;
    }

    // 限额取 5：本进程自己占 1 个，给 fork 留 4 个空间
    const int PIDS_LIMIT = 5;
    std::string cg = sandbox::cgroup::create(256, PIDS_LIMIT);
    if (cg.empty()) {
        bad("cgroup::create 失败（无法建 pids.max 测试环境）");
        return;
    }

    // 先确认 pids.max 真的写进去了（防"write_file 静默失败"）
    // 这防的是 E4 同型 bug：配了但没生效
    {
        std::ifstream f(cg + "/pids.max");
        std::string val;
        std::getline(f, val);
        if (val.find(std::to_string(PIDS_LIMIT)) != std::string::npos) {
            ok(("pids.max 已写入 = " + val).c_str());
        } else {
            bad(("pids.max 未写入或值不符，实际: [" + val + "]").c_str());
            sandbox::cgroup::cleanup(cg);
            return;
        }
    }

    if (!sandbox::cgroup::migrate_pid(getpid(), cg)) {
        bad("migrate_pid 失败（本进程未进 cgroup，测不了封顶）");
        sandbox::cgroup::cleanup(cg);
        return;
    }

    // 连续 fork，直到失败或达到保险丝
    int ok_count = 0;
    bool capped = false;
    int capped_errno = 0;
    for (int i = 0; i < 200; i++) {
        pid_t p = fork();
        if (p < 0) {
            capped = true;
            capped_errno = errno;
            break;
        }
        if (p == 0) {
            _exit(0);   // 子进程立即退出，不留僵尸也不占 stdout
        }
        ok_count++;
    }

    // 断言 A1：fork 会被封顶（不能无限 fork）
    if (capped) {
        ok("pids.max 生效：fork 被内核封顶");
        std::printf("   [INFO] 成功 fork %d 次后失败，errno=%d (%s)\n",
                    ok_count, capped_errno, std::strerror(capped_errno));
    } else {
        bad(("pids.max 未生效：连 fork 200 次都没被封顶（成功 " + std::to_string(ok_count) + " 次）").c_str());
    }

    // 断言 A2：失败的 errno 必须是 EAGAIN(11)
    // pids.max 的契约就是 EAGAIN。若看到 ENOSYS(38) 说明这台机器/这个环境是
    // seccomp 拦的（那本题就跑偏了，要报出来让人查）
    if (capped && capped_errno == EAGAIN) {
        ok("失败 errno = EAGAIN(11)（符合 pids.max 契约）");
    } else if (capped) {
        bad(("失败 errno 不符 pids.max 契约，实际 errno=" + std::to_string(capped_errno)).c_str());
    }

    // 断言 A3：封顶点接近限额（PIDS_LIMIT-1，因为本进程自己占 1 个）
    // 这一条防"pids.max 写了个永远触不到的巨大值"——比如把限额写成了 pid 数上限
    // 而不是进程数，实测会表现为 ok_count 远大于 PIDS_LIMIT
    if (capped && ok_count <= PIDS_LIMIT) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "封顶点 %d <= 限额 %d（限额真的在约束进程数）",
                      ok_count, PIDS_LIMIT);
        ok(msg);
    } else if (capped) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "封顶点 %d > 限额 %d（pids.max 未正确约束）",
                      ok_count, PIDS_LIMIT);
        bad(msg);
    }

    // 回收所有子进程
    while (waitpid(-1, nullptr, 0) > 0) {}
    sandbox::cgroup::cleanup(cg);
}

// ============================================================
// 部分 B：端到端——用户提交 fork 炸弹，沙箱围堵效果
// ============================================================
static void test_fork_bomb_end_to_end() {
    std::printf("-- B. 端到端 fork 炸弹围堵（完整沙箱）--\n");

    if (geteuid() != 0) {
        std::printf("   跳过：需要 root\n");
        return;
    }

    const char* src_path = "/tmp/w1_pids.cpp";
    const char* bin_path = "/tmp/w1_pids";
    {
        std::ofstream f(src_path);
        f << "#include <unistd.h>\n"
          << "#include <sys/wait.h>\n"
          << "#include <cstdio>\n"
          << "#include <cerrno>\n"
          << "#include <cstring>\n"
          << "int main() {\n"
          << "    int ok_count = 0;\n"
          << "    for (int i = 0; i < 500; i++) {\n"   // 500 是保险丝：防线全失效也要能退
          << "        pid_t p = fork();\n"
          << "        if (p < 0) {\n"
          << "            printf(\"fork_failed_at=%d errno=%d (%s)\\n\",\n"
          << "                   ok_count, errno, strerror(errno));\n"
          << "            fflush(stdout);\n"
          << "            while (waitpid(-1, nullptr, WNOHANG) > 0) {}\n"
          << "            return 0;\n"
          << "        }\n"
          << "        if (p == 0) { _exit(0); }\n"
          << "        ok_count++;\n"
          << "    }\n"
          << "    printf(\"no_limit_reached forks=%d\\n\", ok_count);\n"  // 防线全失效的信号
          << "    fflush(stdout);\n"
          << "    while (waitpid(-1, nullptr, WNOHANG) > 0) {}\n"
          << "    return 0;\n"
          << "}\n";
    }

    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    if (system(cmd.c_str()) != 0) {
        bad("编译 fork 炸弹测试程序失败");
        return;
    }

    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin_path;
    cfg.input = "";
    cfg.time_limit_ms = 3000;      // 3 秒墙钟：若围堵失效，这里会现形
    cfg.memory_limit_mb = 256;
    cfg.pids_limit = 16;
    cfg.output_limit_mb = 8;
    cfg.use_rootfs = true;

    sandbox::JudgeResult result = sandbox::run(cfg);
    const std::string& out = result.stdout_output;

    // 断言 B1：围堵有效——程序没能无限 fork
    bool fork_failed = out.find("fork_failed_at=") != std::string::npos;
    bool no_limit    = out.find("no_limit_reached") != std::string::npos;
    bool killed      = (result.signal != 0);

    if (no_limit) {
        bad("fork 完全没被拦住（no_limit_reached，两道防线都失效）");
    } else if (fork_failed) {
        ok("fork 炸弹被拦住（程序观察到 fork 失败）");
        std::printf("   [INFO] %s\n", out.substr(0, out.find('\n')).c_str());
    } else if (killed) {
        ok("fork 炸弹进程被终止（隔离在系统调用层起作用）");
    } else {
        bad(("围堵形态无法判定: stdout=[" + out + "] signal=" + std::to_string(result.signal)).c_str());
    }

    // 断言 B2：判定状态可解释
    if (result.status == sandbox::Status::OK ||
        result.status == sandbox::Status::NONZERO_EXIT ||
        result.status == sandbox::Status::TLE ||
        result.status == sandbox::Status::MLE) {
        ok(("判定状态可解释：" + std::string(sandbox::status_to_string(result.status))).c_str());
    } else {
        bad(("判定状态不可解释: " + std::string(sandbox::status_to_string(result.status))).c_str());
    }

    // 断言 B3：墙钟快速返回——恶意提交不占判题槽位
    double wall_ms = result.wall_time_us / 1000.0;
    if (wall_ms < 2000) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "墙钟 %.0fms < 2000ms（未被拖住）", wall_ms);
        ok(msg);
    } else {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "墙钟 %.0fms >= 2000ms（判题机被拖住）", wall_ms);
        bad(msg);
    }

    std::printf("   [INFO] status=%s exit=%d signal=%d wall_ms=%.0f cpu_us=%ld\n",
                sandbox::status_to_string(result.status), result.exit_code, result.signal,
                wall_ms, (long)result.cpu_time_us);
}

int main() {
    std::printf("== 测试 09：pids.max fork 炸弹隔离 ==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_09_pids\n");
        return 2;
    }

    // A 先跑：它测的是 pids.max 机制本身，是验收表第 10 条的正面回答
    test_pids_max_mechanism();

    // B 后跑：测用户可见行为
    test_fork_bomb_end_to_end();

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
