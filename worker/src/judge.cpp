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
#include "internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/resource.h>   // setrlimit/RLIMIT_*（编译子进程资源限制）
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace worker {

namespace {

// 当前墙钟（毫秒）——超时判断用
int64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

// ================= 工具：扁平 JSON 对象解析（学习期简化，但转义正确）=================
// judge_main 输出是我们自己 printf 的扁平 JSON（一层键值对，值只有 string/number）。
// 不引入 nlohmann/json（VM 无三方库），但**必须**把字符串值的转义引号解析对——
// 旧的 find("\"key\"") + find('"') 子串查找会在值内第一个转义引号处截断：
// 用户程序输出 say "hi" → JSON 值为 "say \"hi\"" → 旧解析取到 "say \" →
// 与期望输出比对失败 → 必误判 WA（审查 P1-1，真实事故级缺陷）。单测锁死。

// 前向声明：unescape 定义在本匿名命名空间后面，这里先用（键名还原用）
std::string unescape(const std::string& s);

// 跳过空白（空格/制表/换行/回车）
size_t json_skip_ws(const std::string& s, size_t i) {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
    return i;
}

// 扫描一个带引号的 JSON 字符串（要求 s[i] == '"'）。
// raw 输出原始内容（保留 \" \n 等转义序列，由 unescape 还原）；
// *i 前进到闭合引号之后。转义引号被整体跳过——这是修复截断的关键。
// 格式非法（未闭合引号）返回 false。
bool json_scan_string(const std::string& s, size_t* i, std::string* raw) {
    if (*i >= s.size() || s[*i] != '"') return false;
    ++*i;
    while (*i < s.size()) {
        const char c = s[*i];
        if (c == '\\' && *i + 1 < s.size()) {
            *raw += c;
            *raw += s[*i + 1];
            *i += 2;
            continue;
        }
        if (c == '"') {
            ++*i;
            return true;
        }
        *raw += c;
        ++*i;
    }
    return false;  // 读到结尾都没等到闭合引号
}

// 解析扁平 JSON 对象到 map（key 已 unescape，value 保留原始转义文本）。
// 顺序解析：值内出现 "status": "WA" 这类伪键值对文本也不会被当成键。
bool json_parse_flat(const std::string& json,
                     std::map<std::string, std::string>* out) {
    size_t i = json.find('{');
    if (i == std::string::npos) return false;
    i = json_skip_ws(json, i + 1);
    if (i < json.size() && json[i] == '}') return true;  // "{}" 空对象
    while (i < json.size()) {
        std::string key_raw;
        if (!json_scan_string(json, &i, &key_raw)) return false;
        i = json_skip_ws(json, i);
        if (i >= json.size() || json[i] != ':') return false;
        i = json_skip_ws(json, i + 1);
        std::string val_raw;
        if (i < json.size() && json[i] == '"') {
            if (!json_scan_string(json, &i, &val_raw)) return false;
        } else {
            // 数字/字面量：读到 , 或 } 为止
            const size_t e = json.find_first_of(",}", i);
            if (e == std::string::npos) return false;
            val_raw = json.substr(i, e - i);
            i = e;
        }
        (*out)[unescape(key_raw)] = val_raw;
        i = json_skip_ws(json, i);
        if (i < json.size() && json[i] == ',') {
            i = json_skip_ws(json, i + 1);
            continue;
        }
        return i < json.size() && json[i] == '}';
    }
    return false;
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
// 子进程资源限制（fork 后、exec 前套用；0 = 不限）。编译路径必设：
// 恶意模板递归（template<int N> struct X{X<N-1> x;};）能让 g++ 吃数 GB
// 内存 OOM 整机——判题机不能被一份源码打挂。judge_main 路径不设：
// 用户程序的限制由沙箱自己的 cgroup/RLIMIT 管，套在 judge_main 上会
// 连累沙箱计量（内存限额会被外层先触发）。
struct ChildLimits {
    int64_t address_space_mb = 0;  // RLIMIT_AS：进程虚拟内存上限
    int cpu_sec = 0;               // RLIMIT_CPU：CPU 秒数（超限 SIGKILL）
    int64_t fsize_mb = 0;          // RLIMIT_FSIZE：可写文件大小上限
};

// 把限制套到当前进程（fork 后 exec 前的子进程分支调用）
// 失败即 _exit：套不上限制就不能 exec（宁可判 system_error 也不能裸跑）
void apply_child_limits(const ChildLimits& lim) {
    struct rlimit rl;
    if (lim.address_space_mb > 0) {
        rl = {lim.address_space_mb * 1024 * 1024, lim.address_space_mb * 1024 * 1024};
        if (setrlimit(RLIMIT_AS, &rl) != 0) _exit(126);
    }
    if (lim.cpu_sec > 0) {
        rl = {lim.cpu_sec, lim.cpu_sec};
        if (setrlimit(RLIMIT_CPU, &rl) != 0) _exit(126);
    }
    if (lim.fsize_mb > 0) {
        rl = {lim.fsize_mb * 1024 * 1024, lim.fsize_mb * 1024 * 1024};
        if (setrlimit(RLIMIT_FSIZE, &rl) != 0) _exit(126);
    }
}

// heartbeat（D3）：判题处理期间每 kHeartbeatIntervalMs 调一次的可选回调。
//   为什么需要：run_capture 阻塞时主循环没有机会 reset_idle，长任务消息的
//   idle 会涨过判死阈值被 XAUTOCLAIM 误判死；回调在轮询循环里周期续命。
// child_input（P1-3）：非空时经管道写进子进程 stdin（写完关写端给 EOF）。
//   为什么不用 argv 传用例输入：Linux 单参数上限 128KB（MAX_ARG_STRLEN），
//   大输入用例 execve 直接 E2BIG → judge_main 没跑 → 落 system_error；
//   stdin 没有 128KB 限制，还能原样传递 NUL 等字节（argv 是 C 字符串会截断）。
//   写入带 10s 预算：judge_main 启动即读 stdin，正常毫秒级完成；不读 = 卡死，
//   预算耗尽放弃，由外层 timeout 兜底（与 sandbox 的 stdin 预算同一纪律）。
int run_capture(const std::vector<std::string>& argv, int timeout_ms,
                bool capture_stderr,
                std::string* output, int* status_out, std::string* err,
                const std::function<void()>& heartbeat = {},
                const std::string* child_input = nullptr,
                const ChildLimits& limits = {}) {
    // 忽略 SIGPIPE：child_input 写入时子进程可能已退出（EPIPE），
    // 默认 SIGPIPE 会直接杀死 worker 整个进程——判题机不能被一个死子进程带走
    signal(SIGPIPE, SIG_IGN);

    // ① 管道：子进程 stdout → 父进程（child_input 非空时再加一条 stdin 管道）
    int pipefd[2];
    int in_pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        if (err) *err = std::string("pipe 失败: ") + std::strerror(errno);
        return -1;
    }
    if (child_input && pipe(in_pipefd) != 0) {
        if (err) *err = std::string("stdin pipe 失败: ") + std::strerror(errno);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    // ② fork
    pid_t pid = fork();
    if (pid < 0) {
        if (err) *err = std::string("fork 失败: ") + std::strerror(errno);
        close(pipefd[0]);
        close(pipefd[1]);
        if (in_pipefd[0] >= 0) { close(in_pipefd[0]); close(in_pipefd[1]); }
        return -1;
    }

    if (pid == 0) {
        // 子进程：stdout 重定向到管道写端，然后 exec 替换
        dup2(pipefd[1], STDOUT_FILENO);
        if (capture_stderr) dup2(pipefd[1], STDERR_FILENO);  // 合并 stderr
        if (in_pipefd[0] >= 0) {
            dup2(in_pipefd[0], STDIN_FILENO);   // stdin ← 管道读端
            close(in_pipefd[0]);
            close(in_pipefd[1]);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        // 资源限制必须在 exec 前套：exec 后就是 g++/judge_main 的进程，晚了
        apply_child_limits(limits);
        // vector<string> → char* 数组（execvp 需要，末尾 nullptr）
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        // exec 失败才到这。★ 向 stderr 写 OJ_EXEC_FAIL 标记（两端夹不可打印
        // 前缀 \x01）：父进程据此区分"编译器不存在/无执行权限"（环境故障）与
        // "用户代码 CE"（正常判题结果）。
        // ★ N1：\x01 前缀防用户注入——若裸写 "__EXEC_FAIL__"，用户源码一行
        //   `#error __EXEC_FAIL__` 会让 g++ 把该串原样输出到 stderr → 误判成
        //   SystemError（把编译错误伪装成环境故障）。\x01 用户源码几乎无法输出。
        //   capture_stderr=true 时标记收进 output；false 时打到 worker 日志。
        const std::string emsg =
            std::string("\x01OJ_EXEC_FAIL\x01 ") + std::strerror(errno) + "\n";
        ssize_t ign = write(STDERR_FILENO, emsg.data(), emsg.size());
        (void)ign;
        _exit(127);  // exec 失败的标准码。用 _exit 不走 atexit/析构
                     //   （fork 副本跑析构会 double-free）
    }

    // 父进程：
    // ③ 读端设非阻塞 —— 否则 read 阻塞时超时检查做不了
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    close(pipefd[1]);  // ★ 必须关写端，否则 read 永远等不到 EOF

    // ③' 写子进程 stdin（child_input 非空时）：非阻塞 + 10s 预算，写完关写端
    //    给 EOF——judge_main 读到 EOF 才开始判题。EPIPE（子进程已退出）安全
    //    跳出（SIGPIPE 已在函数入口忽略）。
    if (in_pipefd[0] >= 0) {
        close(in_pipefd[0]);
        const int wflags = fcntl(in_pipefd[1], F_GETFL, 0);
        fcntl(in_pipefd[1], F_SETFL, wflags | O_NONBLOCK);
        const int64_t write_deadline = now_ms() + 10000;
        size_t written = 0;
        while (written < child_input->size()) {
            ssize_t n = write(in_pipefd[1],
                              child_input->data() + written,
                              child_input->size() - written);
            if (n > 0) {
                written += static_cast<size_t>(n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (now_ms() >= write_deadline) break;  // 子进程不读 stdin，放弃
                usleep(1000);
            } else {
                break;  // EPIPE 等：子进程已退出
            }
        }
        close(in_pipefd[1]);  // EOF：judge_main 见到它才开始跑沙箱
    }

    std::string out;
    char buf[4096];
    const int64_t start = now_ms();
    int status = 0;
    bool done = false;
    bool timed_out = false;
    // ★ D3：心跳周期 30s = 判死阈值 10min 的 1/20，容错余量充足；
    //   回调内部是 Redis 往返，30s 一次的频率开销可忽略。
    constexpr int64_t kHeartbeatIntervalMs = 30000;
    int64_t last_heartbeat_ms = start;

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

        // ★ D3：周期心跳（子进程还在跑才需要；done 后无需续命）。
        //   run_capture 阻塞期间只有这里有机会执行回调。
        if (!done && heartbeat && now_ms() - last_heartbeat_ms >= kHeartbeatIntervalMs) {
            last_heartbeat_ms = now_ms();
            heartbeat();
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
    // ★ 编译资源限制（审查 P1-4）：恶意模板递归可让 g++ 吃数 GB 内存 OOM 整机。
    //   RLIMIT_AS 2GB（正常 -O2 编译 <512MB，重模板题也不超 2GB）；
    //   RLIMIT_CPU 20s（墙钟 30s 留余量，CPU 限先杀省资源）；
    //   RLIMIT_FSIZE 256MB（巨型静态数组编出的目标文件/临时文件封顶，
    //   超限 g++ 被 SIGXFSZ 杀 → 落 CE 分支，属用户代码问题）。
    const ChildLimits compile_limits = {/*address_space_mb=*/2048,
                                        /*cpu_sec=*/20,
                                        /*fsize_mb=*/256};
    // capture_stderr=true：编译错误（g++ 写 stderr）必须捕获
    const int rc = run_capture(argv, 30000, true, &output, &status, err,
                               {}, nullptr, compile_limits);

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
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        // ★ 区分"编译器启动失败"（环境故障）与"用户代码 CE"：
        //   execvp 失败的子进程向 stderr 写 \x01OJ_EXEC_FAIL\x01 标记
        //   （capture_stderr 把它收进 output，\x01 前缀防用户源码注入）。
        //   g++/gcc 不存在或无执行权限 → SystemError，而不是误导性的
        //   CompileError（用户看到"编译错误"其实是环境故障）。
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127 &&
            output.find("\x01OJ_EXEC_FAIL\x01") != std::string::npos) {
            unlink(src.c_str());
            unlink(bin.c_str());
            outcome->status = JudgeStatus::SystemError;
            outcome->error_msg = "编译器启动失败（g++/gcc 不存在或无执行权限）";
            return true;
        }
        // 正常 CE：用户代码编译错误（g++ 返回非 0）
        if (compiler_stderr) *compiler_stderr = output;
        unlink(src.c_str());
        unlink(bin.c_str());
        outcome->status = JudgeStatus::CompileError;
        return true;
    }

    // ④ 编译成功：src 删掉，bin 保留给运行阶段
    unlink(src.c_str());
    // ★ 显式重置 status：JudgeOutcome 默认 SystemError（fail-closed），编译成功
    //   若不重置，judge_code 的 `if (status==SystemError) return true`（编译故障
    //   判断）会把"编译成功"误判成编译超时 → 所有提交一律 system_error。
    //   编译成功 = 尚未判题，取中性值 Pending，后续逐用例循环会覆盖它。
    outcome->status = JudgeStatus::Pending;
    *out_bin = bin;
    return true;
}

}  // namespace

// ---- D4 导出：内部纯函数（声明见 internal.h，供单元测试）----

// 取字符串字段（已 unescape）。字段缺失/JSON 非法 → 空串（fail-closed：
// 调用方 parse_status 对空串落 SystemError，不猜）
std::string json_field_str(const std::string& json, const std::string& key) {
    std::map<std::string, std::string> m;
    if (!json_parse_flat(json, &m)) return "";
    const auto it = m.find(key);
    return it == m.end() ? "" : unescape(it->second);
}

// 取数字字段的原始文本（调用方 atoi/atoll）。缺失/非法 → 空串（atoi("")==0）
std::string json_field_num(const std::string& json, const std::string& key) {
    std::map<std::string, std::string> m;
    if (!json_parse_flat(json, &m)) return "";
    const auto it = m.find(key);
    if (it == m.end()) return "";
    const std::string& v = it->second;   // 尾部可能带空白（", " 分隔），去尾
    const size_t e = v.find_last_not_of(" \t\r\n");
    return e == std::string::npos ? "" : v.substr(0, e + 1);
}

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

bool output_matches(const std::string &expected, const std::string &actual)
{
   auto rtrim = [](std::string s){
        while(!s.empty() && (s.back() == ' '|| s.back() == '\n'|| s.back() == '\r'||s.back() == '\t')) s.pop_back();
        return s;
   };
   return rtrim(expected) == rtrim(actual);
}

// ================= 主函数：编译一次 + 逐用例运行 + 比对 =================
bool judge_code(const std::string& code, const std::string& language,
                const std::string& judge_main,
                int time_ms, int mem_mb, int output_mb,
                const std::vector<Testcase>& testcases,
                JudgeOutcome* outcome, std::string* err,
                const std::function<void()>& heartbeat) {
    outcome->total_cases = static_cast<int>(testcases.size());
    outcome->passed_cases = 0;
    // ① 编译（沙箱外 g++，一次）——产物与用例无关，N 个用例共用同一份 bin
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

    // ② 逐用例运行 judge_main（沙箱内，每个用例独立沙箱 = 独立隔离 + 独立计量）
    //    短路：第一个非 AC 即停（全过才 AC，结果已定后续白跑）；bin 循环结束才删
    for (const auto& tc : testcases) {
        // judge_main 需要 root：worker 不持 root（权限隔离），非 root 时 sudo -n 前缀
        // -n = non-interactive，不弹密码（worker 无交互，弹密码会挂死）
        // ★ 用例输入经 stdin 传（run_capture 的 child_input），不进 argv：
        //   argv 单参数上限 128KB，大输入用例 execve 会 E2BIG（审查 P1-3）
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
        // heartbeat 每个用例都传：循环里多个 run_capture，每个都要续命（D3）
        const int rc = run_capture(argv, time_ms + 5000, false, &json, &status, err,
                                   heartbeat, &tc.input);

        if (rc == -1) {   // fork/pipe 故障（内部错误）
            unlink(bin.c_str());
            return false;
        }
        if (rc == 1) {    // judge_main 自身超时（兜底）
            unlink(bin.c_str());
            outcome->status = JudgeStatus::SystemError;
            outcome->error_msg = "judge_main 运行超时";
            return true;
        }

        // 解析 JSON（学习期子串查找；生产级 nlohmann/json）
        if (json.find("\"status\"") == std::string::npos) {
            unlink(bin.c_str());
            outcome->status = JudgeStatus::SystemError;
            outcome->error_msg = "judge_main 输出缺少 status: " + json;
            return true;
        }

        const JudgeStatus st = parse_status(json_field_str(json, "status"));
        outcome->exit_code  = std::atoi(json_field_num(json, "exit_code").c_str());
        outcome->signal     = std::atoi(json_field_num(json, "signal").c_str());
        outcome->cpu_us     = std::atoll(json_field_num(json, "cpu_us").c_str());
        outcome->mem_bytes  = std::atoll(json_field_num(json, "mem_bytes").c_str());
        outcome->wall_us    = std::atoll(json_field_num(json, "wall_us").c_str());
        outcome->stdout_text = json_field_str(json, "stdout");
        outcome->stderr_text = json_field_str(json, "stderr");
        outcome->error_msg  = json_field_str(json, "error_msg");

        // ★ 判定（台阶 2 核心）：OK 只证明"程序跑完没崩"，对错靠输出比对
        if (st == JudgeStatus::Accepted &&
            output_matches(outcome->stdout_text, tc.expected_output)) {
            outcome->passed_cases++;   // 该用例过，继续下一用例
        } else if (st == JudgeStatus::Accepted) {
            outcome->status = JudgeStatus::WrongAnswer;  // 输出不对 → WA，短路
            break;
        } else {
            outcome->status = st;                        // TLE/MLE/RE → 短路
            break;
        }
    }

    unlink(bin.c_str());  // 谁创建谁清理：所有用例跑完删 bin

    // ③ 全过（循环没 break）→ Accepted
    if (outcome->passed_cases == outcome->total_cases) {
        outcome->status = JudgeStatus::Accepted;
    }
    return true;
}

}  // namespace worker
