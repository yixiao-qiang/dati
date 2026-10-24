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
//      （用例输入经 judge_main 的 stdin 传入——argv 单参数上限 128KB，大输入
//        会 E2BIG，见 judge.cpp run_capture 的 child_input 注释）

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "db.h"   // JudgeStatus 枚举（与 db.finish 对齐）

namespace worker {

// 判题结果（与 judge_main 输出的 JSON 字段对应）
struct JudgeOutcome {
    // ★ fail-closed 默认：任何未走完判题流水线的路径（内部故障提前 return）
    //   都留下"系统错误"而不是未初始化的垃圾枚举值——垃圾值传给 db.finish()
    //   会被白名单拒绝 → finish 失败 → 不 ACK → 无限重投。默认 SystemError
    //   保证即使逻辑漏了一条路径，落库也是明确可诊断的状态。
    worker::JudgeStatus status = worker::JudgeStatus::SystemError;
    int exit_code = -1;
    int signal = 0;
    int64_t cpu_us = 0;
    int64_t mem_bytes = 0;
    int64_t wall_us = 0;
    std::string stdout_text;
    std::string stderr_text;
    std::string error_msg;   // INTERNAL_ERROR 时的人类可读原因
    int passed_cases = 0;   // 台阶 2：通过的用例数（全过 = AC）
    int total_cases = 0;    // 台阶 2：用例总数（台阶 3 落库展示用）
};

// 判题流水线：编译 + 运行，返回 outcome
// 参数：
//   code      - 用户提交的源码
//   language  - cpp / c
//   judge_main - judge_main 可执行文件路径（JUDGE_MAIN 环境变量，启动时校验）
//   time_ms / mem_mb / output_mb - 判题限额
//   heartbeat - 可选心跳回调（D3）：判题处理期间 run_capture 轮询循环每 30s
//               调一次。长任务（编译/运行超过判死阈值 10min）靠它续命，
//               防止消息 idle 涨过阈值被 XAUTOCLAIM 误判死——主循环在
//               judge_code 里是阻塞的，只有回调有机会动 Redis。
// 编译失败（CE）也通过 outcome.status="CE" 返回，不视为调用错误
bool judge_code(const std::string& code, const std::string& language,
                const std::string& judge_main,
                int time_ms, int mem_mb, int output_mb,
                const std::vector<Testcase>& testcases,
                JudgeOutcome* outcome, std::string* err,
                const std::function<void()>& heartbeat = {});

}  // namespace worker
