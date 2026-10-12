// Wave 2 · judge.cpp —— 判题流水线（台阶 1 实现）
//
// 流程：编译（沙箱外 g++）→ 运行（沙箱内 judge_main）→ 解析 JSON
//
// 面试知识点：
// - 编译在沙箱外、运行在沙箱内：编译器需要读写/exec，会被 seccomp 拦；
//   judge_main 自己建沙箱关用户程序（Wave 1 产物）
// - fork+exec：exec 成功不返回，失败必须 _exit(127)（否则子进程跑父进程代码）
// - 管道：父进程必须关写端（否则 read 永远不 EOF）；先 waitpid 再读会死锁
// - 超时：非阻塞轮询 waitpid(WNOHANG) + 墙钟；杀后必须 waitpid 收尸防僵尸
// - 谁创建谁清理：src/bin 都是本文件创建的临时文件，本文件负责删除

#include "judge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace worker {

namespace {

// ================= 工具：status string → 枚举 =================
// judge_main 输出 "OK"/"WA"/"TLE"...，映射到 db.h 的枚举（与 finish 对齐）
// 未知字符串 → SystemError（fail-closed：读不懂就当系统错误，不猜）
JudgeStatus parse_status(const std::string& s) {
    if (s == "OK")              return JudgeStatus::Accepted;
    if (s == "WA")              return JudgeStatus::WrongAnswer;
    if (s == "TLE")             return JudgeStatus::TimeLimit;
    if (s == "MLE")             return JudgeStatus::MemoryLimit;
    if (s == "RE")              return JudgeStatus::RuntimeError;
    if (s == "CE")              return JudgeStatus::CompileError;
    if (s == "INTERNAL_ERROR")  return JudgeStatus::SystemError;
    return JudgeStatus::SystemError;
}

// 当前墙钟（毫秒）——超时判断用
int64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

// ================= 工具：JSON 字段提取（学习期简化）=================
// 提取字符串字段："key" : "value"（容忍冒号后空格——
//  judge_main 输出 {"status": "OK"}，冒号后有空格，旧写法匹配不上）
std::string json_str(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < json.size() && (json[p] == ':' || json[p] == ' ')) ++p;  // 跳过 : 和空格
    if (p >= json.size() || json[p] != '"') return "";
    ++p;  // 跳过开引号
    size_t e = json.find('"', p);
    if (e == std::string::npos) return "";
    return json.substr(p, e - p);
}

// 提取数字字段："key":123（容忍冒号后空格）
std::string json_num(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < json.size() && (json[p] == ':' || json[p] == ' ')) ++p;  // 跳过 : 和空格
    size_t e = json.find_first_of(",}", p);
    if (e == std::string::npos) return "";
    return json.substr(p, e - p);
}

// 最简 unescape：把 JSON 转义还原成原始文本（学习期只处理常见的四种）
// 差距说明（面试点）：生产级用 nlohmann/json，转义/嵌套/类型安全全包
std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                default: out += c; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// ================= 核心：fork + exec + 管道 + 超时 =================
// 编译 g++ 和运行 judge_main 复用同一个模式（DRY）
// 返回：0=正常退出  1=超时被杀  -1=内部故障（pipe/fork 失败）
// 输出：output（stdout 全文）、status_out（waitpid 状态）
// capture_stderr：true=stderr 合并进管道（编译用，错误信息要捕获）；
//                 false=stderr 继承父进程（运行 judge_main 用，
//                       JSON 只从 stdout 读，自身 stderr 直接上 worker 日志）
int run_capture(const std::vector<std::string>& argv, int timeout_ms,
                bool capture_stderr,
                std::string* output, int* status_out, std::string* err) {
    // ① 管道：子进程 stdout → 父进程
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (err) *err = std::string("pipe 失败: ") + std::strerror(errno);
        return -1;
    }

    // ② fork
    pid_t pid = fork();
    if (pid < 0) {
        if (err) *err = std::string("fork 失败: ") + std::strerror(errno);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        // 子进程：stdout 重定向到管道写端，然后 exec 替换
        dup2(pipefd[1], STDOUT_FILENO);
        if (capture_stderr) dup2(pipefd[1], STDERR_FILENO);  // 合并 stderr
        close(pipefd[0]);
        close(pipefd[1]);
        // vector<string> → char* 数组（execvp 需要，末尾 nullptr）
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);  // ★ exec 失败才到这。用 _exit 不走 atexit/析构
                     //   （fork 副本跑析构会 double-free）
    }

    // 父进程：
    // ③ 读端设非阻塞 —— 否则 read 阻塞时超时检查做不了
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    close(pipefd[1]);  // ★ 必须关写端，否则 read 永远等不到 EOF

    std::string out;
    char buf[4096];
    const int64_t start = now_ms();
    int status = 0;
    bool done = false;
    bool timed_out = false;

    while (!done) {
        // ④ 非阻塞检查子进程状态
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) done = true;              // 子进程退出
        else if (w < 0 && errno != EINTR) done = true;  // ECHILD 等错误

        // ⑤ 非阻塞读管道（能读多少读多少；EAGAIN=暂时没数据）
        for (;;) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n > 0) out.append(buf, static_cast<size_t>(n));
            else break;  // n==0 EOF；n<0 EAGAIN 或出错
        }

        if (!done) {
            // ⑥ 超时检查：墙钟到了就杀
            if (now_ms() - start > timeout_ms) {
                timed_out = true;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);  // ★ 收尸，防僵尸
                done = true;
            } else {
                usleep(5000);  // 5ms 轮询间隔（学习期够用）
            }
        }
    }

    // ⑦ 子进程已退出：写端已关，把管道剩余数据读完（防丢尾）
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) out.append(buf, static_cast<size_t>(n));
        else break;
    }
    close(pipefd[0]);

    if (output) *output = std::move(out);
    if (status_out) *status_out = status;
    return timed_out ? 1 : 0;
}

// ================= 步骤 1：编译（沙箱外）=================
// 返回 true = 编译流程走完（成功 / CE / 超时，outcome->status 区分）
// 返回 false = 内部故障（写文件/管道失败），调用方应返回 false
// CE 不是调用错误：用户代码写错是正常判题结果，不是 worker 故障
bool compile_code(const std::string& code, const std::string& lang,
                  std::string* out_bin, std::string* compiler_stderr,
                  JudgeOutcome* outcome, std::string* err) {
    // ① 唯一临时文件名：pid + 自增序号（学习期够用，生产用 uuid）
    static int seq = 0;
    const std::string unique =
        std::to_string(getpid()) + "_" + std::to_string(seq++);
    const std::string src = "/tmp/oj_" + unique +
                            (lang == "c" ? ".c" : ".cpp");
    const std::string bin = "/tmp/oj_" + unique;

    // ② 写源码（谁创建谁清理：src 在本函数内必删）
    FILE* f = fopen(src.c_str(), "wb");
    if (!f) {
        if (err) *err = std::string("写源码失败: ") + std::strerror(errno);
        return false;
    }
    const size_t wr = fwrite(code.data(), 1, code.size(), f);
    fclose(f);
    if (wr != code.size()) {
        if (err) *err = "写源码不完整";
        unlink(src.c_str());
        return false;
    }

    // ③ 编译命令
    std::vector<std::string> argv;
    if (lang == "c") argv = {"gcc", "-O2", "-o", bin, src};
    else             argv = {"g++", "-O2", "-o", bin, src};

    std::string output;
    int status = 0;
    // capture_stderr=true：编译错误（g++ 写 stderr）必须捕获
    const int rc = run_capture(argv, 30000, true, &output, &status, err);

    if (rc == -1) {             // 内部故障：管道/fork 失败
        unlink(src.c_str());
        return false;
    }
    if (rc == 1) {              // 编译超时（编译器被杀 ≠ 用户代码错）
        unlink(src.c_str());
        unlink(bin.c_str());
        outcome->status = JudgeStatus::SystemError;
        outcome->error_msg = "编译超时（>30s）";
        return true;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {  // CE
        if (compiler_stderr) *compiler_stderr = output;
        unlink(src.c_str());
        unlink(bin.c_str());
        outcome->status = JudgeStatus::CompileError;
        return true;
    }

    // ④ 编译成功：src 删掉，bin 保留给运行阶段
    unlink(src.c_str());
    *out_bin = bin;
    return true;
}

}  // namespace

// ================= 主函数：编译 + 运行 + 解析 =================
bool judge_code(const std::string& code, const std::string& language,
                const std::string& judge_main,
                int time_ms, int mem_mb, int output_mb,
                JudgeOutcome* outcome, std::string* err) {
    // ① 编译
    std::string bin, compiler_stderr;
    if (!compile_code(code, language, &bin, &compiler_stderr, outcome, err)) {
        return false;  // 内部故障
    }
    if (outcome->status == JudgeStatus::CompileError) {
        outcome->stderr_text = compiler_stderr;  // CE：把编译器输出带给用户
        return true;   // CE 是正常判题结果
    }
    if (outcome->status == JudgeStatus::SystemError) {
        return true;   // 编译超时
    }

    // ② 运行 judge_main（沙箱内）
    //    judge_main 需要 root：worker 不持 root（权限隔离），非 root 时 sudo -n 前缀
    //    -n = non-interactive，不弹密码（worker 无交互，弹密码会挂死）
    std::vector<std::string> argv;
    if (geteuid() != 0) {
        argv = {"sudo", "-n", judge_main, bin,
                std::to_string(time_ms), std::to_string(mem_mb),
                std::to_string(output_mb)};
    } else {
        argv = {judge_main, bin,
                std::to_string(time_ms), std::to_string(mem_mb),
                std::to_string(output_mb)};
    }

    std::string json;
    int status = 0;
    // 运行超时 = 判题限额 + 5s 余量（进程启动/收尾不计入用户墙钟）
    // capture_stderr=false：JSON 只从 stdout 读，judge_main 自身 stderr 直接上日志
    const int rc = run_capture(argv, time_ms + 5000, false, &json, &status, err);

    unlink(bin.c_str());  // 谁创建谁清理：运行完删 bin

    if (rc == -1) return false;   // fork/pipe 故障（内部错误）
    if (rc == 1) {                // judge_main 自身超时（兜底）
        outcome->status = JudgeStatus::SystemError;
        outcome->error_msg = "judge_main 运行超时";
        return true;
    }

    // ③ 解析 JSON（学习期子串查找；生产级 nlohmann/json）
    if (json.find("\"status\"") == std::string::npos) {
        outcome->status = JudgeStatus::SystemError;
        outcome->error_msg = "judge_main 输出缺少 status: " + json;
        return true;
    }

    outcome->status     = parse_status(json_str(json, "status"));
    outcome->exit_code  = std::atoi(json_num(json, "exit_code").c_str());
    outcome->signal     = std::atoi(json_num(json, "signal").c_str());
    outcome->cpu_us     = std::atoll(json_num(json, "cpu_us").c_str());
    outcome->mem_bytes  = std::atoll(json_num(json, "mem_bytes").c_str());
    outcome->wall_us    = std::atoll(json_num(json, "wall_us").c_str());
    outcome->stdout_text = unescape(json_str(json, "stdout"));
    outcome->stderr_text = unescape(json_str(json, "stderr"));
    outcome->error_msg  = unescape(json_str(json, "error_msg"));

    return true;
}

}  // namespace worker
