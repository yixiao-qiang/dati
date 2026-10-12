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

private:
    void* conn_ = nullptr;  // MYSQL*
};

}  // namespace worker
