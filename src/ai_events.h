// 事件流：把「操作完成 / 状态变了」以外发形式推给订阅者。
//
// 为什么需要它（而不是让客户端反复调 state）：
//   · 轮询只能回答"现在是什么样"，回答不了"刚刚发生了什么"——
//     两次采样之间发生又恢复的变化会被整个漏掉；
//   · 轮询频率是客户端猜的，高了浪费、低了延迟大；
//   · 没有序号就没有"我从哪一条之后没收到"的共同基准，断线重连必丢事件。
//
// 这里的事件是**审计事实的副本**，不是命令执行的一部分：
//   · 事件发布失败不会影响命令结果；
//   · 事件线也是同一个令牌保护的，没令牌不能订阅；
//   · 环形缓冲有上限，历史事件可补但不会无限占内存。
//
// 落盘（B8）与三级轨迹（B5 / P2-9）：
//   · 每条事件同时追加到 <dataDir>/events.jsonl，按大小轮转，进程重启后
//     LoadFromDisk() 能接回序号、补回最近历史 —— 反思/自迭代才有原料；
//   · 事件带 traceId / turn / step：traceId 是 W3C trace context（32 hex），
//     turn 是会话第几轮，step 是本轮第几步。没有它们就无法把"第 3 轮第 7 步
//     调了哪个工具"与一次请求对上，OTel GenAI 约定也无从对齐；
//   · 上游 traceparent 走请求 params._meta（MCP 2026-07-28 的约定），
//     RequestScope 在请求边界解析它，命令执行与事件发布共用同一个 traceId。
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "json.h"

namespace ai {
namespace events {

// W3C Trace Context 的 traceparent 解析结果。
// 格式：00-<32 hex trace-id>-<16 hex span-id>-<2 hex flags>
// 全零 id、ff 版本、长度/字符不对一律判非法（不猜、不补全）。
struct TraceContext {
    std::wstring traceId;       // 32 hex 小写
    std::wstring parentSpanId;  // 16 hex 小写（上游 span）
    unsigned char flags = 0;
    std::wstring clientInfo;    // _meta.clientInfo 的原始 JSON 文本（可空）
};
bool ParseTraceparent(const std::wstring& text, TraceContext* out);

// 从请求对象取 _meta（JSON-RPC 在 params._meta，旧格式在顶层 _meta）。
// 找不到 / 非法都返回 false，out 保持不变。
bool TraceContextFromRequest(const json::Value& request, TraceContext* out);

struct Event {
    uint64_t seq = 0;
    std::wstring type;   // "command" / "state.changed" / ...
    json::Value data = json::Value::MakeObject();
    std::wstring ts;            // ISO 8601 本地时间（与 audit 同格式）
    std::wstring traceId;       // 32 hex，来自 traceparent 或进程内生成
    std::wstring spanId;        // 16 hex，本事件自己的 span
    std::wstring parentSpanId;  // 16 hex，上游 span（无则空）
    int turn = 0;               // 第几轮，0 = 无请求上下文的后台事件
    int step = 0;               // 本轮第几步，从 1 起
};

// 请求边界：turn 递增、step 归零，并把 traceparent 装进线程局部上下文。
// RAII，出作用域自动恢复 —— 嵌套调用不会把外层 trace 留在上下文里。
class RequestScope {
public:
    explicit RequestScope(const json::Value& request);
    ~RequestScope();
    RequestScope(const RequestScope&) = delete;
    RequestScope& operator=(const RequestScope&) = delete;

private:
    bool armed_ = false;
};

// 当前上下文的 traceId；没有请求上下文时原样返回 fallback。
// 审计用它替代"只有请求 id"的旧口径，让事件与 audit.jsonl 对上同一条 trace。
std::wstring CurrentTraceIdOr(const std::wstring& fallback);

// 发布一条事件，返回其序号（从 1 起，单调递增）。线程安全。
// data 会被解析成 JSON 后存一份快照：调用方之后改它不会影响已发布事件。
// 三级轨迹与时间戳由这里统一补，调用点不填（填漏一处就断链）。
uint64_t Publish(const std::wstring& type, const json::Value& data);

// 显式指定 trace 的发布（跨线程作业等没有 RequestScope 的场合）。
uint64_t PublishWith(const std::wstring& type, const json::Value& data,
                     const TraceContext& ctx, int turn);

// 当前最后一条事件的序号（0 = 还没发过）。订阅者用它做"从哪开始"的基准。
uint64_t LastSeq();

// 取 afterSeq 之后的事件（不含 afterSeq）。limit=0 表示用默认上限。
// 返回 JSON 文本：{"events":[{seq,type,ts,traceId,spanId,parentSpanId,turn,step,data},...],"lastSeq":N}
std::wstring Since(uint64_t afterSeq, size_t limit = 0);

// 等待 afterSeq 之后的新事件并序列化成**一行事件消息**：
//   {"event":true,"seq":N,"type":"...","t":...,"...data字段":...}
// 超时（毫秒）返回 false。timeoutMs<0 表示无限等（订阅线程用）。
bool WaitFor(uint64_t afterSeq, std::wstring* line, int timeoutMs);

// 清空**内存**历史（仅测试用）。磁盘文件与序号都不动 —— B8 要的就是跨重启历史。
void Clear();

// 启动时从 events.jsonl（含 .1）恢复最近事件与序号。失败不致命：
// 落盘是观测，不是命令执行的一部分。必须在第一次 Publish 之前调用。
void LoadFromDisk();

// 落盘路径（测试用来核对文件确实写了）。
std::wstring EventsPath();

// 状态变化检测：与上次快照比较，变了就发布一条 state.changed。
// 由 UI 线程的 PumpRequests 驱动 —— 客户端拿到的是推送，不是自己轮询。
void PollStateChanges();

// 单测用：关掉/打开状态轮询，避免测试之间互相污染"上次快照"。
void ResetStateBaseline();

}  // namespace events
}  // namespace ai
