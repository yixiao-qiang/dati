// Wave 1 · 沙箱核心 —— 公共接口
// 对应知识点：任务 1.1 沙箱外编译 + 沙箱内运行架构
#pragma once

#include <string>
#include <cstdint>

namespace sandbox {

// 判题状态
enum class Status {
    OK,              // 正常退出（退出码 0）
    NONZERO_EXIT,    // 正常退出但退出码非 0
    TLE,             // 超时（Time Limit Exceeded）
    MLE,             // 内存超限（Memory Limit Exceeded）
    RUNTIME_ERROR,   // 运行时错误（段错误、被信号杀等）
    OLE,             // 输出超限（Output Limit Exceeded）
    INTERNAL_ERROR,  // 沙箱内部错误（不是用户代码的问题）
};

// 沙箱运行配置
struct SandboxConfig {
    std::string binary_path;      // 要运行的二进制路径（沙箱外的绝对路径）
    std::string input;            // 标准输入内容
    int time_limit_ms = 1000;     // 墙钟超时（毫秒）
    int memory_limit_mb = 256;    // 内存限制（MB）
    int pids_limit = 64;          // 进程数上限
    int output_limit_mb = 64;     // 输出上限（MB）
    bool use_rootfs = true;       // 是否 pivot_root（骨架阶段可关）
};

// 判题结果
struct JudgeResult {
    Status status = Status::INTERNAL_ERROR;
    int exit_code = -1;           // 正常退出时的退出码
    int signal = 0;               // 被信号杀死时的信号编号
    std::string stdout_output;    // 标准输出
    std::string stderr_output;    // 标准错误
    int64_t cpu_time_us = 0;      // CPU 时间（微秒，来自 cpu.stat）
    int64_t memory_peak_bytes = 0;// 内存峰值（字节，来自 memory.peak）
    int64_t wall_time_us = 0;     // 墙钟时间（微秒，父进程计时）
    std::string error_msg;        // 内部错误时的描述
};

// 主入口：在沙箱中运行配置指定的二进制
JudgeResult run(const SandboxConfig& config);

// 工具：状态转字符串
const char* status_to_string(Status s);

} // namespace sandbox
