// Wave 2 · queue.cpp —— Redis Stream 队列封装（占位实现，待你填充）
//
// 需要 hiredis 三个核心调用：
//   1. redisContext* redisConnect(host, port)
//   2. redisReply* redisCommand(ctx, "XADD ...", ...)
//   3. freeReplyObject(reply)   // ★ 每个 reply 必须 free，否则内存泄漏
//
// 你要实现的三个函数（台阶 1）：
//   produce: XADD judge:stream * submission_id <id>
//            -> reply->type == REDIS_REPLY_STRING，reply->str 即消息 ID
//   consume: XREADGROUP GROUP workers <consumer> COUNT 10 BLOCK <ms> STREAMS judge:stream >
//            -> reply->type == REDIS_REPLY_ARRAY，三层嵌套，逐层剥：
//               [0] 是 stream 名数组；[1] 是消息数组；消息 [i] 内 [0]=ID、[1]=字段值对
//   ack:     XACK judge:stream workers <msg_id>
//            -> reply->type == REDIS_REPLY_INTEGER（1 = 成功确认）

#include "queue.h"

namespace worker {

Queue::~Queue() {
    // TODO: redisFree(ctx_)
}

bool Queue::connect(const std::string& host, int port, std::string* err) {
    // TODO: redisConnect(host.c_str(), port)；失败时 redisContext->errstr
    (void)host; (void)port; (void)err;
    return false;  // 占位：实现后删除
}

std::string Queue::produce(int64_t submission_id, std::string* err) {
    // TODO: XADD judge:stream * submission_id %lld
    (void)submission_id; (void)err;
    return "";  // 占位
}

bool Queue::consume(const std::string& consumer_id, int block_ms,
                    std::vector<StreamMessage>* out, std::string* err) {
    // TODO: XREADGROUP GROUP workers <consumer_id> COUNT 10 BLOCK <block_ms> STREAMS judge:stream >
    // 注意：reply 为空数组（无消息）不是错误，返回空 out 且不填 err
    (void)consumer_id; (void)block_ms; (void)out; (void)err;
    return false;  // 占位
}

bool Queue::ack(const std::string& msg_id, std::string* err) {
    // TODO: XACK judge:stream workers <msg_id>
    (void)msg_id; (void)err;
    return false;  // 占位
}

}  // namespace worker
