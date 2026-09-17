// Wave 1 · 根文件系统管理
// 对应知识点：任务 1.2 rootfs 挂载清单；Wave 0 任务 0.5 pivot_root
#pragma once

#include <string>

namespace sandbox {
namespace rootfs {

// 在子进程中调用：创建 tmpfs 新根、复制二进制、pivot_root、卸载旧根
// binary_path：沙箱外的二进制路径（会被复制进 tmpfs）
// 返回 0 成功，-1 失败
int setup(const std::string& binary_path);

} // namespace rootfs
} // namespace sandbox
