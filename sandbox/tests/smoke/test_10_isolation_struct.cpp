// Wave 1 · 测试 10：隔离结构断言（验收表第 1/2/3/4/5 条）
//
// 覆盖的验收项（原 16 条表中均标注 ⚠ 未覆盖，只在实现里无断言）：
//   1 | mount namespace | 沙箱内 cat /proc/self/mountinfo | 无宿主挂载
//   2 | pivot_root       | 沙箱内 ls /                    | 只有 tmpfs 目录
//   3 | umount 旧根      | mountinfo 无宿主挂载树         | 旧根挂载已卸载
//   4 | MS_PRIVATE       | 宿主 mountinfo                 | 无 tmpfs 事件
//   5 | cgroup 迁移      | 子进程启动后读 cgroup.procs     | PID 在目标 cgroup
//
// 面试知识点：
// - 第 1~3 条为什么必须让**用户程序自己**报：pivot_root 之后父进程看不到沙箱内视图，
//   只有 exec 进去的程序读 /proc/self/mountinfo 才是"沙箱内视角"。
//   父进程读自己的 mountinfo 只能看到 mount namespace 没克隆——那是另一回事。
// - 第 4 条必须由**父进程**（宿主任一进程）读：MS_PRIVATE 的作用正是"沙箱内的挂载
//   不传播到宿主"，所以宿主看不到任何 tmps/proc 变更才是通过。这一条和 1~3 条方向相反。
// - 第 5 条是竞态防线：如果 cgroup 迁移失败，用户程序的资源限制全部失效
//   （RLIMIT_CPU 只在子进程设，cgroup 侧 memory.max/pids.max 靠迁移生效）。
//
// 为什么合成一个测试而不是 5 个：
// 这 5 条共享同一次 sandbox::run() 调用和同一个用户程序，拆开会让每个测试都重跑
// 一遍 pivot_root + 4 个 bind mount（约 10ms × 5）。且它们的证据来自同一份 mountinfo，
// 分开读反而容易各取一半、互相矛盾。
#include "sandbox.h"
#include "cgroup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;
static void ok(const char* m)  { std::printf("  [PASS] %s\n", m); ++g_pass; }
static void bad(const char* m) { std::printf("  [FAIL] %s\n", m); ++g_fail; }

int main() {
    std::printf("== 测试 10：隔离结构（验收表第 1/2/3/4/5 条）==\n");

    if (geteuid() != 0) {
        std::printf("需要 root：sudo ./test_10_isolation_struct\n");
        return 2;
    }

    // ============================================================
    // 宿主基线：跑沙箱前先抓一份 mountinfo 的快照
    // 第 4 条要靠前后对比判定——没有 before 就无从判断"有没有泄漏"
    // ============================================================
    auto read_all = [](const std::string& path) {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    std::string host_mount_before = read_all("/proc/self/mountinfo");

    // ============================================================
    // 用户程序：在沙箱内自报可观测事实
    // ⚠ 纯 C stdio，不用 <fstream>/<sstream>/<string>——iostream 的 locale/静态
    //   初始化会走 futex，在沙箱里会抛 "The futex facility returned an unexpected
    //    error code" 直接崩（实测）。read/opendir 是 syscall 直路，不碰 futex。
    // ============================================================
    const char* src_path = "/tmp/w1_iso_struct.cpp";
    const char* bin_path = "/tmp/w1_iso_struct";
    {
        std::ofstream f(src_path);
        f << "#include <stdio.h>\n"
          << "#include <string.h>\n"
          << "#include <errno.h>\n"
          << "#include <dirent.h>\n"
          << "#include <unistd.h>\n"
          << "#include <fcntl.h>\n"
          << "int main() {\n"
          // --- 第 2 条：沙箱内 ls / ---
          << "    printf(\"ROOT_BEGIN\\n\");\n"
          << "    if (DIR* d = opendir(\"/\")) {\n"
          << "        struct dirent* e;\n"
          << "        while ((e = readdir(d)) != nullptr) {\n"
          << "            if (strcmp(e->d_name, \".\") == 0 || strcmp(e->d_name, \"..\") == 0) continue;\n"
          << "            printf(\"ROOT_ENTRY %s\\n\", e->d_name);\n"
          << "        }\n"
          << "        closedir(d);\n"
          << "    }\n"
          << "    printf(\"ROOT_END\\n\");\n"
          // --- 第 1 条：沙箱内 mountinfo（完整转储，父进程解析）---
          << "    printf(\"MOUNTINFO_BEGIN\\n\");\n"
          << "    int fd = open(\"/proc/self/mountinfo\", O_RDONLY);\n"
          << "    if (fd >= 0) {\n"
          << "        char buf[4096]; ssize_t n;\n"
          << "        while ((n = read(fd, buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)n, stdout);\n"
          << "        close(fd);\n"
          << "    }\n"
          << "    printf(\"MOUNTINFO_END\\n\");\n"
          // --- 附带：pidns 生效的话 hostname 应是空/新值 ---
          << "    printf(\"HOSTNAME_FILE \");\n"
          << "    int hf = open(\"/proc/sys/kernel/hostname\", O_RDONLY);\n"
          << "    if (hf >= 0) {\n"
          << "        char b[256]; ssize_t n2 = read(hf, b, sizeof(b) - 1);\n"
          << "        if (n2 > 0) { b[n2] = 0; printf(\"%s\", b); }\n"
          << "        close(hf);\n"
          << "    }\n"
          << "    printf(\"\\n\");\n"
          << "    fflush(stdout);\n"
          << "    return 0;\n"
          << "}\n";
    }

    std::string cmd = std::string("g++ -O2 -o ") + bin_path + " " + src_path + " 2>&1";
    if (system(cmd.c_str()) != 0) {
        bad("编译隔离结构测试程序失败");
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("隔离结构测试程序编译成功");

    // ============================================================
    // 跑沙箱
    // ============================================================
    sandbox::SandboxConfig cfg;
    cfg.binary_path = bin_path;
    cfg.input = "";
    cfg.time_limit_ms = 3000;
    cfg.memory_limit_mb = 128;
    cfg.pids_limit = 16;
    cfg.output_limit_mb = 8;      // mountinfo 可能几十 KB，8MB 足够且不会误触 OLE
    cfg.use_rootfs = true;        // 关键：必须 true，否则测的是没 pivot_root 的骨架

    sandbox::JudgeResult result = sandbox::run(cfg);
    const std::string& out = result.stdout_output;

    if (result.status != sandbox::Status::OK) {
        bad(("沙箱运行状态 != OK，实际: " + std::string(sandbox::status_to_string(result.status))).c_str());
        if (!result.stderr_output.empty()) std::printf("  stderr: %s\n", result.stderr_output.c_str());
        std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
        return 1;
    }
    ok("沙箱运行状态 = OK（用户程序完整执行）");

    // 提取 mountinfo 段
    std::string mi;
    {
        size_t b = out.find("MOUNTINFO_BEGIN\n");
        size_t e = out.find("MOUNTINFO_END");
        if (b != std::string::npos && e != std::string::npos && e > b) {
            mi = out.substr(b + 16, e - b - 16);
        }
    }
    // 提取 ROOT_ENTRY 列表
    std::vector<std::string> root_entries;
    {
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.rfind("ROOT_ENTRY ", 0) == 0) root_entries.push_back(line.substr(11));
        }
    }
    // 旧根是否仍挂载在沙箱内 mountinfo 中（第 3 条判定）
    // 判决实验证据（2026-09-30 实测）：把 rootfs.cpp 的 umount2 禁用后，
    // 宿主挂载树（/dev /run /sys /snap /mnt 等）全量出现在沙箱内 mountinfo；
    // 正常情况只有 5 行（tmpfs + 3 个 bind + /proc）。
    // 因此判别特征 = "挂载点字段命中 rootfs setup 从不创建的宿主一级目录"。
    auto oldroot_mounted = [&]() {
        std::istringstream iss(mi);
        std::string line;
        while (std::getline(iss, line)) {
            // mountinfo 行格式：id parent maj:min root mountpoint opts - fstype src ...
            // 第 5 字段 = 挂载点（实测：3457 3333 0:78 / / rw,... - tmpfs tmpfs ...）
            std::istringstream ls(line);
            std::string f;
            std::vector<std::string> fields;
            while (ls >> f) fields.push_back(f);
            if (fields.size() < 5) continue;
            const std::string& mp = fields[4];
            // 宿主挂载树特征：rootfs setup 从不创建这些路径
            // （/proc 排除：沙箱自己合法挂载了 /proc，会和宿主的 /proc 混淆）
            static const char* oldroot_marks[] = {
                "/dev", "/run", "/sys", "/snap", "/mnt", nullptr
            };
            for (int i = 0; oldroot_marks[i]; ++i) {
                const std::string mark = oldroot_marks[i];
                if (mp == mark || mp.rfind(mark + "/", 0) == 0)
                    return true;  // 命中 → 旧根挂载仍在沙箱内
            }
        }
        return false;
    };

    // ============================================================
    // 第 2 条：pivot_root —— 根目录只有 tmpfs 体系
    // 判定：根下不应出现宿主才有的一级目录（etc/home/var/root/srv/opt）
    //      （bin lib lib64 是我们自己建的，usr 是 bind mount 库目录，proc 是 pidns 的，都算合法）
    // ============================================================
    {
        static const char* host_only[] = {
            "etc", "home", "var", "root", "srv", "opt", "boot", "media", "mnt", nullptr
        };
        std::string leaked;
        for (const auto& e : root_entries) {
            for (int i = 0; host_only[i]; ++i) {
                if (e == host_only[i]) { leaked += e; leaked += " "; }
            }
        }
        if (leaked.empty()) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "【第2条 pivot_root】根目录只有 tmpfs 内容（%zu 项，无宿主目录泄漏）",
                          root_entries.size());
            ok(msg);
        } else {
            bad(("【第2条 pivot_root】根目录出现宿主目录: " + leaked).c_str());
        }
    }

    // ============================================================
    // 第 3 条：umount 旧根 —— 旧根挂载不得出现在沙箱内 mountinfo
    // 判别：mountinfo 挂载点命中宿主一级目录（/dev /run /sys /snap /mnt）即旧根未卸载
    // 反证：把 rootfs.cpp 的 umount2 改 return 0 → 本条断言变红（判决实验已验证）
    // ============================================================
    if (!oldroot_mounted()) {
        ok("【第3条 umount 旧根】旧根挂载不在沙箱内 mountinfo（已卸载）");
    } else {
        bad("【第3条 umount 旧根】旧根挂载仍在沙箱内 mountinfo（MNT_DETACH 未生效）");
    }

    // ============================================================
    // 第 1 条：mount namespace —— 沙箱内看不到宿主挂载
    // 判定：mountinfo 里不能出现宿主的挂载点（/etc/hosts、/sys、/home 等）。
    //      注意：不检查 /dev/sd* —— 3 个 bind mount 的源设备就是宿主的 /dev/sda1，
    //      源设备必然出现（bind 内容本就来自宿主 lib），它是设计不是泄漏。
    //      真正的泄漏特征是"宿主的挂载点路径出现在沙箱内"（旧根未卸载等），由第 3 条兜底。
    //      （我们只 bind 了 3 个 lib 目录 + tmpfs 自身）
    // ============================================================
    if (mi.empty()) {
        bad("【第1条 mount namespace】未取到沙箱内 mountinfo（用户程序输出缺失）");
    } else {
        static const char* host_marks[] = {
            "/etc/hostname", "/etc/resolv.conf", "/etc/hosts",
            "/sys/", "/proc/sys/fs/binfmt", "/home/", nullptr
        };
        std::string leaked;
        for (int i = 0; host_marks[i]; ++i) {
            if (mi.find(host_marks[i]) != std::string::npos) { leaked += host_marks[i]; leaked += " "; }
        }
        if (leaked.empty()) {
            ok("【第1条 mount namespace】沙箱内无宿主挂载点");
        } else {
            bad(("【第1条 mount namespace】沙箱内仍见宿主挂载: " + leaked).c_str());
        }
    }

    // ============================================================
    // 第 4 条：MS_PRIVATE —— 宿主 mountinfo 无沙箱事件
    // 判定：跑完后宿主的挂载表与跑之前**完全一致**
    //      （沙箱内的 tmpfs + bind mount 一次都没传播出来）
    // ============================================================
    {
        std::string host_mount_after = read_all("/proc/self/mountinfo");
        if (host_mount_after == host_mount_before) {
            ok("【第4条 MS_PRIVATE】宿主 mountinfo 无变化（沙箱挂载未泄漏）");
        } else {
            // 说出差在哪——直接判定"有变化"对人没用
            bad("【第4条 MS_PRIVATE】宿主 mountinfo 发生变化（挂载传播泄漏）");
            std::printf("  [INFO] before=%zu bytes, after=%zu bytes\n",
                        host_mount_before.size(), host_mount_after.size());
        }
    }

    // ============================================================
    // 第 5 条：cgroup 迁移 —— 沙箱运行期间 PID 在目标 cgroup
    // 这一条沙箱跑完就查不到了（run() 内部会 cleanup），
    // 所以独立复现一次：建 cgroup、迁一个子进程、从 cgroup.procs 读回来。
    // ============================================================
    {
        std::string cg = sandbox::cgroup::create(128, 8);
        if (cg.empty()) {
            bad("【第5条 cgroup 迁移】cgroup::create 失败（无法构造测试环境）");
        } else {
            // 子进程只负责"待命"，父进程把它迁进 cgroup 再从 cgroup.procs 读
            int syncp[2];
            if (pipe(syncp) != 0) {
                bad("【第5条 cgroup 迁移】pipe 失败");
                sandbox::cgroup::cleanup(cg);
            } else {
                pid_t pid = fork();
                if (pid < 0) {
                    bad("【第5条 cgroup 迁移】fork 失败");
                    sandbox::cgroup::cleanup(cg);
                } else if (pid == 0) {
                    // 子进程：阻塞等父进程信号后退出
                    char c;
                    while (read(syncp[0], &c, 1) == 1) { if (c == 'x') break; }
                    close(syncp[0]);
                    _exit(0);
                } else {
                    close(syncp[0]);
                    // 迁移（runner 用的同一个函数）
                    bool migrated = sandbox::cgroup::migrate_pid(pid, cg);
                    if (!migrated) {
                        bad(("【第5条 cgroup 迁移】migrate_pid 失败: " + std::string(std::strerror(errno))).c_str());
                    } else {
                        // 从 cgroup.procs 读回来，必须包含 pid
                        std::string procs = read_all(cg + "/cgroup.procs");
                        bool found = false;
                        std::istringstream iss(procs);
                        int p;
                        while (iss >> p) { if (p == pid) found = true; }
                        if (found) {
                            ok("【第5条 cgroup 迁移】PID 在目标 cgroup 的 cgroup.procs 中");
                        } else {
                            bad("【第5条 cgroup 迁移】迁移后 cgroup.procs 中找不到该 PID");
                            std::printf("  [INFO] cgroup.procs = [%s]\n", procs.c_str());
                        }
                    }
                    // 放行并回收子进程
                    char x = 'x';
                    ssize_t wr = write(syncp[1], &x, 1);
                    (void)wr;
                    close(syncp[1]);
                    int st;
                    waitpid(pid, &st, 0);
                }
            }
        }
        sandbox::cgroup::cleanup(cg);
    }

    // 观测信息
    std::printf("  [INFO] 根目录条目数=%zu, mountinfo 长度=%zu bytes\n",
                root_entries.size(), mi.size());
    if (!root_entries.empty()) {
        std::string list;
        for (const auto& e : root_entries) { list += e; list += " "; }
        std::printf("  [INFO] 根目录: %s\n", list.c_str());
    }

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
