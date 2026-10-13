// Wave 2 · queue.cpp —— Redis Stream 队列封装（台阶 1 实现）
//
// 面试知识点（为什么 Redis Stream 而不是 List/发布订阅）：
// - Stream + 消费组 = 可重投递的可靠队列：消息被 XREADGROUP 读走但未 XACK 时
//   留在 PEL（Pending Entries List），Worker 崩溃后可被 XAUTOCLAIM 找回
// - List（BRPOP）是"弹走即删"，消费方崩溃消息就丢，无法恢复
// - 发布订阅是"即发即焚"，订阅方不在线消息直接丢
// - 因此"判题任务"这种不能丢的负载必须用 Stream
//
// hiredis 三个核心调用：
//   1. redisContext* redisConnect(host, port)
//   2. redisReply* redisCommand(ctx, "XADD ...", ...)
//   3. freeReplyObject(reply)   // ★ 每个 reply 必须 free，否则内存泄漏
//
// 判题任务不能静默丢失（连接失败必须显式暴露，宁可 worker 退出让
// supervisor 拉起，也不吞错）——所有方法失败返回 false 并填 err。

#include "queue.h"

#include <hiredis/hiredis.h>

#include <cstdio>
#include <cstring>

namespace worker {

namespace {

// 流名与消费组名：集中定义，避免三个函数里字符串漂移
constexpr const char* kStream = "judge:stream";
constexpr const char* kGroup = "workers";

// XREADGROUP 返回三层嵌套数组：
//   [0] = 流名
//   [1] = 消息数组
//         [i][0] = 消息 ID
//         [i][1] = 字段/值对（["submission_id", "123"]）
// 逐层剥，每层都先判 type 再取——hiredis 对异常返回 REDIS_REPLY_ERROR/-1，
// 不判类型直接解引用会读到空指针。
void parse_consume_reply(const redisReply* r, std::vector<StreamMessage>* out) {
    if (r->type != REDIS_REPLY_ARRAY || r->elements == 0) {
        return;  // 无消息（空数组 / nil）不是错误
    }
    const redisReply* stream_arr = r->element[0];
    if (stream_arr->type != REDIS_REPLY_ARRAY || stream_arr->elements < 2) {
        return;
    }
    const redisReply* msgs = stream_arr->element[1];
    if (msgs->type != REDIS_REPLY_ARRAY) {
        return;
    }
    for (size_t i = 0; i < msgs->elements; ++i) {
        const redisReply* m = msgs->element[i];
        if (m->type != REDIS_REPLY_ARRAY || m->elements < 2) continue;
        const redisReply* id_r = m->element[0];
        const redisReply* fields = m->element[1];
        if (id_r->type != REDIS_REPLY_STRING) continue;
        if (fields->type != REDIS_REPLY_ARRAY) continue;

        StreamMessage msg;
        msg.id = id_r->str ? id_r->str : "";

        // 字段/值对：[k1, v1, k2, v2, ...]，取 submission_id
        for (size_t j = 0; j + 1 < fields->elements; j += 2) {
            const redisReply* k = fields->element[j];
            const redisReply* v = fields->element[j + 1];
            if (k->type != REDIS_REPLY_STRING || v->type != REDIS_REPLY_STRING) continue;
            if (std::strcmp(k->str, "submission_id") == 0) {
                msg.submission_id = v->str ? std::strtoll(v->str, nullptr, 10) : 0;
                break;
            }
        }
        out->push_back(msg);
    }
}

}  // namespace

Queue::~Queue() {
    if (ctx_) {
        redisFree(static_cast<redisContext*>(ctx_));
        ctx_ = nullptr;
    }
}

bool Queue::connect(const std::string& host, int port, std::string* err) {
    redisContext* c = redisConnect(host.c_str(), port);
    if (c == nullptr || c->err) {
        if (err) *err = c ? c->errstr : "redisConnect 返回 null（内存不足）";
        if (c) redisFree(c);
        return false;
    }
    // 读写超时：阻塞读（XREADGROUP BLOCK）不能永久挂死，
    // 否则 Redis 重启后 worker 卡在 recv 里不退出、不重连。
    //
    // ★ 但 socket 超时必须大于 BLOCK 时间（10s > 5s）：
    //   hiredis 是同步库，BLOCK 由服务端实现——客户端 socket 一直在等，
    //   直到服务端到点返回（或立即有消息）。
    //   若 socket 超时(2s) < BLOCK(5s)，客户端会在服务端返回前 recv 超时，
    //   redisCommand 返回 NULL → consume 每 2 秒报一次 Timeout，
    //   BLOCK 5000 形同虚设（服务端白阻塞，客户端已断开等待）。
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    redisSetTimeout(c, tv);

    // 消费组必须先创建，否则 XREADGROUP 报 NOGROUP（易错点种子）。
    // BUSYGROUP 说明组已存在——这是正常情况，不算错误。
    //
    // ★ 注意建组位置用 `$`（流末尾）的正确性前提：
    //   交接文档第七节第 5 条记录的坑——「先 XADD 再 XGROUP CREATE ... '$'」
    //   会让刚发的消息读不到，XPENDING=0 是**假通过**。
    //   因此：生产环境 worker 启动时流里已有历史消息，用 $ 是正确的
    //   （不重放判题任务）；测试必须**先建组再 XADD** 才能验证消费路径。
    redisReply* r = static_cast<redisReply*>(
        redisCommand(c, "XGROUP CREATE %s %s $ MKSTREAM", kStream, kGroup));
    if (r == nullptr) {
        if (err) *err = std::string("XGROUP CREATE 失败: ") + c->errstr;
        redisFree(c);
        return false;
    }
    if (r->type == REDIS_REPLY_ERROR && std::strstr(r->str, "BUSYGROUP") == nullptr) {
        if (err) *err = std::string("XGROUP CREATE 错误: ") + r->str;
        freeReplyObject(r);
        redisFree(c);
        return false;
    }
    freeReplyObject(r);
    ctx_ = c;
    return true;
}

std::string Queue::produce(int64_t submission_id, std::string* err) {
    if (!ctx_) {
        if (err) *err = "未连接 Redis";
        return "";
    }
    redisContext* c = static_cast<redisContext*>(ctx_);
    redisReply* r = static_cast<redisReply*>(
        redisCommand(c, "XADD %s * submission_id %lld", kStream, (long long)submission_id));
    if (r == nullptr) {
        if (err) *err = std::string("XADD 失败: ") + c->errstr;
        return "";
    }
    if (r->type != REDIS_REPLY_STRING) {
        if (err) *err = std::string("XADD 返回非字符串: ") +
                        (r->str ? r->str : "(无内容)");
        freeReplyObject(r);
        return "";
    }
    std::string id = r->str ? r->str : "";
    freeReplyObject(r);
    return id;
}

bool Queue::consume(const std::string& consumer_id, int block_ms,
                    std::vector<StreamMessage>* out, std::string* err) {
    if (!ctx_) {
        if (err) *err = "未连接 Redis";
        return false;
    }
    out->clear();
    redisContext* c = static_cast<redisContext*>(ctx_);
    // COUNT 10：一次最多取 10 条，避免单次拉太多导致判题串行堆积
    // BLOCK：无消息时阻塞等待而不是空转烧 CPU
    redisReply* r = static_cast<redisReply*>(redisCommand(
        c, "XREADGROUP GROUP %s %s COUNT 10 BLOCK %d STREAMS %s >",
        kGroup, consumer_id.c_str(), block_ms, kStream));
    if (r == nullptr) {
        if (err) *err = std::string("XREADGROUP 失败: ") + c->errstr;
        return false;
    }
    if (r->type == REDIS_REPLY_ERROR) {
        // NOGROUP：消费组被删/流被删——重建组后下轮重试（不退出，不吞错）
        if (std::strstr(r->str, "NOGROUP") != nullptr) {
            if (err) *err = "NOGROUP：消费组不存在，已尝试重建";
            freeReplyObject(r);
            // 重建起点用 `$`（流末尾）而非 `0`（流头）：
            // 若流还在（存着历史判题任务）、组被删，`0` 会从流头重放全部
            // 历史消息 → 重复判题风暴（DB 幂等能拦，但白烧 CPU）。
            // 判题任务不该重放——崩溃恢复的重放通道是 XAUTOCLAIM，
            // 建组不该顺带触发恢复。
            // （流不存在时 MKSTREAM 建空流，$ 与 0 等价，无副作用。）
            redisReply* g = static_cast<redisReply*>(
                redisCommand(c, "XGROUP CREATE %s %s $ MKSTREAM", kStream, kGroup));
            if (g == nullptr) {
                if (err) *err = std::string("NOGROUP 重建失败: ") + c->errstr;
            } else {
                freeReplyObject(g);
            }
            return true;
        }
        if (err) *err = std::string("XREADGROUP 错误: ") + r->str;
        freeReplyObject(r);
        return false;
    }

    parse_consume_reply(r, out);
    freeReplyObject(r);
    return true;
}

bool Queue::ack(const std::string& msg_id, std::string* err) {
    if (!ctx_) {
        if (err) *err = "未连接 Redis";
        return false;
    }
    redisContext* c = static_cast<redisContext*>(ctx_);
    redisReply* r = static_cast<redisReply*>(
        redisCommand(c, "XACK %s %s %s", kStream, kGroup, msg_id.c_str()));
    if (r == nullptr) {
        if (err) *err = std::string("XACK 失败: ") + c->errstr;
        return false;
    }
    if (r->type != REDIS_REPLY_INTEGER) {
        if (err) *err = "XACK 返回非整数";
        freeReplyObject(r);
        return false;
    }
    // 面试点：integer==0 表示这条消息本来就不在 PEL（重复 ACK），
    // 不是错误——幂等要求重复 ACK 必须安全。
    freeReplyObject(r);
    return true;
}

namespace {

// XAUTOCLAIM 返回两层嵌套数组：
//   [0] = 下次扫描游标（字符串，学习期不保存，每轮从 0 重扫）
//   [1] = 被接管消息数组（元素结构与 XREADGROUP 的消息一致）：
//         [i][0] = 消息 ID
//         [i][1] = 字段/值对（["submission_id", "123"]）
// 注意与 parse_consume_reply 的外层结构不同：
//   consume 是 [流名, [消息]]，claim 是 [游标, [消息]]——元素[0] 含义不同，
//   所以不能复用 consume 的解析（它会拿游标当流名去剥层）。
void parse_claim_reply(const redisReply* r, std::vector<StreamMessage>* out) {
    if (r->type != REDIS_REPLY_ARRAY || r->elements < 2) {
        return;  // 格式异常/无消息：不是错误
    }
    const redisReply* msgs = r->element[1];
    if (msgs->type != REDIS_REPLY_ARRAY) {
        return;
    }
    // 以下逐条解析与 consume 完全一致（同一消息结构）
    for (size_t i = 0; i < msgs->elements; ++i) {
        const redisReply* m = msgs->element[i];
        if (m->type != REDIS_REPLY_ARRAY || m->elements < 2) continue;
        const redisReply* id_r = m->element[0];
        const redisReply* fields = m->element[1];
        if (id_r->type != REDIS_REPLY_STRING) continue;
        if (fields->type != REDIS_REPLY_ARRAY) continue;

        StreamMessage msg;
        msg.id = id_r->str ? id_r->str : "";
        for (size_t j = 0; j + 1 < fields->elements; j += 2) {
            const redisReply* k = fields->element[j];
            const redisReply* v = fields->element[j + 1];
            if (k->type != REDIS_REPLY_STRING || v->type != REDIS_REPLY_STRING) continue;
            if (std::strcmp(k->str, "submission_id") == 0) {
                msg.submission_id = v->str ? std::strtoll(v->str, nullptr, 10) : 0;
                break;
            }
        }
        out->push_back(msg);
    }
}

}  // namespace

bool Queue::claim_idle(const std::string& consumer_id, int64_t min_idle_ms,
                       std::vector<StreamMessage>* out, std::string* err) {
    if (!ctx_) {
        if (err) *err = "未连接 Redis";
        return false;
    }
    out->clear();
    redisContext* c = static_cast<redisContext*>(ctx_);
    // XAUTOCLAIM key group consumer min-idle-time start [COUNT count]
    //   - min-idle-time：判死阈值（毫秒）——PEL 中 idle 超过它的消息才被接管
    //   - start=0：从 PEL 头部开始扫。学习期每轮从 0 重扫（被接管的消息要么
    //     ACK 移出 PEL 要么已归本 consumer，重扫代价低）；生产应保存返回的
    //     游标，避免每次都扫已处理区域
    //   - COUNT 10：一次最多接管 10 条，与 consume 批量一致
    redisReply* r = static_cast<redisReply*>(redisCommand(
        c, "XAUTOCLAIM %s %s %s %lld 0 COUNT 10",
        kStream, kGroup, consumer_id.c_str(), (long long)min_idle_ms));
    if (r == nullptr) {
        if (err) *err = std::string("XAUTOCLAIM 失败: ") + c->errstr;
        return false;
    }
    if (r->type == REDIS_REPLY_ERROR) {
        if (err) *err = std::string("XAUTOCLAIM 错误: ") + r->str;
        freeReplyObject(r);
        return false;
    }
    if (r->type != REDIS_REPLY_ARRAY || r->elements < 2) {
        if (err) *err = "XAUTOCLAIM 返回格式异常（非 [游标, 消息]）";
        freeReplyObject(r);
        return false;
    }
    parse_claim_reply(r, out);
    freeReplyObject(r);
    return true;
}

bool Queue::reset_idle(const std::string& consumer_id, const std::string& msg_id,
                       std::string* err) {
    if (!ctx_) {
        if (err) *err = "未连接 Redis";
        return false;
    }
    redisContext* c = static_cast<redisContext*>(ctx_);
    // XCLAIM key group consumer min-idle-time id [IDLE ms]
    //   - min-idle-time=0：不管消息当前 idle 多少都执行接管——
    //     这是给自己的消息续命，不是抢别人的，条件放宽到 0
    //   - IDLE 0：把 idle 重置为 0（"刚刚投递"）。不写 IDLE 参数时
    //     XCLAIM 默认也重置 idle 为 0，这里显式写出意图，读代码的人
    //     一眼知道"这是心跳不是接管"
    //   - 语义：告诉 XAUTOCLAIM "我还在处理这条消息，别判我死"
    redisReply* r = static_cast<redisReply*>(redisCommand(
        c, "XCLAIM %s %s %s 0 %s IDLE 0",
        kStream, kGroup, consumer_id.c_str(), msg_id.c_str()));
    if (r == nullptr) {
        if (err) *err = std::string("XCLAIM 失败: ") + c->errstr;
        return false;
    }
    if (r->type == REDIS_REPLY_ERROR) {
        if (err) *err = std::string("XCLAIM 错误: ") + r->str;
        freeReplyObject(r);
        return false;
    }
    // 返回被接管消息数组。消息不在 PEL（已被 ACK/被他人接管）时返回空数组
    // ——不是错误（幂等要求重复心跳安全）。
    freeReplyObject(r);
    return true;
}

}  // namespace worker
