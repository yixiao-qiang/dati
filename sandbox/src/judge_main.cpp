#include "sandbox.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// 把字符串转义成合法的 JSON 字符串内容
// 必须处理：\ " \n \r \t（否则用户程序输出含引号/换行时 JSON 会坏）
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    // 其他控制字符：\u00XX
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

int main(int argc, char* argv[]) {
    // 参数：argv[1]=二进制路径 argv[2]=时间ms argv[3]=内存MB argv[4]=输出MB
    if (argc != 5) {
        std::fprintf(stderr, "用法: %s <binary> <time_ms> <mem_mb> <output_mb>\n", argv[0]);
        return 1;
    }

    sandbox::SandboxConfig cfg;
    cfg.binary_path = argv[1];
    cfg.time_limit_ms = std::atoi(argv[2]);
    cfg.memory_limit_mb = std::atoi(argv[3]);
    cfg.output_limit_mb = std::atoi(argv[4]);
    cfg.input = "";
    cfg.pids_limit = 64;
    cfg.use_rootfs = true;

    sandbox::JudgeResult result = sandbox::run(cfg);

    // 输出 JSON 到 stdout（一行，Python json.loads() 可直接解析）
    std::printf("{");
    std::printf("\"status\": \"%s\",", sandbox::status_to_string(result.status));
    std::printf("\"exit_code\": %d,", result.exit_code);
    std::printf("\"signal\": %d,", result.signal);
    std::printf("\"cpu_us\": %ld,", (long)result.cpu_time_us);
    std::printf("\"mem_bytes\": %ld,", (long)result.memory_peak_bytes);
    std::printf("\"wall_us\": %ld,", (long)result.wall_time_us);
    std::printf("\"stdout\": \"%s\",", json_escape(result.stdout_output).c_str());
    std::printf("\"stderr\": \"%s\",", json_escape(result.stderr_output).c_str());
    // error_msg：沙箱内部失败原因（如 cgroup 创建失败、clone 失败），
    // 必须带出给前端——否则用户只看到 INTERNAL_ERROR，不知道怎么办
    std::printf("\"error_msg\": \"%s\"", json_escape(result.error_msg).c_str());
    std::printf("}\n");

    return 0;
}
