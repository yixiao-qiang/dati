// Wave 2 · main.cpp —— worker 主循环（台阶 1-3 实现）
//
// 面试知识点（主循环的可靠性设计）：
// - BLOCK 读：XREADGROUP BLOCK 5000，无消息时阻塞等待而不是空转烧 CPU
// - 幂等防线：claim 影响行数=0 -> 直接 ACK 丢弃（消息重复但任务只判一次）
// - 失败不 ACK：数据库错误时让消息留在 PEL，10 分钟后 XAUTOCLAIM 会重投
//   —— 宁可延迟重试，不能静默丢任务
// - 崩溃恢复（台阶 2）：worker 崩了 -> 消息挂 PEL -> XAUTOCLAIM 认领
// - 卡死恢复（台阶 3）：消息重投后 claim 只认 pending，judging 卡死要靠
//   rescue 扫描重置回 pending；重试超限转死信等人工

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
// 台阶 3：rescue 扫描（judging 卡死 → pending 重新入队 / 死信）
constexpr int64_t kRescueScanMs = 60000;   // 每 60s 扫一次卡死任务
constexpr int kRescueStaleMin = 10;        // judging 超时阈值（与判死阈值 10min 对齐）
constexpr int kRescueMaxRetry = 3;         // 重试上限，超限转死信等人工
// 台阶 4：Outbox 投递器（本地消息表 → Redis Stream）
constexpr int64_t kOutboxScanMs = 2000;    // 每 2s 扫一次待投递消息

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
    // 扫描时间点初始化为 0：启动后**立即**扫一次（重启的 worker 应马上接管
    // 死任务/卡死任务/待投递消息，而不是等满一个周期才恢复）
    int64_t last_claim_scan_ms = 0;    // 上次扫死任务的时间点
    int64_t last_rescue_scan_ms = 0;   // 上次扫卡死任务的时间点
    int64_t last_outbox_scan_ms = 0;   // 上次扫待投递消息的时间点
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

        // ---- 台阶 3：周期 rescue 扫描（judging 卡死 → pending / 死信）----
        // 与 claim_idle 的分工：Redis 层管消息投递（PEL），DB 层管任务状态。
        // 为什么需要：消息重投后 claim 只认 pending；若任务卡在 judging
        // （worker 判题中 DB 断连 → finish 失败 → 消息重投 → claim 0 行被
        // ACK），XAUTOCLAIM 救不了它——必须把 judging 重置回 pending。
        // 时机：每 60s 一次（比 claim 扫描慢：判死阈值 10min，60s 粒度足够）
        if (now_ms() - last_rescue_scan_ms >= kRescueScanMs) {
            last_rescue_scan_ms = now_ms();
            std::vector<int64_t> reset_ids, dead_ids;
            if (db.rescue_stale(kRescueStaleMin, kRescueMaxRetry,
                                &reset_ids, &dead_ids, &err)) {
                // 重置的任务必须重新入队：
                // 消息可能已不在 PEL（XAUTOCLAIM 可能先重投过、claim 0 行后
                // 被 ACK）——不重新 XADD 的话，状态是 pending 但没人消费。
                // 重复消息无害：claim 条件更新幂等挡住（另一条消息已处理时
                // 影响 0 行 → ACK）。
                for (int64_t id : reset_ids) {
                    std::string mid = q.produce(id, &err);
                    if (mid.empty()) {
                        std::fprintf(stderr, "rescue 重新入队失败(id=%lld): %s\n",
                                     (long long)id, err.c_str());
                    } else {
                        std::fprintf(stderr, "[rescue] id=%lld 重置 pending 并重新入队 msg=%s\n",
                                     (long long)id, mid.c_str());
                    }
                }
                for (int64_t id : dead_ids) {
                    std::fprintf(stderr, "[rescue] id=%lld 重试超限转死信，等待人工介入\n",
                                 (long long)id);
                }
            } else {
                std::fprintf(stderr, "rescue_stale 失败: %s\n", err.c_str());
                // 不退出：扫描失败下轮再试
            }
        }

        // ---- 台阶 4：周期投递 Outbox（本地消息表 → Redis Stream）----
        // 解决双写一致性：server 事务里写 submission + outbox（原子），
        // 本投递器负责把 outbox 里的待发消息送进 Stream。
        // 为什么需要 outbox：直接 XADD 的话，"DB 写成功 + Redis 写失败"时
        // 任务卡 pending 无人消费（rescue 只捞 judging 不捞 pending）。
        // 时机：每 2s 一次（提交到出队延迟 ≈2s，用户体验可接受）
        if (now_ms() - last_outbox_scan_ms >= kOutboxScanMs) {
            last_outbox_scan_ms = now_ms();
            std::vector<worker::Db::OutboxEntry> events;
            if (db.fetch_undelivered(10, &events, &err)) {
                for (const auto& e : events) {
                    std::string mid = q.produce(e.submission_id, &err);
                    if (mid.empty()) {
                        std::fprintf(stderr, "outbox 投递失败(outbox_id=%lld): %s\n",
                                     (long long)e.id, err.c_str());
                        // 释放 FOR UPDATE 锁：这批行留给下轮重试
                        db.rollback(nullptr);
                        break;
                    }
                    if (!db.mark_delivered(e.id, &err)) {
                        std::fprintf(stderr, "outbox 标记失败(outbox_id=%lld): %s\n",
                                     (long long)e.id, err.c_str());
                        db.rollback(nullptr);
                        break;
                    }
                    std::fprintf(stderr, "[outbox] 已投递 submission=%lld (outbox_id=%lld) msg=%s\n",
                                 (long long)e.submission_id, (long long)e.id, mid.c_str());
                }
            } else {
                std::fprintf(stderr, "fetch_undelivered 失败: %s\n", err.c_str());
                // 不退出：投递失败下轮再试（消息还在 outbox，不丢）
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
    constexpr int64_t kOutboxScanMs = 2000;

    return 0;
}
