// Wave 2 · main.cpp —— worker 主循环（占位骨架，待你填充）
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
#include <string>

#include "queue.h"
#include "db.h"
#include "judge.h"

int main() {
    // ---- 配置（学习期先用环境变量，Wave 3 统一配置中心）----
    const char* redis_host = "127.0.0.1";
    int redis_port = 6379;
    const char* db_host = "127.0.0.1";
    const char* db_user = "root";
    const char* db_pass = std::getenv("MYSQL_PASS") ? std::getenv("MYSQL_PASS") : "";
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
    if (!q.connect(redis_host, redis_port, &err)) {
        std::fprintf(stderr, "Redis 连接失败: %s\n", err.c_str());
        return 1;
    }
    if (!db.connect(db_host, db_user, db_pass, db_name, &err)) {
        std::fprintf(stderr, "MySQL 连接失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("[worker %s] 已连接 Redis(%s:%d) + MySQL(%s/%s)，JUDGE_MAIN=%s\n",
                consumer_id, redis_host, redis_port, db_host, db_name, judge_main);

    // ---- 主循环 ----
    while (true) {
        std::vector<worker::StreamMessage> msgs;
        if (!q.consume(consumer_id, 5000, &msgs, &err)) {
            std::fprintf(stderr, "consume 失败: %s\n", err.c_str());
            continue;  // 连接层错误：不退出，下轮重试（生产会退避，学习期先继续）
        }
        for (const auto& m : msgs) {
            // TODO 台阶 1 处理流程：
            //   1. db.fetch_code(m.submission_id) 取源码
            //   2. db.claim(m.submission_id) 条件更新幂等
            //       - claimed=true  -> judge_code() 判题 -> db.finish() 写终态
            //       - claimed=false -> 已被别人抢/已终态，直接 ACK 丢弃
            //   3. 无论哪条路径最后都 q.ack(m.id)
            std::printf("[worker %s] 收到消息 id=%s submission_id=%lld\n",
                        consumer_id, m.id.c_str(), (long long)m.submission_id);
        }
    }

    return 0;
}
