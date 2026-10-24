// Wave 2 · worker 内部纯函数（D4：导出供单元测试）
//
// 为什么单独建 internal.h 而不是直接测 .cpp：
//   - 这些函数原本在匿名 namespace（编译单元私有），测试文件无法引用；
//   - 方案 A：把它们导出到 worker 命名空间，生产代码与测试共用同一份定义
//     （测试 include 声明，不是复制实现——改实现测试自然跟着过/挂）；
//   - 文件名叫 internal 表明"非对外 API"：只给 worker 内部和测试用，
//     Wave 3 server 侧不要 include 它。
//
// 纯函数判定（可单测的边界）：
//   truncate_utf8 / parse_status / judge_status_to_string：无外部依赖，纯逻辑；
//   escape_sql：依赖 MYSQL 句柄——测试覆盖空串/nullptr 防御分支与真实转义
//   （mysql_init 一个未连接句柄即可调 mysql_real_escape_string，无需真连库）。

#pragma once

#include <cstddef>
#include <string>

#include "db.h"  // JudgeStatus 枚举 + judge_status_to_string 声明

namespace worker {

// 按 UTF-8 字符边界截断到 max_bytes。
// 为什么不能直接 substr(0, N)：用户程序输出可能含多字节 UTF-8，从中间切断
// 会留下半个字符，前端渲染成乱码。判据：续字节高 2 位是 10（0x80~0xBF），
// 若被丢弃部分的第一个字节是续字节，说明切口落在字符内部，往前退到边界。
std::string truncate_utf8(const std::string& s, size_t max_bytes);

// MySQL 字符串字面量转义。空串返回空串（无需转义）；
// conn 为 nullptr（无连接）时返回空串（防御，正常调用方不会走到）。
// 生产路径的调用方（finish）会先判断连接，此分支仅为单测暴露的契约。
std::string escape_sql(void* conn, const std::string& raw);

// judge_main 状态字符串 → 枚举（"OK"→Accepted ...）。
// 未知字符串 → SystemError（fail-closed：读不懂就当系统错误，不猜）。
JudgeStatus parse_status(const std::string& s);

bool output_matches(const std::string& expected, const std::string& actual);

// ★ runner → parse_status 词汇表契约（Wave 5 台阶 1 契约测试用）。
// 权威源：sandbox/src/runner.cpp:134 status_to_string——runner 输出的全部
// 状态。parse_status 必须能认这套全集，否则 runner 新状态会落 default
// → SystemError（1ae0449 教训：曾只认短码 RE，RUNTIME_ERROR 误判 system_error）。
// 加新状态必须三处同步：runner status_to_string + 本数组 + parse_status。
// 为什么 inline + constexpr：头文件变量必须有内联定义，多编译单元包含
// 才不触发 ODR 重复定义（C++17 inline 变量规则，面试可能问）。
inline const char* const kRunnerStatusVocabulary[] = {
    "OK", "NONZERO_EXIT", "TLE", "MLE",
    "RUNTIME_ERROR", "OLE", "INTERNAL_ERROR",
};
inline constexpr size_t kRunnerStatusVocabularySize = 7;

// 扁平 JSON 对象字段提取（judge_main 输出的解析，顺序解析、转义正确）。
// json_field_str 返回已 unescape 的字符串值；json_field_num 返回数字的
// 原始文本（调用方 atoi/atoll）。字段缺失/JSON 非法 → 空串（fail-closed：
// status 解析不出 → parse_status("") → SystemError，不猜）。
// ★ 为什么导出：旧的子串查找在值内第一个转义引号处截断（输出含 " 必误判
//   WA，审查 P1-1），解析器是判题正确性的关键纯函数，必须有单测锁死。
std::string json_field_str(const std::string& json, const std::string& key);
std::string json_field_num(const std::string& json, const std::string& key);
}  // namespace worker
