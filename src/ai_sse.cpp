#include "ai_sse.h"

#include <cstddef>

#include "json.h"
#include "str.h"

namespace ai {
namespace sse {

void SseParser::Feed(const char* data, size_t len) {
    if (!data || len == 0) return;
    if (done_) return;  // 哨兵之后的内容一律忽略（有的服务端会多吐几行空行）

    // 纯字节累积。**不在这里转 wstring** —— 转码必须发生在"完整帧"上，
    // 否则一个被 chunk 边界切开的汉字会被解成两个 U+FFFD。
    carry_.append(data, len);

    // 只按 '\n' 切。SSE 规范允许 \r\n / \n / \r 三种行尾，前两种最常见；
    // 这里按 '\n' 切，再单独吃掉行尾可能残留的 '\r'。
    size_t start = 0;
    for (;;) {
        size_t nl = carry_.find('\n', start);
        if (nl == std::string::npos) break;

        std::string line = carry_.substr(start, nl - start);
        start = nl + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        ConsumeLine(line);
        if (done_) break;  // 收到 [DONE]，后续字节丢弃
    }

    // 保留未成行的残字节，等下一块
    if (start > 0) carry_.erase(0, start);
    if (done_) carry_.clear();
}

void SseParser::ConsumeLine(const std::string& line) {
    // 空行 = 一帧结束。**必须在这里才把 data: 拼起来交给解析** ——
    // SSE 规范允许一帧有多条 data: 行（用 '\n' 连接），而网关/代理
    // 确实会这么做。若每行独立解析，拆行的帧会被丢掉一半。
    if (line.empty()) {
        FlushFrame();
        return;
    }

    // 以 ':' 开头是注释 / 保活（服务端常发 `: keepalive` 防代理掐连接）
    if (line[0] == ':') return;

    if (line.size() >= 6 && line.compare(0, 6, "event:") == 0) {
        // Anthropic 把事件类型放在这里；OpenAI 兼容流不用。
        // 现在只记下来，具体语义交给 HandlePayload（它只看 JSON 形状）。
        eventName_ = line.substr(6);
        if (!eventName_.empty() && eventName_.front() == ' ') eventName_.erase(0, 1);
        return;
    }

    // 只认 `data:`。`id:` / `retry:` 对 LLM 流没有意义，全部忽略 ——
    // 但**不能因此报错**，否则遇到带这些字段的网关就整条流挂掉。
    if (line.size() >= 5 && line.compare(0, 5, "data:") == 0) {
        std::string payload = line.substr(5);
        // 规范允许 `data:` 后有一个可选空格
        if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
        // ★ [DONE] 是无歧义的单行哨兵，**立即处理**，不等空行。
        //   真实服务端常常是 `data: [DONE]\n` 就关流（尾部没有空行），
        //   坚持"必须空行才算一帧"会让它留在缓冲里丢掉 ——
        //   表现为"回复出完了但不结束，UI 一直转圈"。
        if (payload == "[DONE]") {
            dataBuf_.clear();
            HandlePayload(payload);
            return;
        }
        // 其余按规范累积：一帧可以有多条 data: 行，用 '\n' 连接。
        // （OpenAI 兼容流实际总是一帧一行，但 Anthropic 与部分网关不是。）
        dataBuf_.push_back(std::move(payload));
        return;
    }
    // 其他字段（id: / retry: / 厂商私有字段）一律忽略
}

void SseParser::FlushFrame() {
    eventName_.clear();
    if (dataBuf_.empty()) return;

    // 多条 data: 用 '\n' 连接成一份载荷（SSE 规范）
    std::string payload;
    for (size_t i = 0; i < dataBuf_.size(); ++i) {
        if (i) payload += '\n';
        payload += dataBuf_[i];
    }
    dataBuf_.clear();
    if (payload.empty()) return;

    HandlePayload(payload);
}

void SseParser::HandlePayload(const std::string& payload) {
    // 哨兵：不是 JSON。[DONE] 之后流结束，不能再解析。
    if (payload == "[DONE]") {
        FlushPendingToolCalls(L"tool_calls");  // 有的网关不发 finish_reason 直接 [DONE]
        done_ = true;
        StreamEvent ev;
        ev.kind = StreamEvent::kDone;
        events_.push_back(std::move(ev));
        return;
    }

    // 解析 JSON。**解析失败不视为致命** —— 有的网关会插入非标准心跳帧，
    // 一处失败不该让整条流作废。但也不静默：把它当"忽略一帧"处理，
    // 由上层通过"流结束时没有 kDone"来判断异常。
    json::Value v;
    if (!json::Parse(str::FromUtf8(payload), v, nullptr)) return;
    if (!v.IsObject()) return;

    // 错误帧：{"error":{"message":...}} —— 必须让用户看见，否则表现为"AI 不说话"
    if (const json::Value* err = v.Find(L"error")) {
        StreamEvent ev;
        ev.kind = StreamEvent::kText;
        std::wstring msg = err->GetStr(L"message");
        if (msg.empty()) msg = L"请求失败（服务端未给出原因）";
        ev.text = L"\n[" + msg + L"]\n";
        events_.push_back(std::move(ev));
        done_ = true;
        StreamEvent d;
        d.kind = StreamEvent::kDone;
        d.finishReason = L"error";
        events_.push_back(std::move(d));
        return;
    }

    const json::Value* choices = v.Find(L"choices");
    if (!choices || !choices->IsArray() || choices->arr.empty()) return;

    // 只取第 0 个 choice（n>1 的场景我们不用）
    const json::Value& c0 = choices->arr[0];

    const json::Value* delta = c0.Find(L"delta");

    if (delta && delta->IsObject()) {
        // 1) 正文增量
        if (const json::Value* content = delta->Find(L"content")) {
            if (content->IsString() && !content->str.empty()) {
                StreamEvent ev;
                ev.kind = StreamEvent::kText;
                ev.text = content->str;
                events_.push_back(std::move(ev));
            }
        }
        // 2) 思考增量（DeepSeek-R1 / GLM 等）。P4-b 不渲染，但**必须取出来**：
        //    混进正文会让"思考过程"看起来像 AI 的正式回答。
        if (const json::Value* rc = delta->Find(L"reasoning_content")) {
            if (rc->IsString() && !rc->str.empty()) {
                StreamEvent ev;
                ev.kind = StreamEvent::kReasoning;
                ev.text = rc->str;
                events_.push_back(std::move(ev));
            }
        }
        // 3) tool_calls 分片累积
        if (const json::Value* tcs = delta->Find(L"tool_calls")) {
            if (tcs->IsArray()) {
                for (const auto& tc : tcs->arr) {
                    if (!tc.IsObject()) continue;
                    int idx = tc.GetInt(L"index", 0);
                    if (idx < 0) idx = 0;
                    if (static_cast<size_t>(idx) >= calls_.size())
                        calls_.resize(static_cast<size_t>(idx) + 1);
                    PartialCall& pc = calls_[static_cast<size_t>(idx)];
                    // id / name 通常只在第一个片里出现，后续片为空
                    if (const json::Value* id = tc.Find(L"id"))
                        if (id->IsString() && !id->str.empty()) pc.id = id->str;
                    if (const json::Value* fn = tc.Find(L"function")) {
                        if (const json::Value* nm = fn->Find(L"name"))
                            if (nm->IsString() && !nm->str.empty()) pc.name = nm->str;
                        if (const json::Value* ar = fn->Find(L"arguments")) {
                            if (ar->IsString()) {
                                const std::string chunk = str::ToUtf8(ar->str);
                                // ★ 不是所有 relay 都"分片追加"。有些 OpenAI 兼容网关
                                //   （部分 vLLM / one-api 构建）在**每个 chunk 里重发整段**
                                //   arguments。盲目 += 会拼出 {"a":1}{"a":1} 这种废 JSON。
                                //
                                //   判定"是否整段重发"不能靠"累积后能否解析" —— 半截的
                                //   合法分片本来也解析不出来，那样判会把正常分片判错。
                                //   正确判据：**这一片自己能否解析成一个 JSON 对象**。
                                //   能 → 它是整段（替换）；不能 → 是真分片（追加）。
                                bool whole = false;
                                {
                                    json::Value probe;
                                    if (json::Parse(str::FromUtf8(chunk), probe, nullptr) &&
                                        probe.IsObject()) {
                                        whole = true;
                                    }
                                }
                                if (whole) {
                                    pc.args = chunk;
                                    pc.gotWholeObject = true;
                                } else {
                                    // 已经见过整段之后又来分片：视作补充（少见，但
                                    // 不能因此丢掉后续内容）
                                    pc.args += chunk;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // finish_reason 到了 = 这一轮生成结束。此时才能把累积的 tool_calls 整体交付。
    const json::Value* fr = c0.Find(L"finish_reason");
    if (fr && fr->IsString() && !fr->str.empty()) {
        const std::wstring reason = fr->str;
        if (!calls_.empty() && (reason == L"tool_calls" || reason == L"function_call")) {
            FlushPendingToolCalls(reason);
        }
        StreamEvent ev;
        ev.kind = StreamEvent::kDone;
        ev.finishReason = reason;
        events_.push_back(std::move(ev));
        // 注意：**不在这里 done_=true**。finish_reason 之后通常还有 [DONE]；
        // 提前关掉会吞掉那种"只有 finish_reason 没有 [DONE]"的流。
    }
}

void SseParser::FlushPendingToolCalls(const std::wstring& reason) {
    for (auto& pc : calls_) {
        if (pc.name.empty() && pc.args.empty()) continue;  // 空洞（index 跳号）
        StreamEvent ev;
        ev.kind = StreamEvent::kToolCall;
        ev.toolName = pc.name;
        ev.toolId = pc.id;
        ev.toolArgs = str::FromUtf8(pc.args);
        ev.finishReason = reason;
        events_.push_back(std::move(ev));
    }
    calls_.clear();
}

std::vector<StreamEvent> SseParser::TakeEvents() {
    std::vector<StreamEvent> out;
    out.swap(events_);
    return out;
}

std::vector<StreamEvent> SseParser::FinishIncomplete() {
    // 连接断了：先把最后一帧（可能没有空行结尾）处理掉 —— 丢掉它会表现为
    // "最后一段回复没了"，而那正是用户最想看到的一段。
    FlushFrame();
    // 已累积的 tool_calls 吐出来，并标注不完整。
    // 静默丢弃会让"AI 说了一半就没了"无法解释 —— 上层要能提示用户。
    FlushPendingToolCalls(L"incomplete");
    // 更早的残行（既无换行也非完整帧）在这里丢弃：不完整的 JSON 解析出来也是错的。
    carry_.clear();
    return TakeEvents();
}

}  // namespace sse
}  // namespace ai