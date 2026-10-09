// Wave 2 · db.cpp —— MySQL 判题状态存储（占位实现，待你填充）
//
// 需要 MySQL C API 四个核心调用：
//   1. MYSQL* mysql_init(nullptr)
//   2. mysql_real_connect(conn, host, user, pass, db, 0, nullptr, 0)
//   3. mysql_query(conn, sql)   // 返回 0 成功
//   4. mysql_affected_rows(conn) / mysql_store_result / mysql_fetch_row
//
// 注意：
// - SQL 里拼接 submission_id 时用 snprintf 拼完整语句（学习期不引依赖），
//   面试点：生产级必须用 mysql_stmt_prepare 预编译防注入，这里先讲清差距
// - 条件更新失败要区分"影响行数 0"（正常，别人抢了）和"SQL 报错"（真错误）
//   mysql_query 返回非 0 = 错误，此时不能当"没抢到"处理

#include "db.h"

namespace worker {

const char* judge_status_to_string(JudgeStatus s) {
    switch (s) {
        case JudgeStatus::Pending:      return "pending";
        case JudgeStatus::Judging:      return "judging";
        case JudgeStatus::Accepted:     return "accepted";
        case JudgeStatus::WrongAnswer:  return "wrong_answer";
        case JudgeStatus::TimeLimit:    return "tle";
        case JudgeStatus::MemoryLimit:  return "mle";
        case JudgeStatus::RuntimeError: return "runtime_error";
        case JudgeStatus::CompileError: return "compile_error";
        case JudgeStatus::SystemError:  return "system_error";
    }
    return "unknown";
}

Db::~Db() {
    // TODO: mysql_close(conn_)
}

bool Db::connect(const std::string& host, const std::string& user,
                 const std::string& password, const std::string& database,
                 std::string* err) {
    // TODO: mysql_init + mysql_real_connect；失败填 mysql_error(conn)
    (void)host; (void)user; (void)password; (void)database; (void)err;
    return false;  // 占位
}

bool Db::claim(int64_t submission_id, std::string* err, bool* claimed) {
    // TODO:
    //   UPDATE submission SET status='judging', judge_started_at=NOW()
    //   WHERE id=<submission_id> AND status='pending'
    //   mysql_query 返回非 0 -> 填 err 返回 false
    //   mysql_affected_rows(conn)==1 -> *claimed=true
    //   mysql_affected_rows(conn)==0 -> *claimed=false（不是错误）
    (void)submission_id; (void)err; (void)claimed;
    return false;  // 占位
}

bool Db::finish(int64_t submission_id, JudgeStatus status,
                const std::string& stdout_text, const std::string& stderr_text,
                std::string* err) {
    // TODO:
    //   UPDATE submission SET status='<终态>', result='<终态>',
    //          stdout=<截断>, stderr=<截断>
    //   WHERE id=<submission_id>
    // 注意：stdout/stderr 先截断（建议各 64KB）再入库，防单条超长
    (void)submission_id; (void)status; (void)stdout_text; (void)stderr_text; (void)err;
    return false;  // 占位
}

bool Db::fetch_code(int64_t submission_id, std::string* code, std::string* language,
                    std::string* err) {
    // TODO:
    //   SELECT code, language FROM submission WHERE id=<submission_id>
    //   mysql_store_result + mysql_fetch_row 取第一行
    (void)submission_id; (void)code; (void)language; (void)err;
    return false;  // 占位
}

}  // namespace worker
