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
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "json.h"

namespace ai {
namespace events {

struct Event {
    uint64_t seq = 0;
    std::wstring type;   // "command" / "state.changed" / ...
    json::Value data = json::Value::MakeObject();
};

// 发布一条事件，返回其序号（从 1 起，单调递增）。线程安全。
// data 会被解析成 JSON 后存一份快照：调用方之后改它不会影响已发布事件。
uint64_t Publish(const std::wstring& type, const json::Value& data);

// 当前最后一条事件的序号（0 = 还没发过）。订阅者用它做"从哪开始"的基准。
uint64_t LastSeq();

// 取 afterSeq 之后的事件（不含 afterSeq）。limit=0 表示用默认上限。
// 返回 JSON 文本：{"events":[{seq,type,t,data},...],"lastSeq":N}
std::wstring Since(uint64_t afterSeq, size_t limit = 0);

// 等待 afterSeq 之后的新事件并序列化成**一行事件消息**：
//   {"event":true,"seq":N,"type":"...","t":...,"...data字段":...}
// 超时（毫秒）返回 false。timeoutMs<0 表示无限等（订阅线程用）。
bool WaitFor(uint64_t afterSeq, std::wstring* line, int timeoutMs);

// 清空历史（仅测试/停服时用）。序号不清零，避免订阅者把旧序号当成新事件。
void Clear();

// 状态变化检测：与上次快照比较，变了就发布一条 state.changed。
// 由 UI 线程的 PumpRequests 驱动 —— 客户端拿到的是推送，不是自己轮询。
void PollStateChanges();

// 单测用：关掉/打开状态轮询，避免测试之间互相污染"上次快照"。
void ResetStateBaseline();

}  // namespace events
}  // namespace ai
