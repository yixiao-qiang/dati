// Wave 1 · 测试 03：文件系统隔离 —— /etc/passwd 在沙箱内不可见
// 验收：open("/etc/passwd") 返回 -1 且 errno=ENOENT(2)
//
// 面试知识点：
// - pivot_root + 最小化挂载 = 沙箱里根本没有 /etc，openat 直接 ENOENT
// - seccomp 不解析文件路径字符串，路径级隔离靠文件系统层面实现
// - 这验证的是"隔离有效性"：沙箱漏了 /etc，用户程序看不到宿主敏感文件
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
    std::printf("== 测试 03：文件系统隔离（/etc/passwd 不可见）==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_03_fs_isolation\n");
        return 2;
    }

    // 1. 写一个尝试读 /etc/passwd 的测试程序
    const char* src_path = "/tmp/w1_fs_iso.cpp";
    const char* bin_path = "/tmp/w1_fs_iso";
    {
        std::ofstream f(src_path);
        f << "#include <fcntl.h>\n"
          << "#include <unistd.h>\n"
          << "#include <cstdio>\n"
          << "#include <cerrno>\n"
          << "#include <cstring>\n"
          << "int main() {\n"
          << "    int fd = open(\"/etc/passwd\", O_RDONLY);\n"
          << "    printf(\"open(/etc/passwd) returned %d, errno=%d (%s)\\n\", fd, errno, strerror(errno));\n"
          << "    // 预期：fd=-1, errno=ENOENT(2)，因为沙箱里没挂 /etc\n"
          << "    return (fd == -1 && errno == ENOENT) ? 0 : 1;\n"
          << "}\n";
    }

    // 2. 动态编译
    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        bad("编译 fs 隔离测试程序失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("fs 隔离测试程序编译成功");

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
        ok("沙箱运行状态 = OK（程序正确判断 ENOENT，退出码 0）");
    } else {
        bad(("沙箱运行状态 != OK，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.stderr_output.empty())
            std::printf("  stderr: %s\n", result.stderr_output.c_str());
    }

    if (result.stdout_output.find("errno=2") != std::string::npos) {
        ok("open(/etc/passwd) 返回 -1 且 errno=ENOENT(2)（沙箱里无 /etc）");
    } else {
        bad(("stdout 不包含 errno=2，实际: " + result.stdout_output).c_str());
    }

    if (result.stdout_output.find("No such file or directory") != std::string::npos) {
        ok("错误信息为 'No such file or directory'（ENOENT 对应描述）");
    } else {
        bad("stdout 不包含 'No such file or directory'");
    }

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
