// Wave 2 · queue.h —— Redis Stream 队列封装（接口契约）
//
// 面试知识点（为什么 Redis Stream 而不是 List/发布订阅）：
// - Stream + 消费组 = 可重投递的可靠队列：消息被 XREADGROUP 读走但未 XACK 时
//   留在 PEL（Pending Entries List），Worker 崩溃后可被 XAUTOCLAIM 找回
// - List（BRPOP）是"弹走即删"，消费方崩溃消息就丢，无法恢复
// - 发布订阅是"即发即焚"，订阅方不在线消息直接丢
// - 因此"判题任务"这种不能丢的负载必须用 Stream
//
// 台阶 1 只实现：produce / consume / ack 三件套；
// 台阶 2（崩溃恢复）再加：claim_idle（XAUTOCLAIM）+ reset_idle（XCLAIM 心跳）

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace worker {

// 一条从 Stream 读到的判题任务
struct StreamMessage {
    std::string id;        // 消息 ID（"时间戳-序号"），XACK 时原样传回
    int64_t submission_id; // 判题任务载荷：提交 ID（MySQL submission 表主键）
};

// Redis Stream 队列封装
// 所有方法失败时返回 false 并填充 err（面试点：判题任务不能静默丢失，
// 连接失败必须显式暴露，宁可 worker 退出让 supervisor 拉起，也不吞错）
class Queue {
public:
    Queue() = default;
    ~Queue();

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    // 连接 Redis（host:port）。失败返回 false 并填 err。
    bool connect(const std::string& host, int port, std::string* err);

    // 生产：XADD 一条判题任务（台阶 1 由测试/脚本调用，Wave 3 由提交接口调用）
    // 返回消息 ID，失败返回空串并填 err。
    std::string produce(int64_t submission_id, std::string* err);

    // 消费：XREADGROUP 从消费组 workers 读消息（阻塞 BLOCK 毫秒）
    // 返回 0..n 条消息；无消息返回空 vector（不是错误）；失败返回 false 并填 err。
    bool consume(const std::string& consumer_id, int block_ms,
                 std::vector<StreamMessage>* out, std::string* err);

    // 确认：XACK 删除 PEL 里的消息（判题完成/被幂等拦截后必须 ACK）
    // 面试点：不 ACK = 消息永远挂 PEL = 10 分钟后被 XAUTOCLAIM 当死任务重投
    bool ack(const std::string& msg_id, std::string* err);

    // ---- 台阶 2：崩溃恢复（XAUTOCLAIM 判死 + XCLAIM 心跳，必须成对）----

    // 接管死任务：XAUTOCLAIM 把 idle 超过 min_idle_ms 的 pending 消息转移给本 consumer
    // 返回 0..n 条消息；无死任务返回空 vector（不是错误）；失败返回 false 并填 err。
    // 面试点：判死负责"把死任务捞回来"，心跳负责"证明活任务没死"——
    // 只有判死会误杀慢任务，只有心跳死任务永远卡 PEL。
    bool claim_idle(const std::string& consumer_id, int64_t min_idle_ms,
                    std::vector<StreamMessage>* out, std::string* err);

    // 心跳续命：XCLAIM 把指定消息 idle 重置为 0（"刚刚投递"），
    // 防止长任务被 XAUTOCLAIM 误判死。失败返回 false 并填 err。
    bool reset_idle(const std::string& consumer_id, const std::string& msg_id,
                    std::string* err);

private:
    void* ctx_ = nullptr;  // redisContext*（前向声明避免把头文件暴露给 hiredis）
};

}  // namespace worker
