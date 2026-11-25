// AI 模型的 HTTP 传输层（形态 A，D13）：WinHTTP + 流式读 → ai::sse 解析。
//
// 【为什么单独一个文件】WinHTTP 的句柄生命周期 + 阻塞读 + 取消，是这一层最容易
// 出错的部分；同时它**不依赖任何 UI**，可以离网单测（URL 拆分、请求体拼装、
// 流式喂入都是纯逻辑）。UI 只看到"一串 StreamEvent"。
//
// 【三条硬约束】（D13）
//   1. **只在专用线程调用 Post*Streaming**。WinHttpReadData 会阻塞到有数据为止；
//      在 UI 线程调 = 界面假死。
//   2. **密钥只在本线程内使用**，不挂到任何 UI 对象上、不进日志。
//   3. **只投增量**：产出事件的 text 是 delta 片段，不是累积全文 ——
//      累积交给上层（定时器合并重绘）。
//
// 【超时】流式请求的接收超时必须设长（默认 30s 会在长时间生成时把连接掐断）。
// 这里给 180s，与业界做法一致。
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "ai_sse.h"

namespace ai {
namespace http {

// 一个 OpenAI 兼容端点的连接参数。
// 国内主流全部兼容 chat completions：DeepSeek / DashScope / Moonshot / 智谱 / MiniMax。
struct Endpoint {
    std::wstring host;      // 不含 scheme 与 path，如 L"api.deepseek.com"
    std::wstring path;      // 如 L"/v1/chat/completions"
    std::wstring apiKey;    // Bearer；只在本线程使用，不外传、不落盘
    int port = 443;         // HTTPS
};

// 把 base URL 拆成 host + path。
// 接受 "https://api.deepseek.com/v1" + "/chat/completions" 两种常见写法，
// 也接受直接给完整 "https://api.deepseek.com/v1/chat/completions"。
// 失败返回 false（缺少 scheme、host 为空）。**纯函数，可单测。**
bool ParseBaseUrl(const std::wstring& base, Endpoint& out, std::wstring* why = nullptr);

// 取消令牌：另一个线程置 true 即可让流式读提前返回。
// 用户点"停止"时用。不用 atomic<bool> 的裸指针，避免生命周期问题。
struct Cancel {
    std::atomic<bool> flag{false};
    void Set() { flag.store(true, std::memory_order_relaxed); }
    bool IsSet() const { return flag.load(std::memory_order_relaxed); }
};

// 流式请求的结果。**不抛异常** —— 网络失败是常态，返回结构化结果。
struct StreamResult {
    bool ok = false;              // 流正常结束（收到 [DONE] 或 finish_reason 后自然关闭）
    bool canceled = false;        // 被 Cancel::Set 中断
    std::wstring error;           // ok==false 时的人话（含 HTTP 状态码 / WinHTTP 错误）
    int statusCode = 0;           // HTTP 状态码；0 = 没拿到响应
};

// 同步阻塞地发一个流式请求，**每读到一段增量就回调 onEvent**（在调用线程上）。
//
// onEvent 在**本线程**被调用，所以它绝对不能在内部碰 UI —— 正确做法是把它
// 转投到 UI 队列（见 ai_queue 的模式），由 UI 定时器消费。
//
// requestBody 是已拼好的 JSON 字节（UTF-8）。本函数不解析它。
StreamResult PostChatStreaming(const Endpoint& ep,
                               const std::string& requestBody,
                               const std::function<void(const sse::StreamEvent&)>& onEvent,
                               const Cancel& cancel);

// 拼一个最小请求体（纯函数，可单测）：一条 user 消息 + stream:true。
// 只做我们真正需要的那几个字段 —— 不碰各家 reasoning/thinking 参数差异（D13 的刻意收窄）。
std::string BuildChatBody(const std::wstring& model,
                          const std::wstring& userText,
                          double temperature);

}  // namespace http
}  // namespace ai