// Wave 0 · 任务 0.5：验证 pivot_root + umount 旧根
// 验收：切根并卸载旧根后，ls / 只看到我们的 tmpfs 内容，看不到宿主根
// RED 信号：ls / 仍能看到宿主根目录树（漏了 umount2）
//
// ★ 安全措施：先 unshare(CLONE_NEWNS) 建独立 mount namespace，
//   否则 MS_PRIVATE 与 pivot_root 会影响 VM 的整个挂载视图。
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <dirent.h>
#include <sched.h>
#include <string>
#include <set>

static int g_pass = 0, g_fail = 0;
static void ok(const char* m)  { std::printf("  [PASS] %s\n", m); ++g_pass; }
static void bad(const char* m) { std::printf("  [FAIL] %s\n", m); ++g_fail; }

int main() {
    std::printf("== 任务 0.5：pivot_root + umount 旧根 ==\n");

    if (geteuid() != 0) { std::printf("需要 root：sudo ./test_pivot_root\n"); return 2; }

    // 0) 独立 mount namespace —— 保护 VM 自身的挂载视图
    if (unshare(CLONE_NEWNS) != 0) {
        bad("unshare(CLONE_NEWNS) 失败"); return 2;
    }
    ok("已进入独立 mount namespace（宿主挂载视图不受影响）");

    // 1) 关闭挂载传播（否则沙箱内的挂载会泄漏到宿主）
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        bad("设置 MS_PRIVATE 失败"); return 2;
    }
    ok("MS_PRIVATE 设置成功（挂载事件不外泄）");

    // 2) 建 tmpfs 作为新根
    char sbdir[] = "/tmp/sb-XXXXXX";
    if (!mkdtemp(sbdir)) { bad("mkdtemp 失败"); return 2; }
    if (mount("tmpfs", sbdir, "tmpfs", 0, "size=256m,mode=0700,nodev,noexec,nosuid") != 0) {
        bad("挂载 tmpfs 失败"); return 2;
    }
    std::printf("  tmpfs 挂载于 %s\n", sbdir);
    ok("tmpfs 挂载成功");

    // 3) 在 tmpfs 内放标记目录
    std::string base(sbdir);
    std::string marker = base + "/sandbox-marker";
    mkdir(marker.c_str(), 0700);
    mkdir((base + "/bin").c_str(), 0700);

    // 4) pivot_root：把 "." 同时作为新根与旧根挂载点
    if (chdir(sbdir) != 0) { bad("chdir 到 tmpfs 失败"); return 2; }
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        std::printf("  pivot_root 失败：%s\n", std::strerror(errno));
        bad("pivot_root 失败"); return 2;
    }
    ok("pivot_root 成功");

    // 5) 卸载旧根（关键！不卸载则旧根仍可达）
    if (umount2(".", MNT_DETACH) != 0) {
        bad("umount2(MNT_DETACH) 失败——旧根仍挂载着，可被逃逸");
    } else {
        ok("旧根已卸载（umount2 MNT_DETACH）");
    }

    if (chdir("/") != 0) { bad("chdir(/) 失败"); return 2; }

    // 6) 枚举新的 "/" 并判定
    std::set<std::string> entries;
    DIR* d = opendir("/");
    if (!d) { bad("opendir(/) 失败"); return 2; }
    while (struct dirent* e = readdir(d)) entries.insert(e->d_name);
    closedir(d);

    std::printf("  / 下内容：");
    for (const auto& s : entries) std::printf("%s ", s.c_str());
    std::printf("\n");

    if (entries.count("sandbox-marker")) {
        ok("/ 中看到 sandbox-marker（新根已生效）");
    } else {
        bad("/ 中看不到 sandbox-marker");
    }

    // 宿主根的特征目录不应出现
    const char* host_markers[] = {"etc", "home", "usr", "var", "root", "boot", "opt"};
    int leaked = 0;
    for (const char* h : host_markers) {
        if (entries.count(h)) { std::printf("  泄漏宿主目录项：%s\n", h); ++leaked; }
    }
    if (leaked == 0) {
        ok("未出现宿主根的特征目录（隔离成功）");
    } else {
        bad("出现宿主根目录项——隔离失败（检查 umount2 是否执行）");
    }

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
