// Wave 2 · worker 内部纯函数单元测试（D4，方案 A）
//
// 测试对象（声明见 src/internal.h，生产代码与测试共用同一份定义）：
//   truncate_utf8 / escape_sql / parse_status / judge_status_to_string
//
// 为什么自写断言宏而不是 gtest：
//   VM 无 gtest；断言宏 20 行足够覆盖"纯函数输入输出"场景，
//   不引入第三方依赖。失败时打印 文件:行:表达式 并累计，main 返回非 0。
//
// 运行（CMake 目标 worker_tests）：
//   cd ~/work/dati/worker && cmake -B build && cmake --build build
//   ./build/worker_tests
//   退出码 0 = 全部通过；非 0 = 有失败（stderr 有 FAIL 行）。

#include <cstdio>
#include <cstring>
#include <string>

#include <mysql/mysql.h>

#include "../src/internal.h"
#include "../src/db.h"
#include "../src/judge.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (cond) {                                                      \
            std::printf("PASS %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        } else {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

void test_truncate_utf8() {
    // 短串/恰好等于限长：不截断
    CHECK(worker::truncate_utf8("hello", 100) == "hello");
    CHECK(worker::truncate_utf8("", 10) == "");
    CHECK(worker::truncate_utf8("abc", 3) == "abc");
    CHECK(worker::truncate_utf8("abc", 0) == "");

    // 长 ASCII：按字节边界切
    CHECK(worker::truncate_utf8(std::string(10, 'a'), 5) == "aaaaa");

    // UTF-8 边界："你"占 3 字节（E4 BD A0），切口落在字符内部必须回退
    const std::string cn = "你好";  // 6 字节：你(3) + 好(3)
    CHECK(worker::truncate_utf8(cn, 3) == "你");   // 恰好整字符边界
    CHECK(worker::truncate_utf8(cn, 2) == "");     // 切口在"你"中间 → 回退到 0
    CHECK(worker::truncate_utf8(cn, 1) == "");
    CHECK(worker::truncate_utf8(cn, 4) == "你");   // 4 = 你(3) + 好首字节 → 回退 1
    CHECK(worker::truncate_utf8(cn, 6) == cn);     // 恰好全长

    // 混合：ASCII 前缀 + 中文，切口落在中文中间
    const std::string mix = "ab你cd";  // 2 + 3 + 2 = 7 字节
    CHECK(worker::truncate_utf8(mix, 5) == "ab你");  // 5 = ab(2) + 你(3)
    CHECK(worker::truncate_utf8(mix, 4) == "ab");    // 4 落在"你"中间 → 回退到 2
}

void test_escape_sql() {
    // 防御分支（无连接）：nullptr 与空串
    CHECK(worker::escape_sql(nullptr, "") == "");
    CHECK(worker::escape_sql(nullptr, "x") == "");

    // 真实转义：mysql_init 句柄即可调 mysql_real_escape_string，无需真连库。
    // 设置字符集避免依赖连接默认值（utf8mb4 与生产一致）。
    MYSQL* c = mysql_init(nullptr);
    if (c) {
        mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        CHECK(worker::escape_sql(c, "") == "");
        CHECK(worker::escape_sql(c, "it's") == "it\\'s");
        CHECK(worker::escape_sql(c, "a\"b") == "a\\\"b");
        CHECK(worker::escape_sql(c, "back\\slash") == "back\\\\slash");
        // 内嵌 NUL：strlen 语义会截断，必须按长度转义成 \0（2 字符）
        const std::string nul("a\0b", 3);
        CHECK(worker::escape_sql(c, nul) == "a\\0b");
        mysql_close(c);
    } else {
        // 环境没有 mysqlclient 时跳过真实转义部分（防御性降级，不判失败）
        std::fprintf(stderr, "SKIP mysql_init 失败（无 mysqlclient？）—— 真实转义用例跳过\n");
    }
}

void test_parse_status() {
    using worker::JudgeStatus;
    CHECK(worker::parse_status("OK") == JudgeStatus::Accepted);
    CHECK(worker::parse_status("WA") == JudgeStatus::WrongAnswer);
    CHECK(worker::parse_status("TLE") == JudgeStatus::TimeLimit);
    CHECK(worker::parse_status("MLE") == JudgeStatus::MemoryLimit);
    CHECK(worker::parse_status("RE") == JudgeStatus::RuntimeError);
    // ★ runner 实际输出全名（status_to_string）：RE 程序/exit≠0 程序（Wave 1 遗留
    //   漏映射，e2e RE 场景首测暴露——只认短码会把 RUNTIME_ERROR 落成 SystemError）
    CHECK(worker::parse_status("RUNTIME_ERROR") == JudgeStatus::RuntimeError);
    CHECK(worker::parse_status("NONZERO_EXIT") == JudgeStatus::RuntimeError);
    // OLE 输出超限：DB 无独立枚举，归 WrongAnswer（与前端 8 终态词汇表一致）
    CHECK(worker::parse_status("OLE") == JudgeStatus::WrongAnswer);
    CHECK(worker::parse_status("CE") == JudgeStatus::CompileError);
    CHECK(worker::parse_status("INTERNAL_ERROR") == JudgeStatus::SystemError);
    // fail-closed：未知/空串都落 SystemError（读不懂就当系统错误，不猜）
    CHECK(worker::parse_status("BOGUS") == JudgeStatus::SystemError);
    CHECK(worker::parse_status("") == JudgeStatus::SystemError);
}

void test_judge_status_to_string() {
    using worker::JudgeStatus;
    // 每个枚举值有唯一字符串（含 D4 补的 Dead——rescue 死信写入依赖它）
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::Pending)) == "pending");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::Judging)) == "judging");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::Accepted)) == "accepted");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::WrongAnswer)) == "wrong_answer");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::TimeLimit)) == "tle");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::MemoryLimit)) == "mle");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::RuntimeError)) == "runtime_error");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::CompileError)) == "compile_error");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::SystemError)) == "system_error");
    CHECK(std::string(worker::judge_status_to_string(JudgeStatus::Dead)) == "dead");

    // 往返一致性（真实生产链路）：judge_main 输出 → parse_status → 枚举 →
    //   judge_status_to_string → DB 字符串。
    // ★ 注意：parse_status 与 judge_status_to_string **不是互逆函数**——
    //   两套词汇表不同（judge_main 输出 "OK" vs DB 存 "accepted"），
    //   parse_status 只认 judge_main 输出。旧断言
    //   parse_status(judge_status_to_string(Accepted)) == Accepted 是错的
    //   （VM 实测 FAIL，见提交记录），已修正为下方全链路断言。
    // ★ 必须包 std::string(...) 再比较：judge_status_to_string 返回 const char*，
    //   裸指针 == 字符串字面量是**指针地址比较**（不是内容比较）——"accepted"
    //   恰好被字符串池合并到同地址所以碰巧 PASS，其余全部 FAIL（VM 实测教训）。
    CHECK(std::string(worker::judge_status_to_string(worker::parse_status("OK"))) == "accepted");
    CHECK(std::string(worker::judge_status_to_string(worker::parse_status("WA"))) == "wrong_answer");
    CHECK(std::string(worker::judge_status_to_string(worker::parse_status("TLE"))) == "tle");
    CHECK(std::string(worker::judge_status_to_string(worker::parse_status("MLE"))) == "mle");
    CHECK(std::string(worker::judge_status_to_string(worker::parse_status("RE"))) == "runtime_error");
    CHECK(std::string(worker::judge_status_to_string(worker::parse_status("CE"))) == "compile_error");
    CHECK(std::string(worker::judge_status_to_string(worker::parse_status("INTERNAL_ERROR"))) == "system_error");
    // Dead：DB 侧状态，不在 parse_status 白名单 → SystemError（两层语义隔离）
    CHECK(worker::parse_status(worker::judge_status_to_string(JudgeStatus::Dead)) == JudgeStatus::SystemError);
}

void test_json_fields() {
    using worker::json_field_str;
    using worker::json_field_num;
    // judge_main 真实输出形状（冒号后带空格）
    const std::string j =
        "{"
        "\"status\": \"OK\", "
        "\"exit_code\": 0, "
        "\"signal\": 9, "
        "\"cpu_us\": 123456, "
        "\"stdout\": \"say \\\"hi\\\"\", "
        "\"stderr\": \"\", "
        "\"error_msg\": \"\""
        "}";

    CHECK(json_field_str(j, "status") == "OK");
    CHECK(json_field_num(j, "exit_code") == "0");
    CHECK(json_field_num(j, "signal") == "9");
    CHECK(json_field_num(j, "cpu_us") == "123456");
    // ★ 审查 P1-1：值内转义引号不得截断——旧子串解析取到 "say \" → 必误判 WA
    CHECK(json_field_str(j, "stdout") == "say \"hi\"");
    CHECK(json_field_str(j, "stderr") == "");

    // 值内出现"伪键值对"文本：顺序解析不会把值内文本当键
    const std::string tricky =
        "{\"status\": \"OK\", \"stdout\": \"x \\\"status\\\": \\\"WA\\\" y\", "
        "\"error_msg\": \"e\"}";
    CHECK(json_field_str(tricky, "stdout") == "x \"status\": \"WA\" y");
    CHECK(json_field_str(tricky, "error_msg") == "e");
    CHECK(json_field_str(tricky, "status") == "OK");

    // 换行/制表转义还原
    CHECK(json_field_str("{\"stdout\": \"a\\nb\\tc\"}", "stdout") == "a\nb\tc");

    // 缺失字段 / 非法 JSON → 空串（fail-closed，parse_status 落 SystemError）
    CHECK(json_field_str("{\"status\": \"OK\"}", "stdout") == "");
    CHECK(json_field_str("not json at all", "status") == "");
    CHECK(json_field_num("{\"signal\": 7}", "exit_code") == "");
    CHECK(json_field_str("{\"stdout\": \"unterminated", "stdout") == "");

    // 负数与空格容错
    CHECK(json_field_num("{\"exit_code\": -1, \"signal\": 0}", "exit_code") == "-1");
}

void test_output_matches() {
    // 精确相等
    CHECK(worker::output_matches("hi\n", "hi\n"));
    // 尾部换行不同（常见：程序 printf 自带 \n，期望串没有）
    CHECK(worker::output_matches("hi\n", "hi"));
    // 尾部空格
    CHECK(worker::output_matches("hi ", "hi"));
    // 尾部 \r\n
    CHECK(worker::output_matches("hi\r\n", "hi"));
    // 内容不同 → false
    CHECK(!worker::output_matches("hi", "ho"));
    // 多行：行内差异必须判 false
    CHECK(!worker::output_matches("a\nb\n", "a\nc"));
    // 空 vs 空
    CHECK(worker::output_matches("", ""));
    // 空 vs 纯空白：rtrim 后都是空 → 相等
    CHECK(worker::output_matches("\n", ""));
}

void test_judge_outcome_default() {
    // K7 契约：JudgeOutcome 默认构造必须是 SystemError（fail-closed）。
    // 起因：status 原是未初始化枚举——judge_code 内部故障提前 return 时
    // 留下垃圾值，传给 db.finish() 被白名单拒绝 → finish 失败 → 不 ACK
    // → 消息无限重投。judge.h 已加默认成员初始化，此断言锁住它不被回退。
    // 注意配套约束：编译成功路径必须显式重置 status（见 judge.cpp 的
    // Pending 重置）——默认 SystemError 与"中性中间态"是成对设计。
    worker::JudgeOutcome out;
    CHECK(out.status == worker::JudgeStatus::SystemError);
    // 附带契约：计数字段默认 0（台阶 2/3 的落库字段）
    CHECK(out.passed_cases == 0);
    CHECK(out.total_cases == 0);
}

void test_runner_status_vocabulary_contract() {
    // Wave 5 台阶 1 契约：runner 输出的每个状态，parse_status 必须给出
    // 明确映射（不落 default SystemError）。唯一例外是 INTERNAL_ERROR
    // 本身——它映射 SystemError 是合法行为（runner 内部错误=系统错误）。
    using worker::JudgeStatus;
    for (const char* s : worker::kRunnerStatusVocabulary) {
        JudgeStatus st = worker::parse_status(s);
        if (std::string(s) == "INTERNAL_ERROR") {
            CHECK(st == JudgeStatus::SystemError);
        } else {
            CHECK(st != JudgeStatus::SystemError);
        }
    }
    // 关键映射钉死（与 1ae0449 修复一致）：
    // runner 输出全名，parse_status 认短码又认全名（历史兼容）
    CHECK(worker::parse_status("OK") == JudgeStatus::Accepted);
    CHECK(worker::parse_status("NONZERO_EXIT") == JudgeStatus::RuntimeError);
    CHECK(worker::parse_status("TLE") == JudgeStatus::TimeLimit);
    CHECK(worker::parse_status("MLE") == JudgeStatus::MemoryLimit);
    CHECK(worker::parse_status("RUNTIME_ERROR") == JudgeStatus::RuntimeError);
    CHECK(worker::parse_status("OLE") == JudgeStatus::WrongAnswer);
    CHECK(worker::parse_status("INTERNAL_ERROR") == JudgeStatus::SystemError);
}

}  // namespace

int main() {
    test_truncate_utf8();
    test_escape_sql();
    test_parse_status();
    test_judge_status_to_string();
    test_json_fields();
    test_output_matches();
    test_judge_outcome_default();
    test_runner_status_vocabulary_contract();

    if (g_failures == 0) {
        std::printf("\nworker_tests: 全部通过 ✅（%d 断言，0 失败）\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "\nworker_tests: %d 个断言失败 ❌\n", g_failures);
    return 1;
}
