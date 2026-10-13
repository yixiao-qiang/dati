// Wave 2 · main.cpp —— worker 主循环（台阶 1 实现）
//
// 面试知识点（主循环的可靠性设计）：
// - BLOCK 读：XREADGROUP BLOCK 5000，无消息时阻塞等待而不是空转烧 CPU
// - 幂等防线：claim 影响行数=0 -> 直接 ACK 丢弃（消息重复但任务只判一次）
// - 失败不 ACK：数据库错误时让消息留在 PEL，10 分钟后 XAUTOCLAIM 会重投
//   —— 宁可延迟重试，不能静默丢任务
// - 崩溃恢复（台阶 2 加）：worker 崩了 -> 消息挂 PEL -> XAUTOCLAIM 认领

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "queue.h"
#include "db.h"
#include "judge.h"


// ---- 台阶 2 常量：崩溃恢复参数 ----
// 判死阈值 10min：判题秒级完成，10min 足够宽；worker 死后最多 10 分钟恢复
//   （太短：慢任务被误判死 → 重复投递；太长：死任务恢复慢 → 用户等不了）
// 接管扫描周期 5s：主循环每轮检查、到点才扫，避免每轮都发 XAUTOCLAIM
//   （Redis 往返有成本；判死阈值 10min，早几秒晚几秒无差别）
constexpr int64_t kClaimIdleMs = 600000;   // 10min 判死阈值
constexpr int64_t kClaimScanMs = 5000;     // 每 5s 扫一次死任务
constexpr int64_t kBlockMs = 5000;         // 无消息时阻塞等待

// 单调时钟（CLOCK_MONOTONIC）：不受系统改时间影响，只用于"过了多久"
int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


void process_one(worker::Queue& q, worker::Db& db, const char* judge_main,
                 const worker::StreamMessage& m) {
    std::string err, code, lang;
    bool found = false;
    if(!db.fetch_code(m.submission_id, &code, &lang, &err,&found)){
        std::fprintf(stderr,"fetch_code 失败(id=%lld): %s\n", (long long )m.submission_id,err.c_str());
        return;
    }
    if(!found){
        q.ack(m.id,nullptr);
        return;
    }

    bool claimd = false;
    if(!db.claim(m.submission_id, &err, &claimd)){
        std::fprintf(stderr,"claim失败(id=%lld): %s\n",(long long)m.submission_id,err.c_str());
        return;
    }
    if(!claimd){
        q.ack(m.id,nullptr);
        return;
    }

    worker::JudgeOutcome out;
    // 真判题：编译(沙箱外 g++) + judge_main(沙箱内) → 填 out
    // 限额学习期硬编码 1000ms/256MB/64MB —— 正式版应从 DB/题目表读取
    if (!judge_code(code, lang, judge_main, 1000, 256, 64, &out, &err)) {
        std::fprintf(stderr, "judge 内部故障(id=%lld): %s\n",
                     (long long)m.submission_id, err.c_str());
        return;  // 不 ACK —— 内部故障留 PEL 等重投
    }

    if(!db.finish(m.submission_id,out.status,out.stdout_text,out.stderr_text,&err)){
        std::fprintf(stderr,"finish 失败(id=%lld):%s\n",(long long )m.submission_id,err.c_str());
        return;
    }
    q.ack(m.id,nullptr);
}
int main() {
    // ---- 配置（学习期先用环境变量，Wave 3 统一配置中心）----
    const char* redis_host = "127.0.0.1";
    int redis_port = 6379;
    const char* db_host = "127.0.0.1";
    const char* db_name = "oj";
    const char* judge_main = std::getenv("JUDGE_MAIN") ? std::getenv("JUDGE_MAIN") : "";
    const char* consumer_id = std::getenv("WORKER_ID") ? std::getenv("WORKER_ID") : "w1";

    if (!judge_main[0]) {
        std::fprintf(stderr, "JUDGE_MAIN 未配置：export JUDGE_MAIN=.../build/judge_main\n");
        return 2;
    }

    std::string err;
    worker::Queue q;
    worker::Db db;

    // ---- 连接（失败即退出，让 supervisor 拉起；不吞错）----
    if (!q.connect("127.0.0.1", 6379, &err)) {
        std::fprintf(stderr, "Redis 连接失败: %s\n", err.c_str());
        return 1;
    }
    if (!db.connect("127.0.0.1", "oj", "oj123456", "oj", &err)) {
        std::fprintf(stderr, "MySQL 连接失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("[worker %s] 已连接 Redis(%s:%d) + MySQL(%s/%s)，JUDGE_MAIN=%s\n",
                consumer_id, redis_host, redis_port, db_host, db_name, judge_main);

    // ---- 主循环 ----
    int64_t last_claim_scan_ms = now_ms();  // 上次扫死任务的时间点
    while (true) {
        // ---- 台阶 2：周期扫描死任务（XAUTOCLAIM 判死接管）----
        // 时机：每 5s 一次。被接管的消息走与正常消息完全相同的流程
        // （fetch → claim → judge → finish → ack），因为 DB 条件更新幂等会
        // 挡掉"原 worker 其实没死/已判完"的重复投递——这就是台阶 1 做幂等
        // 的意义：没有它，崩溃恢复就是重复判题。
        if (now_ms() - last_claim_scan_ms >= kClaimScanMs) {
            last_claim_scan_ms = now_ms();
            std::vector<worker::StreamMessage> dead;
            if (q.claim_idle(consumer_id, kClaimIdleMs, &dead, &err)) {
                for (const auto& m : dead) {
                    std::fprintf(stderr, "[worker %s] 接管死任务 id=%s (submission=%lld)\n",
                                 consumer_id, m.id.c_str(), (long long)m.submission_id);
                    process_one(q, db, judge_main, m);
                }
            } else {
                std::fprintf(stderr, "claim_idle 失败: %s\n", err.c_str());
                // 不退出：扫描失败不影响正常消费，下轮再试
            }
        }

        // ---- 正常消费 ----
        std::vector<worker::StreamMessage> msgs;
        if (!q.consume(consumer_id, kBlockMs, &msgs, &err)) {
            std::fprintf(stderr, "consume 失败: %s\n", err.c_str());
            continue;  // 连接层错误：不退出，下轮重试（生产会退避，学习期先继续）
        }
        for (const auto& m : msgs) {
            // ---- 判题前心跳（防御性）----
            // 为什么：XREADGROUP 读走消息瞬间 idle 归零，正常判题（秒级）不会
            // 触发 10min 判死。但若未来任务变慢（大工程编译、判题机负载高），
            // 消息可能在处理中被判死 → 提前重置一次 idle，声明"我接手了"。
            // 处理中如需持续心跳，应在处理循环里周期调用 reset_idle。
            if (!q.reset_idle(consumer_id, m.id, &err)) {
                std::fprintf(stderr, "reset_idle 失败(id=%s): %s\n",
                             m.id.c_str(), err.c_str());
                // 不中断处理：心跳失败不致命，DB 幂等是最后防线
            }
            process_one(q, db, judge_main, m);
        }
    }

    return 0;
}
