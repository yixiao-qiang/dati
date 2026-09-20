// Wave 1 · 根文件系统管理实现
// 对应知识点：任务 1.2 ldd 动态库挂载清单；Wave 0 任务 0.5 pivot_root
//
// 面试要点：
// - 为什么用 bind mount 而不是复制库？省空间、库更新自动生效、但必须只读重挂防穿透
// - 为什么只读要分两步？MS_BIND|MS_RDONLY 一起用 RDONLY 被内核忽略，必须先 bind 再 remount
// - 最小化挂载：只挂 /lib /lib64 /usr/lib，不挂 /etc /usr/bin，攻击面最小
#include "rootfs.h"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace sandbox {
namespace rootfs {

// 递归创建目录（mkdir -p）
static bool mkdir_p(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            if (!current.empty() && current != "/") {
                mkdir(current.c_str(), 0755);
            }
        }
    }
    return true;
}

// 复制文件（简单实现）
static bool copy_file(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary);
    if (!out) return false;
    out << in.rdbuf();
    chmod(dst.c_str(), 0755);
    return out.good();
}

// bind mount 一个目录，并 remount 成只读
// 面试要点：必须分两步，MS_BIND|MS_RDONLY 一起用不生效
static bool bind_ro(const std::string& src, const std::string& dst) {
    // 第一步：bind mount（此时可写）
    if (mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr) != 0) {
        std::fprintf(stderr, "rootfs: bind %s -> %s 失败: %s\n",
                     src.c_str(), dst.c_str(), std::strerror(errno));
        return false;
    }
    // 第二步：remount 成只读（防止沙箱写操作穿透到宿主）
    if (mount(nullptr, dst.c_str(), nullptr,
              MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr) != 0) {
        std::fprintf(stderr, "rootfs: remount ro %s 失败: %s\n",
                     dst.c_str(), std::strerror(errno));
        return false;
    }
    return true;
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

    // 3. 挂载 tmpfs（nodev/nosuid；不用 NOEXEC，因为要在上面执行 /bin/prog）
    if (mount("tmpfs", new_root.c_str(), "tmpfs",
              MS_NODEV | MS_NOSUID,
              "size=64m,mode=0700") != 0) {
        std::fprintf(stderr, "rootfs: mount tmpfs 失败: %s\n", std::strerror(errno));
        rmdir(new_root.c_str());
        return -1;
    }

    // 4. 创建目录结构
    //    /bin/prog                    ← 用户程序
    //    /lib/x86_64-linux-gnu/       ← bind mount（只读，libc/libgcc_s）
    //    /lib64/                      ← bind mount（只读，ld-linux 动态链接器）
    //    /usr/lib/x86_64-linux-gnu/   ← bind mount（只读，libstdc++）
    mkdir_p(new_root + "/bin");
    mkdir_p(new_root + "/lib/x86_64-linux-gnu");
    mkdir_p(new_root + "/lib64");
    mkdir_p(new_root + "/usr/lib/x86_64-linux-gnu");

    // 5. 复制用户二进制到 /bin/prog
    std::string prog_path = new_root + "/bin/prog";
    if (!copy_file(binary_path, prog_path)) {
        std::fprintf(stderr, "rootfs: 复制二进制失败\n");
        umount(new_root.c_str());
        rmdir(new_root.c_str());
        return -1;
    }

    // 6. bind mount 动态库目录（只读）
    //    面试要点：ldd 分析依赖后提取的三个标准库路径
    //    最小化原则：不挂 /etc /usr/bin /home，沙箱里 cat /etc/passwd 应 ENOENT
    struct LibMount { const char* src; const char* dst; };
    LibMount libs[] = {
        {"/lib/x86_64-linux-gnu",      "/lib/x86_64-linux-gnu"},
        {"/lib64",                     "/lib64"},
        {"/usr/lib/x86_64-linux-gnu",  "/usr/lib/x86_64-linux-gnu"},
    };
    for (const auto& lib : libs) {
        if (!bind_ro(lib.src, new_root + lib.dst)) {
            std::fprintf(stderr, "rootfs: 挂载动态库目录失败: %s\n", lib.src);
            umount(new_root.c_str());
            rmdir(new_root.c_str());
            return -1;
        }
    }

    // 7. chdir 到新根，pivot_root
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

    // 8. 卸载旧根（MNT_DETACH 延迟卸载，防止旧根还有引用时 umount 失败）
    if (umount2(".", MNT_DETACH) != 0) {
        std::fprintf(stderr, "rootfs: umount2 旧根失败: %s\n", std::strerror(errno));
        // 不返回失败，继续执行（旧根还在但不影响基本功能）
    }

    // 9. chdir 到新根
    chdir("/");

    return 0;
}

} // namespace rootfs
} // namespace sandbox
