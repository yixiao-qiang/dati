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

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
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

    // 往返一致性：DB 里存的是字符串，读回要能映射回同一枚举
    //   （finish 写 st=to_string(status)，rescue 读 status 列按字符串匹配）
    CHECK(worker::parse_status(worker::judge_status_to_string(JudgeStatus::Accepted)) == JudgeStatus::Accepted);
    CHECK(worker::parse_status(worker::judge_status_to_string(JudgeStatus::Dead)) == JudgeStatus::SystemError);
    // 注意：Dead 字符串 "dead" 不在 parse_status 白名单里 → 落 SystemError，
    //   这符合设计（parse_status 只认 judge_main 输出；dead 是 DB 侧状态，
    //   不会从判题结果路径进来）。
}

}  // namespace

int main() {
    test_truncate_utf8();
    test_escape_sql();
    test_parse_status();
    test_judge_status_to_string();

    if (g_failures == 0) {
        std::printf("\nworker_tests: 全部通过 ✅（%d 断言，0 失败）\n", 0);
        std::printf("（断言数未统计计数宏；失败数=0）\n");
        return 0;
    }
    std::fprintf(stderr, "\nworker_tests: %d 个断言失败 ❌\n", g_failures);
    return 1;
}
