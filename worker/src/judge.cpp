// Wave 2 · judge.cpp —— 判题流水线（占位实现，待你填充）
//
// 两步实现要点：
// 1. 编译（沙箱外）：
//    - 写源码到 /tmp/oj_<uuid>.cpp
//    - fork + execlp("g++", "-O2", "-o", bin, src, nullptr)
//    - 编译墙钟超时（30s）杀掉编译器；失败时把 stderr 当 CE 返回
//    - 谁创建谁清理：src/bin 都在本函数 finally 删除（Wave 1 审查修复的纪律）
// 2. 运行（沙箱内）：
//    - fork + execl(judge_main, bin, time_ms, mem_mb, output_mb)
//    - 读 stdout 一行 JSON，解析 status/exit_code/signal/cpu_us/mem_bytes/...
//    - judge_main 需要 root：geteuid()!=0 时用 sudo -n 前缀（与 server 一致）
//
// 提示：JSON 解析学习期可用最简手段（找 "status":"XXX" 子串），
//       面试点：讲清"学习期简化 vs 生产级 nlohmann/json"的差距即可。

#include "judge.h"

namespace worker {

bool judge_code(const std::string& code, const std::string& language,
                const std::string& judge_main,
                int time_ms, int mem_mb, int output_mb,
                JudgeOutcome* outcome, std::string* err) {
    // TODO: 编译 + 判题全流程
    (void)code; (void)language; (void)judge_main;
    (void)time_ms; (void)mem_mb; (void)output_mb;
    (void)outcome; (void)err;
    return false;  // 占位
}

}  // namespace worker
