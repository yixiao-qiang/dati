// Wave 1 · 测试 00：状态枚举与字符串映射回归（不需要 root）
// 验收：SandboxConfig / JudgeResult 的默认值 + status_to_string 全枚举一一对应
//
// 为什么单独放一个不需要 root 的测试：
//   其余冒烟（01/02）都要 root + namespace + cgroup，而 CI / 非 root 环境
//   一条都跑不了，导致「接口层」从未被任何自动化覆盖。本测试只碰纯逻辑，
//   任何人任何环境都能跑，把最外层的不变量钉住。
//
// 重点盯 OLE：output_limit_mb 与 Status::OLE 在头文件里声明过，
//   但它是否真的有对应实现、字符串映射是否漏项，只有这个测试能发现。
#include "sandbox.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0, g_fail = 0;
static void ok(const char* m)  { std::printf("  [PASS] %s\n", m); ++g_pass; }
static void bad(const char* m) { std::printf("  [FAIL] %s\n", m); ++g_fail; }

int main() {
    std::printf("== 测试 00：状态枚举与默认配置（无 root 可跑）==\n");

    // ---------- 1. status_to_string 全枚举一一对应 ----------
    // 判题状态是这个沙箱对外的唯一出口：Worker 拿它写数据库、
    // 前端拿它显示结果。漏映射 = 用户看到乱码或空状态。
    struct { sandbox::Status s; const char* expect; } cases[] = {
        { sandbox::Status::OK,             "OK" },
        { sandbox::Status::NONZERO_EXIT,   "NONZERO_EXIT" },
        { sandbox::Status::TLE,            "TLE" },
        { sandbox::Status::MLE,            "MLE" },
        { sandbox::Status::RUNTIME_ERROR,  "RUNTIME_ERROR" },
        { sandbox::Status::OLE,            "OLE" },
        { sandbox::Status::INTERNAL_ERROR, "INTERNAL_ERROR" },
    };
    for (const auto& c : cases) {
        const char* got = sandbox::status_to_string(c.s);
        if (got != nullptr && std::strcmp(got, c.expect) == 0) {
            ok((std::string("status_to_string 映射正确: ") + c.expect).c_str());
        } else {
            bad(("status_to_string 映射错误，期望 " + std::string(c.expect) +
                 "，实际 " + std::string(got ? got : "(nullptr)")).c_str());
        }
    }

    // ---------- 2. Oracle：如果枚举加了新值而 switch 没跟上，这里会红 ----------
    // status_to_string 的 switch 末尾 return "UNKNOWN" —— 新增枚举值时
    // 编译器不报错，只有这个测试能抓到"新状态没有对应字符串"。
    {
        sandbox::Status all[] = {
            sandbox::Status::OK, sandbox::Status::NONZERO_EXIT,
            sandbox::Status::TLE, sandbox::Status::MLE,
            sandbox::Status::RUNTIME_ERROR, sandbox::Status::OLE,
            sandbox::Status::INTERNAL_ERROR,
        };
        for (auto s : all) {
            const char* got = sandbox::status_to_string(s);
            if (got != nullptr && std::strcmp(got, "UNKNOWN") == 0) {
                bad("存在未映射到具体字符串的 Status 值（落到了 UNKNOWN 兜底）");
            }
        }
        ok("已确认 switch 覆盖上面列出的全部枚举（无 UNKNOWN 兜底）");
    }

    // ---------- 3. JudgeResult 默认值必须是 INTERNAL_ERROR ----------
    // 面试要点：判题结果默认值取"内部错误"而非 OK——空结果被当成通过，
    // 是判题系统最危险的一类 bug（把失败标成成功）。
    {
        sandbox::JudgeResult r;
        if (r.status == sandbox::Status::INTERNAL_ERROR) {
            ok("JudgeResult 默认状态 = INTERNAL_ERROR（默认不判通过）");
        } else {
            bad("JudgeResult 默认状态不是 INTERNAL_ERROR");
        }
        if (r.exit_code == -1 && r.signal == 0 &&
            r.cpu_time_us == 0 && r.memory_peak_bytes == 0) {
            ok("JudgeResult 计量字段默认归零");
        } else {
            bad("JudgeResult 计量字段默认值异常");
        }
    }

    // ---------- 4. SandboxConfig 默认值 ----------
    // output_limit_mb 默认 64：对应计划任务 1.5 的「打印 128MB → OLE」。
    // 如果有一天它被改成 0，OLE 会把所有输出都截断——这里先钉住默认值。
    {
        sandbox::SandboxConfig c;
        if (c.output_limit_mb == 64) {
            ok("SandboxConfig::output_limit_mb 默认 = 64（计划任务 1.5 口径）");
        } else {
            bad(("SandboxConfig::output_limit_mb 默认值漂移，实际 " +
                 std::to_string(c.output_limit_mb)).c_str());
        }
        if (c.time_limit_ms == 1000 && c.memory_limit_mb == 256 &&
            c.pids_limit == 64) {
            ok("SandboxConfig 其余默认值符合设计（time/mem/pids）");
        } else {
            bad("SandboxConfig 其余默认值漂移");
        }
    }

    std::printf("汇总：PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
