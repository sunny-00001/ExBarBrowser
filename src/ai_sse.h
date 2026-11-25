// SSE（Server-Sent Events）流式解析 + OpenAI chat completions 增量聚合。
//
// 【为什么单独一个文件】这是形态 A（D13）里**最容易写错、又最难在真机上复现**
// 的一块，而它完全是纯函数 —— 不碰 socket、不碰 Win32、不碰 UI，所以能离网单测。
// 把它从 HTTP 读循环里剥出来的理由和 ai_queue 一样：出错的形态不是"功能不对"，
// 是"偶发乱码 / 偶发解析失败"，靠人工点界面几乎测不出来。
//
// 【必须防住的四个坑】（每一个都在真实产品里踩过）
//   1. 一次 WinHttpReadData 不保证返回整行 —— 末尾可能是半行，必须留到下一轮。
//   2. UTF-8 多字节字符会被 chunk 边界切开 —— 所以**只能按 '\n' 切**，绝不能按
//      字节长度切；半行留在 std::string（字节）里不转码，就不会出现 U+FFFD。
//   3. 流末尾的 `data: [DONE]` 不是 JSON —— 直接丢进 json::Parse 会抛错/失败。
//   4. `choices[0].delta.tool_calls[].function.arguments` 是**分片 JSON 字符串**：
//      要按 index 累积所有片段，等 finish_reason == "tool_calls" 再整体解析。
//      中途解析永远失败。
//
// 【与 UI 的关系】本文件只产生"增量事件"，不知道 UI 存在。谁消费、多久消费一次
// （定时器合并重绘）由上层决定 —— 这条边界让"只投增量不投整段"这条铁律
// 有地方落，而不是散在绘制代码里。
#pragma once

#include <string>
#include <vector>

namespace ai {
namespace sse {

// 一条 SSE 事件的载荷（已剥掉 `data:` 前缀与行尾 CR）。
// 空载荷（保活注释、event: 行、纯空行）不会产出 StreamEvent。
struct StreamEvent {
    enum Kind {
        kText,        // delta.content 的一段文本
        kToolCall,    // tool_calls 完整，可以被解析成命令
        kDone,        // 收到 [DONE] 哨兵或 finish_reason，流正常结束
        kReasoning,   // delta.reasoning_content（可选展示，P4-b 先不渲染）
    };
    Kind kind = kText;
    std::wstring text;      // kText / kReasoning 的增量
    // kToolCall 的有效载荷（完整后可解析）
    std::wstring toolName;
    std::wstring toolArgs;  // 已拼完整的 JSON 字符串
    std::wstring toolId;
    std::wstring finishReason;  // "stop" / "tool_calls" / "length"
};

// 增量喂入字节，产出事件。**调用方必须只在"完整行"上切**，
// 所以这里自己做残行缓冲：Feed 接受任意切分的字节，内部累积。
//
// 用法：
//     SseParser p;
//     while (读了一块 buf) {
//         p.Feed(buf);                       // 内部累积残行
//         for (auto& ev : p.TakeEvents()) ...  // 取走本批事件
//     }
class SseParser {
public:
    // 喂入一块原始字节（UTF-8）。可以任意切分，包括切在多字节字符中间。
    void Feed(const char* data, size_t len);
    void Feed(const std::string& s) { Feed(s.data(), s.size()); }

    // 取走自上次调用以来累积的事件。
    std::vector<StreamEvent> TakeEvents();

    // 流被中断（连接断了 / 用户取消）。把残行按"不完整"丢弃，
    // 但已累积的 tool_calls 会以 kToolCall 形式吐出（带 finishReason="incomplete"），
    // 让上层能决定是回滚还是提示 —— 静默丢弃会让"agent 说了一半就没了"无法解释。
    std::vector<StreamEvent> FinishIncomplete();

    bool done() const { return done_; }

private:
    void ConsumeLine(const std::string& line);   // 处理一条完整行（不含换行符）
    void FlushFrame();                           // 空行到达：把累积的 data: 行合成一帧
    void HandlePayload(const std::string& payload);
    void FlushPendingToolCalls(const std::wstring& reason);

    std::string carry_;                  // 跨 chunk 的残行（字节级，不转码）
    std::vector<StreamEvent> events_;
    bool done_ = false;

    // 一帧可以有多条 data: 行（SSE 规范：用 '\n' 连接成一份载荷）。
    // 绝大多数 OpenAI 兼容服务是一帧一行，但网关/代理可能拆行 ——
    // 按规范缓冲到空行再拼，比"每行独立解析"稳。
    std::string eventName_;              // event: 行（Anthropic 用；OpenAI 不用）
    std::vector<std::string> dataBuf_;

    // tool_calls 分片累积：index -> {id, name, args 累积}
    struct PartialCall {
        std::wstring id;
        std::wstring name;
        std::string args;      // 字节级累积，最后统一转宽
        bool gotWholeObject = false;  // 见过"完整 JSON"片 → 后续片按替换处理
    };
    std::vector<PartialCall> calls_;     // 按 index 下标
};

}  // namespace sse
}  // namespace ai