// Wave 2 · judge.h —— 判题流水线（接口契约）
//
// 面试知识点（为什么 worker 不 link sandbox 库，而是子进程调 judge_main）：
// - 崩溃隔离：judge_main 是独立进程，沙箱/判题崩了不影响 worker 主循环
// - 权限隔离：judge_main 需要 root（clone/cgroup/pivot_root），worker 不必持 root
// - 行为一致：server 和 worker 共用同一个判题入口，判题逻辑只维护一份
//
// 判题流程（两步，与垂直切片 server 相同）：
//   1. 编译（沙箱外）：源码 -> g++ -O2 -> 二进制（/tmp/oj_<uuid>）
//   2. 运行（沙箱内）：judge_main <bin> <time_ms> <mem_mb> <output_mb> -> 一行 JSON

#pragma once

#include <cstdint>
#include <string>

namespace worker {

// 判题结果（与 judge_main 输出的 JSON 字段对应）
struct JudgeOutcome {
    std::string status;      // OK / WA / TLE / MLE / RE / CE / INTERNAL_ERROR
    int exit_code = -1;
    int signal = 0;
    int64_t cpu_us = 0;
    int64_t mem_bytes = 0;
    int64_t wall_us = 0;
    std::string stdout_text;
    std::string stderr_text;
    std::string error_msg;   // INTERNAL_ERROR 时的人类可读原因
};

// 判题流水线：编译 + 运行，返回 outcome
// 参数：
//   code      - 用户提交的源码
//   language  - cpp / c
//   judge_main - judge_main 可执行文件路径（JUDGE_MAIN 环境变量，启动时校验）
//   time_ms / mem_mb / output_mb - 判题限额
// 编译失败（CE）也通过 outcome.status="CE" 返回，不视为调用错误
bool judge_code(const std::string& code, const std::string& language,
                const std::string& judge_main,
                int time_ms, int mem_mb, int output_mb,
                JudgeOutcome* outcome, std::string* err);

}  // namespace worker
