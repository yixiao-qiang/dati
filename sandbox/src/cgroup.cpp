// Wave 1 · cgroup v2 管理实现
#include "cgroup.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>

namespace sandbox {
namespace cgroup {

static const char* CGROUP_ROOT = "/sys/fs/cgroup";

// 读一个小文件的全部内容
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 写字符串到文件
static bool write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f) return false;
    f << content;
    return f.good();
}

std::string create(int memory_limit_mb, int pids_limit) {
    // 生成唯一目录名
    char tmpl[] = "/sys/fs/cgroup/sandbox-XXXXXX";
    if (!mkdtemp(tmpl)) return "";
    std::string cg_path(tmpl);

    // 设内存上限（字节）
    long long mem_bytes = (long long)memory_limit_mb * 1024 * 1024;
    if (!write_file(cg_path + "/memory.max", std::to_string(mem_bytes))) {
        cleanup(cg_path);
        return "";
    }

    // 禁止 swap：否则物理内存超限后走 swap，不会立即 OOM Kill
    // 面试点：OJ 必须禁 swap，否则 MLE 不触发（内存被换到磁盘，程序继续跑）
    if (!write_file(cg_path + "/memory.swap.max", "0")) {
        cleanup(cg_path);
        return "";
    }

    // 设进程数上限
    if (!write_file(cg_path + "/pids.max", std::to_string(pids_limit))) {
        cleanup(cg_path);
        return "";
    }

    return cg_path;
}

bool migrate_pid(int pid, const std::string& cg_path) {
    return write_file(cg_path + "/cgroup.procs", std::to_string(pid));
}

int64_t read_memory_peak(const std::string& cg_path) {
    std::string content = read_file(cg_path + "/memory.peak");
    if (content.empty()) return 0;
    return std::atoll(content.c_str());
}

int64_t read_cpu_usage(const std::string& cg_path) {
    // cpu.stat 格式：
    // usage_usec 123456
    // user_usec 100000
    // system_usec 23456
    std::string content = read_file(cg_path + "/cpu.stat");
    std::istringstream iss(content);
    std::string key;
    int64_t value;
    while (iss >> key >> value) {
        if (key == "usage_usec") return value;
    }
    return 0;
}

int read_oom_kill(const std::string& cg_path) {
    // memory.events 格式：
    // anon 123
    // oom_kill 1
    std::string content = read_file(cg_path + "/memory.events");
    std::istringstream iss(content);
    std::string key;
    int value;
    while (iss >> key >> value) {
        if (key == "oom_kill") return value;
    }
    return 0;
}

void cleanup(const std::string& cg_path) {
    if (cg_path.empty()) return;

    // 把 cgroup 里所有进程移到根 cgroup（否则 rmdir 报 "设备或资源忙"）
    std::string procs = read_file(cg_path + "/cgroup.procs");
    std::istringstream iss(procs);
    int pid;
    while (iss >> pid) {
        write_file(std::string(CGROUP_ROOT) + "/cgroup.procs", std::to_string(pid));
    }

    // 删空 cgroup 目录
    rmdir(cg_path.c_str());
}

} // namespace cgroup
} // namespace sandbox
