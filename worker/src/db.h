// Wave 2 · db.h —— MySQL 判题状态存储（接口契约）
//
// 面试知识点（为什么必须条件更新而不是先查再改）：
// - 两个 worker 可能同时读到同一条 pending 提交（消息重复投递/并发消费）
// - "先 SELECT 再 UPDATE" 有 TOCTOU 竞态窗口：两个进程都 SELECT 到 pending，
//   然后都去判题 → 同一提交被判两次
// - 条件更新把"判断 + 赋值"合成一条原子 SQL：
//     UPDATE submission SET status='judging' WHERE id=? AND status='pending'
//   影响行数 = 1 抢到任务；= 0 已被别人抢走（直接 XACK 丢弃）
// - mysql_affected_rows() 是幂等判断的唯一依据

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace worker {

// 判题终态（与沙箱 Status 一一对应，写在 MySQL submission.status 列）
// 面试点：终态不可逆是状态机不变量，judging 只能通过 rescue 回 pending（Wave 2 台阶 3）
enum class JudgeStatus {
    Pending,       // pending      —— 刚提交，等待消费
    Judging,       // judging      —— 已被 worker 抢走，判题中
    Accepted,      // accepted
    WrongAnswer,   // wrong_answer
    TimeLimit,     // tle
    MemoryLimit,   // mle
    RuntimeError,  // runtime_error
    CompileError,  // compile_error
    SystemError,   // system_error
};

const char* judge_status_to_string(JudgeStatus s);

// MySQL 判题状态存储封装
class Db {
public:
    Db() = default;
    ~Db();

    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // 连接 MySQL（默认库 oj）。失败返回 false 并填 err。
    bool connect(const std::string& host, const std::string& user,
                 const std::string& password, const std::string& database,
                 std::string* err);

    // 条件更新幂等：pending -> judging。
    // 返回 true 且 claimed=true  = 抢到任务（影响行数 1），继续判题
    // 返回 true 且 claimed=false = 已被别人抢/已终态（影响行数 0），应 XACK 丢弃
    // 返回 false                  = 数据库错误（不要 ACK，让消息留在 PEL 等重试）
    bool claim(int64_t submission_id, std::string* err, bool* claimed);

    // 判题完成：写入终态与结果（stdout/stderr 截断存储，避免单行超长）
    bool finish(int64_t submission_id, JudgeStatus status,
                const std::string& stdout_text, const std::string& stderr_text,
                std::string* err);

    // 读回一条提交的代码与语言（worker 判题前取源码用）
    // found 出参区分两种失败，语义不同不能混：
    //   *found=false = 库里没有这条提交（脏消息，应 ACK 丢弃）
    //   返回 false   = 数据库错误（不要 ACK，留 PEL 等重投）
    bool fetch_code(int64_t submission_id, std::string* code, std::string* language,
                    std::string* err, bool* found);

    // rescue 扫描：找出 judging 超时（judge_started_at 早于 NOW()-stale_minutes）的任务
    //   - retry_count < max_retry → 重置 pending + retry_count+1（让消息重投后能再次 claim）
    //   - retry_count >= max_retry → 转 dead（死信，人工介入，不再自动捞）
    // 参数用整数分钟而非 ISO 字符串：时间计算交给 MySQL（NOW() - INTERVAL），
    // 整数无注入面、无需转义。
    // 输出：reset_ids（已重置 pending，调用方必须重新入队——消息可能已不在 PEL）、
    //       dead_ids（已转死信，不入队）
    // 返回 false = 数据库错误（本次扫描作废，下轮重试）
    bool rescue_stale(int stale_minutes, int max_retry,
                      std::vector<int64_t>* reset_ids,
                      std::vector<int64_t>* dead_ids,
                      std::string* err);

    // ---- 台阶 4：Outbox 投递器（本地消息表 + SKIP LOCKED）----

    // 一条待投递的 outbox 事件（server 事务里写入，投递器负责送进 Redis）
    struct OutboxEntry {
        int64_t id;             // outbox 主键（mark_delivered 用）
        int64_t submission_id;  // 载荷：关联的提交
    };

    // 取待投递（delivered=0）的 outbox 行，最多 limit 条。
    // ★ SELECT ... FOR UPDATE SKIP LOCKED：
    //   FOR UPDATE 锁住选中的行（本事务处理期间别人拿不到）；
    //   SKIP LOCKED 跳过已被其他事务锁住的行（不等待）。
    //   多投递器并发时各拿各的行 → 不重复投递；且不阻塞。
    // 返回 0..n 行；无待投递返回空 vector（不是错误）；失败返回 false。
    bool fetch_undelivered(int limit, std::vector<OutboxEntry>* out, std::string* err);

    // 标记已投递：UPDATE outbox SET delivered=1 WHERE id=? AND delivered=0
    // 条件更新幂等：重复标记影响 0 行，无害。内部 COMMIT 释放 FOR UPDATE 锁。
    bool mark_delivered(int64_t outbox_id, std::string* err);

    // 回滚 outbox 取行事务（XADD 失败时调用：释放 FOR UPDATE 锁，下轮重试）。
    // ★ 为什么需要：fetch_undelivered 的锁靠显式事务跨语句持有，
    //   投递失败必须 ROLLBACK 释放，否则锁持有到连接关闭/事务超时。
    bool rollback(std::string* err);

private:
    void* conn_ = nullptr;  // MYSQL*
};

}  // namespace worker
