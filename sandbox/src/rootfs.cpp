// Wave 1 · 根文件系统管理实现
#include "rootfs.h"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>

namespace sandbox {
namespace rootfs {

// 复制文件（简单实现，骨架阶段用）
static bool copy_file(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary);
    if (!out) return false;
    out << in.rdbuf();
    // 加可执行权限
    chmod(dst.c_str(), 0755);
    return out.good();
}

int setup(const std::string& binary_path) {
    // 1. 关闭挂载传播（MS_PRIVATE），防止沙箱内挂载泄漏到宿主
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        std::fprintf(stderr, "rootfs: MS_PRIVATE 失败: %s\n", std::strerror(errno));
        return -1;
    }

    // 2. 创建临时目录作为 tmpfs 挂载点
    char tmpl[] = "/tmp/sb-rootfs-XXXXXX";
    if (!mkdtemp(tmpl)) {
        std::fprintf(stderr, "rootfs: mkdtemp 失败: %s\n", std::strerror(errno));
        return -1;
    }
    std::string new_root(tmpl);

    // 3. 挂载 tmpfs（nodev/noexec/nosuid 必须放 mountflags，不能放 data）
    if (mount("tmpfs", new_root.c_str(), "tmpfs",
              MS_NODEV | MS_NOEXEC | MS_NOSUID,
              "size=64m,mode=0700") != 0) {
        std::fprintf(stderr, "rootfs: mount tmpfs 失败: %s\n", std::strerror(errno));
        rmdir(new_root.c_str());
        return -1;
    }

    // 4. 在 tmpfs 里创建 bin 目录，复制二进制
    std::string bin_dir = new_root + "/bin";
    mkdir(bin_dir.c_str(), 0755);
    std::string prog_path = bin_dir + "/prog";
    if (!copy_file(binary_path, prog_path)) {
        std::fprintf(stderr, "rootfs: 复制二进制失败\n");
        umount(new_root.c_str());
        rmdir(new_root.c_str());
        return -1;
    }

    // 5. chdir 到新根，pivot_root
    if (chdir(new_root.c_str()) != 0) {
        std::fprintf(stderr, "rootfs: chdir 失败: %s\n", std::strerror(errno));
        umount(new_root.c_str());
        rmdir(new_root.c_str());
        return -1;
    }

    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        std::fprintf(stderr, "rootfs: pivot_root 失败: %s\n", std::strerror(errno));
        umount(new_root.c_str());
        rmdir(new_root.c_str());
        return -1;
    }

    // 6. 卸载旧根（MNT_DETACH 延迟卸载）
    if (umount2(".", MNT_DETACH) != 0) {
        std::fprintf(stderr, "rootfs: umount2 旧根失败: %s\n", std::strerror(errno));
        // 不返回失败，继续执行（旧根还在但不影响基本功能）
    }

    // 7. chdir 到新根
    chdir("/");

    return 0;
}

} // namespace rootfs
} // namespace sandbox
