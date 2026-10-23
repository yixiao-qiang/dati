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
#include "internal.h"

#include <mysql/mysql.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace worker {

namespace {

// stdout/stderr 入库前截断阈值：MEDIUMTEXT 有 16MB 空间，但一条判题输出
// 存 64KB 足够排查，且防止恶意程序刷爆存储与回显带宽。
constexpr size_t kMaxStoredBytes = 64 * 1024;

}  // namespace

// ---- D4 导出：内部纯函数（声明见 internal.h，供单元测试）----

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
        case JudgeStatus::Dead:         return "dead";
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

    // 状态机不变量：finish 只写"判题产生的终态"。
    // ★ N2：用白名单而非黑名单。黑名单（拒 Pending/Judging）在枚举新增状态时
    //   会漏——Dead 补进枚举后不在拦截名单里，finish(id, Dead) 会被接受并命中
    //   WHERE status='judging' 把判题中的任务静默标成死信。白名单默认拒绝一切
    //   未知状态（fail-closed），枚举未来再新增也只会在 rescue 侧处理，不会
    //   从判题结果这条路写进来。
    switch (status) {
        case JudgeStatus::Accepted:
        case JudgeStatus::WrongAnswer:
        case JudgeStatus::TimeLimit:
        case JudgeStatus::MemoryLimit:
        case JudgeStatus::RuntimeError:
        case JudgeStatus::CompileError:
        case JudgeStatus::SystemError:
            break;  // 判题终态，放行
        default:
            if (err) *err = "finish 拒绝写非判题终态: " +
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

bool Db::fetch_limits(int64_t submission_id, int* time_ms, int* mem_mb, int* output_mb,
                      std::string* err, bool* found) {
    *found = false;
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    // LEFT JOIN：submission 一定存在（fetch_code 已确认），problem 可能缺失
    // （题目被删/没建）。限额属于题目——从 problem 表读，不再硬编码。
    // 全部用整数字面量拼 SQL：无用户输入，无注入面。
    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "SELECT p.time_limit_ms, p.mem_limit_mb, p.output_limit_mb "
                  "FROM submission s LEFT JOIN problem p ON s.problem_id = p.id "
                  "WHERE s.id=%lld",
                  (long long)submission_id);

    if (mysql_query(c, sql) != 0) {
        if (err) *err = mysql_error(c);
        return false;
    }
    MYSQL_RES* res = mysql_store_result(c);
    if (!res) {
        if (err) *err = mysql_error(c);
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        // 提交不存在（脏消息）——与 fetch_code 的 found=false 语义一致，调用方 ACK 丢弃
        mysql_free_result(res);
        return true;
    }

    // ★ 兜底策略（fail-safe）：题目缺失（LEFT JOIN 落空，三列全 NULL）时
    //   用默认限额继续判——判题不因元数据缺失而失败；found 仍置 true，
    //   让调用方继续正常流程（默认值与硬编码时代一致：1000ms/256MB/64MB）。
    *time_ms   = row[0] ? std::atoi(row[0]) : 1000;
    *mem_mb    = row[1] ? std::atoi(row[1]) : 256;
    *output_mb = row[2] ? std::atoi(row[2]) : 64;

    mysql_free_result(res);
    *found = true;
    return true;
}

bool Db::rescue_stale(int stale_minutes, int max_retry,
                      std::vector<int64_t>* reset_ids,
                      std::vector<int64_t>* dead_ids,
                      std::string* err) {
    reset_ids->clear();
    dead_ids->clear();
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    // 第一步：找出"judging 且超时"的任务（含 retry_count 供分类）
    // 判定条件 = status='judging' AND judge_started_at < NOW() - INTERVAL
    // 为什么是 judging 超时而不是全部卡住：pending 是正常待消费状态；
    // judging 超时才是"worker 抢了但没判完"（崩溃/DB 卡死）的现场。
    // stale_minutes 是整数 %d，无注入面。
    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "SELECT id, retry_count FROM submission "
                  "WHERE status='judging' AND judge_started_at < NOW() - INTERVAL %d MINUTE",
                  stale_minutes);

    if (mysql_query(c, sql) != 0) {
        if (err) *err = mysql_error(c);
        return false;
    }
    MYSQL_RES* res = mysql_store_result(c);
    if (!res) {
        if (err) *err = mysql_error(c);
        return false;
    }

    // 第二步：逐条分类处理。
    // 为什么逐条 UPDATE 而不是一条 CASE WHEN：
    //   - 分类（重试+1 vs 转死信）依赖每条自己的 retry_count，一条 SQL 要
    //     CASE 嵌套，可读性差；超时任务罕见，逐条开销可忽略
    // 每条 UPDATE 都带 WHERE status='judging'：复用"终态不可逆"防线——
    //   SELECT 与 UPDATE 之间任务可能已被 finish 写终态（慢 worker 恰好在
    //   超时边缘完成），affected==0 是良性（任务已处理），不加入结果列表。
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        const int64_t id = row[0] ? std::strtoll(row[0], nullptr, 10) : 0;
        const int retry = row[1] ? std::atoi(row[1]) : 0;

        if (retry < max_retry) {
            // 重置 pending + retry_count 原子自增（记录"被判死过一次"）。
            // 重置后消息重投（XAUTOCLAIM 或重新入队）才能再次 claim 成功。
            char up[256];
            std::snprintf(up, sizeof(up),
                          "UPDATE submission SET status='pending', retry_count=retry_count+1 "
                          "WHERE id=%lld AND status='judging'",
                          (long long)id);
            if (mysql_query(c, up) != 0) {
                if (err) *err = mysql_error(c);
                mysql_free_result(res);
                return false;
            }
            if (mysql_affected_rows(c) == 1) reset_ids->push_back(id);
        } else {
            // 重试超限 → 死信：不再自动捞，等人工介入（无限重试=资源黑洞）。
            // ★ 用枚举映射而非裸字符串 'dead'（P1-2）：dead 已进 JudgeStatus
            //   枚举，状态机只有枚举一种权威来源；裸字符串会让 API 层查这个
            //   状态时只能拿到字符串或落到 unknown。
            const std::string dead_st = judge_status_to_string(JudgeStatus::Dead);
            char up[256];
            std::snprintf(up, sizeof(up),
                          "UPDATE submission SET status='%s' "
                          "WHERE id=%lld AND status='judging'",
                          dead_st.c_str(), (long long)id);
            if (mysql_query(c, up) != 0) {
                if (err) *err = mysql_error(c);
                mysql_free_result(res);
                return false;
            }
            if (mysql_affected_rows(c) == 1) dead_ids->push_back(id);
        }
    }
    mysql_free_result(res);
    return true;
}

bool Db::fetch_undelivered(int limit, std::vector<OutboxEntry>* out, std::string* err) {
    out->clear();
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    // ★ BEGIN：FOR UPDATE 的锁必须跨语句持有。
    //   自动提交模式下锁随 SELECT 语句结束立即释放 → 两个投递器仍能拿到
    //   同一行 → SKIP LOCKED 形同虚设。显式事务把"取行→投递→标记"串成
    //   一个持锁区间，锁在 COMMIT/ROLLBACK 才释放。
    if (mysql_query(c, "BEGIN") != 0) {
        if (err) *err = mysql_error(c);
        return false;
    }

    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "SELECT id, submission_id FROM outbox WHERE delivered=0 "
                  "ORDER BY id LIMIT %d FOR UPDATE SKIP LOCKED",
                  limit);

    if (mysql_query(c, sql) != 0) {
        if (err) *err = mysql_error(c);
        mysql_query(c, "ROLLBACK");  // 出错回滚，不残留事务态
        return false;
    }
    MYSQL_RES* res = mysql_store_result(c);
    if (!res) {
        if (err) *err = mysql_error(c);
        mysql_query(c, "ROLLBACK");
        return false;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        OutboxEntry e;
        e.id = row[0] ? std::strtoll(row[0], nullptr, 10) : 0;
        e.submission_id = row[1] ? std::strtoll(row[1], nullptr, 10) : 0;
        out->push_back(e);
    }
    mysql_free_result(res);

    if (out->empty()) {
        // 没拿到行就没有锁可持，立即结束事务（避免空事务挂着连接）
        mysql_query(c, "COMMIT");
    }
    // 非空：锁保持，等调用方 mark_delivered（COMMIT）或 rollback（释放）
    return true;
}

bool Db::mark_delivered(int64_t outbox_id, std::string* err) {
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    // 条件更新：WHERE delivered=0 保证"标记"幂等（重复标记影响 0 行）。
    // ★ P0-1 修复：本方法**不**结束事务（不 COMMIT）——FOR UPDATE 锁必须
    //   跨整批持有。若逐行 COMMIT，第一条就释放全部锁，另一投递器 SKIP
    //   LOCKED 不再跳过任何行 → 拿到同批剩余行 → 同一 submission 重复 XADD。
    //   整批投递完成后由调用方统一 commit()；任一条失败 rollback()。
    char sql[128];
    std::snprintf(sql, sizeof(sql),
                  "UPDATE outbox SET delivered=1 WHERE id=%lld AND delivered=0",
                  (long long)outbox_id);
    if (mysql_query(c, sql) != 0) {
        if (err) *err = mysql_error(c);
        return false;
    }
    return true;
}

bool Db::commit(std::string* err) {
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    // 整批投递完成：统一 COMMIT，释放 fetch_undelivered 持有的 FOR UPDATE 锁。
    if (mysql_query(static_cast<MYSQL*>(conn_), "COMMIT") != 0) {
        if (err) *err = mysql_error(static_cast<MYSQL*>(conn_));
        return false;
    }
    return true;
}

bool Db::rollback(std::string* err) {
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    // XADD 失败路径：回滚释放 FOR UPDATE 锁，下轮投递器重试。
    if (mysql_query(static_cast<MYSQL*>(conn_), "ROLLBACK") != 0) {
        if (err) *err = mysql_error(static_cast<MYSQL*>(conn_));
        return false;
    }
    return true;
}

bool Db::revert_reset(int64_t submission_id, std::string* err) {
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    // P0-2 修复：rescue 重置 pending 后重新入队（XADD）失败时，把状态回滚成
    // judging（retry_count 还原 -1），下一轮 rescue 的 SELECT（judging 且超时）
    // 会重新选中它——否则状态停在 pending 且消息已不在 PEL，永久无人消费。
    // WHERE status='pending' 防竞态：重置后若已被其他 worker claim 走
    //   （status 已变 judging/终态），匹配 0 行，不误伤正在判题的任务。
    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "UPDATE submission SET status='judging', retry_count=retry_count-1 "
                  "WHERE id=%lld AND status='pending'",
                  (long long)submission_id);

    if (mysql_query(c, sql) != 0) {
        if (err) *err = mysql_error(c);
        return false;
    }
    return true;
}

bool Db::cleanup_outbox(int64_t* deleted_rows, std::string* err) {
    if (!conn_) {
        if (err) *err = "未连接 MySQL";
        return false;
    }
    MYSQL* c = static_cast<MYSQL*>(conn_);

    // N4（计划 :722）：已投递记录保留 7 天后清理。
    // - 只删 delivered=1：未投递的可能因 Redis 故障一直没发出去，
    //   delivered=0 是"待办"不是"垃圾"，删了 = 任务永久丢失。
    // - 时间判断交给 MySQL（DATE_SUB(NOW(), INTERVAL 7 DAY)）：
    //   无注入面（无用户输入拼接），worker 不维护自己的时钟。
    const char* sql =
        "DELETE FROM outbox WHERE delivered=1 "
        "AND created_at < DATE_SUB(NOW(), INTERVAL 7 DAY)";
    if (mysql_query(c, sql) != 0) {
        if (err) *err = std::string("cleanup_outbox: ") + mysql_error(c);
        return false;
    }
    if (deleted_rows) {
        *deleted_rows = static_cast<int64_t>(mysql_affected_rows(c));
    }
    return true;
}

}  // namespace worker
