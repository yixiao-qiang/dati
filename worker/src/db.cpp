// Wave 2 · db.cpp —— MySQL 判题状态存储（台阶 1 实现）
//
// 面试知识点（为什么必须条件更新而不是先查再改）：
// - 两个 worker 可能同时读到同一条 pending 提交（消息重复投递/并发消费）
// - "先 SELECT 再 UPDATE" 有 TOCTOU 竞态窗口：两个进程都 SELECT 到 pending，
//   然后都去判题 → 同一提交被判两次
// - 条件更新把"判断 + 赋值"合成一条原子 SQL：
//     UPDATE submission SET status='judging' WHERE id=? AND status='pending'
//   影响行数 = 1 抢到任务；= 0 已被别人抢走（直接 XACK 丢弃）
// - mysql_affected_rows() 是幂等判断的唯一依据
//
// 学习期与生产级的差距（面试点）：
// - 这里 SQL 用 snprintf 拼整数 + mysql_real_escape_string 转义文本；
//   生产级必须用 mysql_stmt_prepare 预编译，从机制上消除注入可能。
//   （stdout/stderr 是**用户程序输出**，属于攻击者可控内容，因此即便学习期
//     也必须转义——这不是"可以简化"的项。）
// - 状态机转移表（pending→judging→终态、终态不可逆）的完整实现在台阶 3；
//   这里先把"终态不可逆"落到 finish() 的 WHERE 条件上，因为那是状态被
//   错误覆盖的发生点。

#include "db.h"

#include <mysql/mysql.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace worker {

namespace {

// stdout/stderr 入库前截断阈值：MEDIUMTEXT 有 16MB 空间，但一条判题输出
// 存 64KB 足够排查，且防止恶意程序刷爆存储与回显带宽。
constexpr size_t kMaxStoredBytes = 64 * 1024;

// 按 UTF-8 字符边界截断。
// 为什么不能直接 substr(0, N)：用户程序输出可能含多字节 UTF-8，
// 从中间切断会留下半个字符，前端渲染成乱码，且写库后无法还原。
// 判据：续字节的高 2 位是 10；若被丢弃部分的第一个字节是续字节，
// 说明切口落在字符内部，往前退到字符边界。
std::string truncate_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut);
}

// MySQL 字符串字面量转义。空串返回空串（无需转义）。
// 缓冲区按最坏情况 2n+1 分配（每个字节最多变成 2 字符）。
std::string escape_sql(void* conn, const std::string& raw) {
    if (raw.empty()) return std::string();
    if (!conn) return std::string();  // 无连接时调用方不会走到这里（防御）
    std::string out(raw.size() * 2 + 1, '\0');
    const size_t n = mysql_real_escape_string(static_cast<MYSQL*>(conn),
                                              &out[0], raw.c_str(), raw.size());
    out.resize(n);
    return out;
}

}  // namespace

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
    if (conn_) {
        mysql_close(static_cast<MYSQL*>(conn_));
        conn_ = nullptr;
    }
}

bool Db::connect(const std::string& host, const std::string& user,
                 const std::string& password, const std::string& database,
                 std::string* err) {
    MYSQL* c = mysql_init(nullptr);
    if (!c) {
        if (err) *err = "mysql_init 失败（内存不足）";
        return false;
    }

    // utf8mb4：用户代码/输出可能含 emoji（4 字节），utf8（3 字节）存不下会报错。
    // 必须在 mysql_real_connect **之前**设置才生效。
    if (mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
        if (err) *err = std::string("设置 utf8mb4 字符集失败: ") + mysql_error(c);
        mysql_close(c);
        return false;
    }

    // 自动重连：worker 是长驻进程，MySQL 重启/连接被踢后若不重连，
    // 之后每条 claim 都失败、任务永远卡 PEL。1 字节的 bool 与 mysql_options
    // 的 void* 形参兼容，且与 C API 读取的 my_bool 位宽一致。
    bool reconnect = true;
    mysql_options(c, MYSQL_OPT_RECONNECT, &reconnect);

    if (!mysql_real_connect(c, host.c_str(), user.c_str(), password.c_str(),
                            database.c_str(), 0, nullptr, 0)) {
        if (err) *err = mysql_error(c);
        mysql_close(c);
        return false;
    }

    conn_ = c;
    return true;
}

bool Db::claim(int64_t submission_id, std::string* err, bool* claimed) {
    *claimed = false;
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    // 条件更新：判断 + 赋值一条 SQL 原子完成，消灭 TOCTOU 窗口。
    // submission_id 是 int64，用 %lld 拼不存在注入面。
    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "UPDATE submission SET status='judging', judge_started_at=NOW() "
                  "WHERE id=%lld AND status='pending'",
                  (long long)submission_id);

    if (mysql_query(c, sql) != 0) {
        // SQL 报错是真错误：不能当"没抢到"处理，否则会静默丢任务。
        // 不 ACK，让消息留在 PEL 等重投。
        if (err) *err = mysql_error(c);
        return false;
    }

    // ★ 影响行数语义：未设 CLIENT_FOUND_ROWS 时，mysql_affected_rows 返回
    // "被修改的行数"而非"匹配的行数"。这里正是我们要的——status 从
    // pending 变成 judging 值一定变，所以 affected==1 精确等价于"我抢到了"。
    const my_ulonglong n = mysql_affected_rows(c);
    *claimed = (n == 1);
    return true;
}

bool Db::finish(int64_t submission_id, JudgeStatus status,
                const std::string& stdout_text, const std::string& stderr_text,
                std::string* err) {
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    // 状态机不变量（台阶 3 会做完整转移表，这里先守住最关键的一条）：
    // finish 只写终态。若传入 pending/judging，说明调用方把"中间态"当结果写，
    // 会让提交永远停在非终态、前端轮询超时——直接拒绝，不吞。
    if (status == JudgeStatus::Pending || status == JudgeStatus::Judging) {
        if (err) *err = "finish 拒绝写非终态: " +
                        std::string(judge_status_to_string(status));
        return false;
    }

    const std::string st = judge_status_to_string(status);  // 来自固定 switch，可信
    const std::string out_t = truncate_utf8(stdout_text, kMaxStoredBytes);
    const std::string err_t = truncate_utf8(stderr_text, kMaxStoredBytes);
    const std::string out_e = escape_sql(conn_, out_t);
    const std::string err_e = escape_sql(conn_, err_t);

    std::string sql;
    sql.reserve(out_e.size() + err_e.size() + 256);
    sql += "UPDATE submission SET status='";
    sql += st;
    sql += "', result='";
    sql += st;
    sql += "', stdout='";
    sql += out_e;
    sql += "', stderr='";
    sql += err_e;
    sql += "' WHERE id=";
    sql += std::to_string(static_cast<long long>(submission_id));
    // ★ WHERE status='judging' 是"终态不可逆"的防线：
    //   慢 worker A 判完时，rescue 已把任务交给 B 且 B 已写终态 → A 的 UPDATE
    //   匹配 0 行，不会覆盖 B 的结果。affected==0 是**良性**情况，不是错误。
    sql += " AND status='judging'";

    if (mysql_query(c, sql.c_str()) != 0) {
        if (err) *err = mysql_error(c);
        return false;
    }

    const my_ulonglong n = mysql_affected_rows(c);
    if (n == 0 && err) {
        // 不返回 false：任务确实判完了，只是状态已被别人写过。
        // 调用方应照常 XACK（消息消费已完成），只记录即可。
        *err = "finish 影响 0 行：终态可能已被写入，不覆盖";
    }
    return true;
}

bool Db::fetch_code(int64_t submission_id, std::string* code, std::string* language,
                    std::string* err, bool* found) {
    *found = false;
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "SELECT code, language FROM submission WHERE id=%lld",
                  (long long)submission_id);

    if (mysql_query(c, sql) != 0) {
        if (err) *err = mysql_error(c);
        return false;
    }

    // SELECT 一定返回结果集（即使 0 行）；NULL 只可能是出错。
    MYSQL_RES* res = mysql_store_result(c);
    if (!res) {
        if (err) *err = mysql_error(c);
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        // 库里没有这条提交：消息是脏的（流里残留/被删）。
        // 用 found=false 明确区分"查无此行"与"查询出错"——
        // 前者应 ACK 丢弃，后者应留 PEL 重试，语义不同不能混。
        mysql_free_result(res);
        return true;
    }

    // 用 mysql_fetch_lengths 而非 strlen：字段可能含内嵌 NUL 字节，
    // 且能避免依赖字符串以 NUL 结尾。
    const unsigned long* len = mysql_fetch_lengths(res);
    if (row[0] && len) code->assign(row[0], static_cast<size_t>(len[0]));
    else if (row[0])   code->assign(row[0]);
    if (row[1] && len) language->assign(row[1], static_cast<size_t>(len[1]));
    else if (row[1])   language->assign(row[1]);

    mysql_free_result(res);
    *found = true;
    return true;
}

}  // namespace worker
