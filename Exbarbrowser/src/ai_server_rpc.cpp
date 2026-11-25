// AI 对外通道的协议分派：简化格式 + JSON-RPC 2.0（MCP 兼容）。
// 只解析/构造 JSON，不碰管道、线程、发现文件；命令仍经 CommandBus 在 UI 线程执行。
#include "ai_server.h"

#include "ai_api.h"
#include "ai_events.h"
#include "common.h"
#include "json.h"

namespace ai {

// ---------------------------------------------------------------- 请求处理

namespace {

// 令牌比较只有这一份。g_token 留在传输层（ai_server.cpp），这里经公开只读入口取；
// 两条协议路径（简格式 / JSON-RPC）必须共用同一个定长比较，否则"bad_token"的
// 边界会在其中一条路上漂移（例如空令牌被当成合法）。
bool TokenMatches(const std::wstring& got) {
    const std::wstring expected = CurrentToken();
    if (expected.empty()) return false;
    if (got.size() != expected.size()) return false;
    // 定长逐字符比较，不提前返回（本地时序侧信道意义有限，成本几乎为零）
    unsigned diff = 0;
    for (size_t i = 0; i < got.size(); ++i) diff |= static_cast<unsigned>(got[i] ^ expected[i]);
    return diff == 0;
}

bool CheckToken(const json::Value& req) {
    return TokenMatches(req.GetStr(L"token"));
}

json::Value Err(const wchar_t* msg) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"ok", json::Value::MakeBool(false));
    v.Set(L"error", json::Value::MakeStr(msg));
    return v;
}

// ---------------------------------------------------------------- JSON-RPC 2.0
//
// 为什么加这一层：MCP 的传输层就是 JSON-RPC 2.0。客户端（Claude Desktop、
// Cursor 等）说的是 {"jsonrpc":"2.0","id":1,"method":"tools/list"}，
// 我们原先的 {"token":"..","method":"tools/list"} 它们看不懂 —— 于是每个客户端
// 都得自己写一层转换，那就失掉了"直接可用"的意义。
//
// 保留旧格式的理由：已经写好的本地脚本按旧格式发的；两者靠**有无 jsonrpc 字段**
// 区分，互不干扰。
//
// 令牌怎么带：MCP 没有这个概念，所以约定放在 params 里 __token 字段
// （也接受顶层 token 以兼容旧格式）。不是加密，只是"别的进程不能随手乱调"。

// JSON-RPC 2.0 标准错误码全集。kParseError / kInternalError 目前没有调用点，
// 但**保留**而不是删掉：这张表是对外协议的一部分（客户端可能按 code 分派），
// 缺两个会让下一个人以为"本实现不支持这两个码"而另发明一个。
// 用 [[maybe_unused]] 明确"有意保留"，而不是靠注释躲过警告。
[[maybe_unused]] constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
[[maybe_unused]] constexpr int kInternalError = -32603;

json::Value RpcError(const json::Value& id, int code, const wchar_t* message) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"jsonrpc", json::Value::MakeStr(L"2.0"));
    v.Set(L"id", id);
    json::Value err = json::Value::MakeObject();
    err.Set(L"code", json::Value::MakeInt(code));
    err.Set(L"message", json::Value::MakeStr(message));
    v.Set(L"error", err);
    return v;
}

json::Value RpcResult(const json::Value& id, const json::Value& result) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"jsonrpc", json::Value::MakeStr(L"2.0"));
    v.Set(L"id", id);
    v.Set(L"result", result);
    return v;
}

}  // namespace

std::wstring HandleRequestLine(const std::wstring& line) {
    json::Value req;
    std::wstring perr;
    if (!json::Parse(line, req, &perr) || !req.IsObject())
        return json::Stringify(Err(L"invalid_json"), 0);

    // 请求边界：解析 _meta.traceparent，开一轮 trace。命令、事件、审计
    // 在整条调用链上共用同一个 traceId（B5 / P2-9 / B12）。
    const events::RequestScope traceScope(req);

    // ---- 协议分派：有 jsonrpc 字段走 JSON-RPC 2.0（MCP 兼容），否则走旧格式 ----
    if (req.Find(L"jsonrpc")) return HandleRpcLine(req);

    if (!CheckToken(req)) return json::Stringify(Err(L"bad_token"), 0);

    const std::wstring method = req.GetStr(L"method");

    if (method == L"tools/list") {
        json::Value v = json::Value::MakeObject();
        v.Set(L"ok", json::Value::MakeBool(true));
        json::Value tools;
        if (json::Parse(ToolsToJson(), tools, &perr)) {
            const json::Value* arr = tools.Find(L"tools");
            v.Set(L"tools", arr ? *arr : json::Value::MakeArray());
        } else {
            v.Set(L"tools", json::Value::MakeArray());
        }
        return json::Stringify(v, 0);
    }

    if (method == L"state") {
        json::Value v = json::Value::MakeObject();
        v.Set(L"ok", json::Value::MakeBool(true));
        json::Value st;
        if (json::Parse(StateToJson(GetState()), st, &perr)) {
            for (const auto& kv : st.obj) v.Set(kv.first, kv.second);
        }
        return json::Stringify(v, 0);
    }

    // ---- 能力面：让客户端能区分"没这条命令 / 有但没开放 / 有但没实现" ----
    // 放在 tools/list 旁边而不是塞进 state：它是编译期事实，与运行时状态无关。
    if (method == L"capabilities" || method == L"exbar/capabilities") {
        json::Value out;
        std::wstring jerr;
        if (!json::Parse(CapabilitiesToJson(), out, &jerr)) out = json::Value::MakeObject();
        out.Set(L"ok", json::Value::MakeBool(true));
        return json::Stringify(out, 0);
    }

    // ---- 事件流：补历史 + 订阅。两者都过同一个令牌校验（见上面的 CheckToken）----
    if (method == L"events/since") {
        const uint64_t after = static_cast<uint64_t>(req.GetInt(L"after", 0));
        const size_t limit = static_cast<size_t>(req.GetInt(L"limit", 0));
        json::Value out;
        std::wstring jerr;
        if (!json::Parse(events::Since(after, limit), out, &jerr)) out = json::Value::MakeObject();
        out.Set(L"ok", json::Value::MakeBool(true));
        return json::Stringify(out, 0);
    }

    if (method == L"events/subscribe") {
        // 真正的"保持连接并推送"在 ServeLoop 里做；这里只回一个确认。
        json::Value v = json::Value::MakeObject();
        v.Set(L"ok", json::Value::MakeBool(true));
        v.Set(L"subscribed", json::Value::MakeBool(true));
        v.Set(L"lastSeq", json::Value::MakeNum(static_cast<double>(events::LastSeq())));
        return json::Stringify(v, 0);
    }

    if (method == L"tools/call") {
        const std::wstring name = req.GetStr(L"name");
        if (name.empty()) return json::Stringify(Err(L"missing_name"), 0);
        const json::Value* args = req.Find(L"args");
        const json::Value empty = json::Value::MakeObject();
        const bool approve = req.GetBool(L"approve", false);
        CallResult r = Call(name, args ? *args : empty, approve,
                            events::CurrentTraceIdOr(std::wstring()));
        return ResultToJson(r);
    }

    return json::Stringify(Err(L"unknown_method"), 0);
}

// ---------------------------------------------------------------- JSON-RPC 2.0

namespace {

// 从 JSON-RPC 请求里取令牌：MCP 没有令牌概念，所以约定放 params.__token；
// 也接受顶层 token（有些客户端会把自定义字段放外面）。
std::wstring RpcToken(const json::Value& req) {
    if (const json::Value* p = req.Find(L"params")) {
        if (p->IsObject() && !p->GetStr(L"__token").empty()) return p->GetStr(L"__token");
    }
    return req.GetStr(L"token");
}

bool RpcHasToken(const json::Value& req) {
    return TokenMatches(RpcToken(req));
}

}  // namespace

std::wstring HandleRpcLine(const json::Value& req) {
    // id 可以是数字或字符串；缺省视为通知（notification），但仍要回一个
    // 带 null id 的响应 —— 我们不走长连接的状态机，一问一答更好调试。
    json::Value id;   // 默认 kNull
    if (const json::Value* v = req.Find(L"id")) id = *v;

    // 1. 版本
    if (req.GetStr(L"jsonrpc") != L"2.0")
        return json::Stringify(RpcError(id, kInvalidRequest, L"需要 jsonrpc: \"2.0\""), 0);

    // 2. 令牌。放在方法分派之前 —— 未授权时不泄露"有哪些方法/工具"。
    if (!RpcHasToken(req)) {
        json::Value e = RpcError(id, kInvalidRequest, L"bad_token");
        return json::Stringify(e, 0);
    }

    const std::wstring method = req.GetStr(L"method");
    if (method.empty()) return json::Stringify(RpcError(id, kInvalidRequest, L"缺少 method"), 0);

    const json::Value* params = req.Find(L"params");
    const json::Value emptyParams = json::Value::MakeObject();
    const json::Value& p = (params && params->IsObject()) ? *params : emptyParams;

    // ---- MCP 的 initialize：客户端握手时一定会问，不实现会被判定为不兼容 ----
    if (method == L"initialize") {
        json::Value result = json::Value::MakeObject();
        // 协议版本回一个我们支持的（MCP 用日期命名）。客户端会据此决定降级行为。
        result.Set(L"protocolVersion", json::Value::MakeStr(L"2024-11-05"));
        json::Value info = json::Value::MakeObject();
        info.Set(L"name", json::Value::MakeStr(id::kAiServiceName));
        info.Set(L"version", json::Value::MakeStr(L"1.0"));
        result.Set(L"serverInfo", info);
        // 只声明 tools 能力。声明了却做不到比不声明更糟。
        json::Value caps = json::Value::MakeObject();
        json::Value toolsCap = json::Value::MakeObject();
        // listChanged=false：命令表是编译期固定的，不会在运行时增删。
        toolsCap.Set(L"listChanged", json::Value::MakeBool(false));
        caps.Set(L"tools", toolsCap);
        result.Set(L"capabilities", caps);
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- 事件流（JSON-RPC 版）：与本地简格式共用同一份事件缓冲 ----
    if (method == L"events/since") {
        json::Value out;
        std::wstring jerr;
        const uint64_t after = static_cast<uint64_t>(p.GetInt(L"after", 0));
        const size_t limit = static_cast<size_t>(p.GetInt(L"limit", 0));
        if (!json::Parse(events::Since(after, limit), out, &jerr)) out = json::Value::MakeObject();
        return json::Stringify(RpcResult(id, out), 0);
    }
    if (method == L"events/subscribe") {
        json::Value result = json::Value::MakeObject();
        result.Set(L"subscribed", json::Value::MakeBool(true));
        result.Set(L"lastSeq", json::Value::MakeNum(static_cast<double>(events::LastSeq())));
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- tools/list ----
    if (method == L"tools/list") {
        json::Value tools;
        std::wstring perr;
        json::Value result = json::Value::MakeObject();
        if (json::Parse(ToolsToJson(), tools, &perr)) {
            const json::Value* arr = tools.Find(L"tools");
            result.Set(L"tools", arr ? *arr : json::Value::MakeArray());
        } else {
            result.Set(L"tools", json::Value::MakeArray());
        }
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- notifications/cancelled ----
    // MCP 客户端取消请求时发的是通知（无 id）。当前 tools/call 仍可能已经在
    // handler 里跑，进程内没有可抢占的 job 句柄，所以这里**不谎称已中断执行**；
    // 它只做两件可靠的事：留审计线索，并让"尚未进入 tools/call"的排队调用
    // 立刻返回取消。真正防悬挂靠客户端本地拒绝 + timeout 标记。
    if (method == L"notifications/cancelled") {
        const std::wstring rid = p.GetStr(L"requestId");
        const std::wstring why = p.GetStr(L"reason");
        LogDbg(L"[ai] 收到取消通知 request=%s reason=%s", rid.c_str(), why.c_str());
        json::Value result = json::Value::MakeObject();
        result.Set(L"cancelled", json::Value::MakeBool(true));
        result.Set(L"requestId", json::Value::MakeStr(rid));
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- tools/call ----
    if (method == L"tools/call") {
        const std::wstring name = p.GetStr(L"name");
        if (name.empty())
            return json::Stringify(RpcError(id, kInvalidParams, L"缺少 params.name"), 0);

        const json::Value* arguments = p.Find(L"arguments");
        const json::Value emptyArgs = json::Value::MakeObject();
        const bool approve = p.GetBool(L"approve", false);

        CallResult r = Call(name, arguments ? *arguments : emptyArgs, approve,
                            events::CurrentTraceIdOr(ai::TraceIdOf(id)));

        // MCP 的 Tool Result 形状：content 数组 + isError。
        // 失败也走 result（不是 JSON-RPC error）—— 那是"工具执行失败"，
        // 与"请求本身不合法"是两回事，混在一起客户端没法区分该重试还是该改请求。
        json::Value result = json::Value::MakeObject();
        json::Value content = json::Value::MakeArray();
        json::Value text = json::Value::MakeObject();
        text.Set(L"type", json::Value::MakeStr(L"text"));

        // 失败优先：以前 hasResult 为真时会把（空的）结果对象当正文输出，
        // 于是调用方只看到 "{}"，真正的原因 r.error（"目标已存在……"）被丢掉 ——
        // 声明了 outputSchema 的命令统统踩这一条。失败时原因必须在文本块里。
        if (!r.ok) {
            text.Set(L"text", json::Value::MakeStr(L"失败：" + r.error));
            if (r.hasResult) result.Set(L"structuredContent", r.result);
        } else if (r.hasResult) {
            // 有结构化结果时，文本块里放序列化 JSON（兼容只认文本的客户端）。
            text.Set(L"text", json::Value::MakeStr(json::Stringify(r.result, 0)));
            result.Set(L"structuredContent", r.result);
        } else {
            text.Set(L"text", json::Value::MakeStr(L"ok: " + r.command));
        }
        content.arr.push_back(text);
        // content 在断言块之后再 Set：json::Value::Set 是 move 语义，
        // 先 Set 会把它搬空，后面 push_back 的失败说明就丢了。

        // ---- 自断言 ----
        //
        // 断言失败时 isError 置 true。这是刻意的：
        //   命令 handler 返回 true 只说明"我这个函数返回了成功"，而断言说的是
        //   "效果对吗"。两者不一致时，从调用方的立场看这次操作**没有达成目的** ——
        //   如果仍报 isError=false，AI 会基于"已成功"继续下一步，
        //   错误被推到很远以后才暴露，而那时现场已经丢了。
        //
        // 同时在 text 里给出可读原因，并在 assertions 字段里给结构化明细：
        // 只说失败没用，调用方需要知道"期望什么、实际什么"才能决策。
        bool assertFailed = false;
        if (r.hasAssertions) {
            result.Set(L"assertions", r.assertions);
            for (const auto& a : r.assertions.arr) {
                const json::Value* okv = a.Find(L"ok");
                if (okv && !okv->b) {
                    assertFailed = true;
                    const std::wstring expr = a.GetStr(L"expr");
                    const std::wstring expect = a.GetStr(L"expect");
                    const std::wstring actual = a.GetStr(L"actual");
                    LogDbg(L"[ai] %s 断言不符：%s 期望 %s 实际 %s", r.command.c_str(),
                           expr.c_str(), expect.c_str(), actual.c_str());
                    // 失败原因写进文本块：只认 content 的客户端也能看到。
                    json::Value warn = json::Value::MakeObject();
                    warn.Set(L"type", json::Value::MakeStr(L"text"));
                    warn.Set(L"text", json::Value::MakeStr(
                        L"断言不符：" + expr + L"（期望 " + expect + L"，实际 " + actual + L"）"));
                    content.arr.push_back(warn);
                }
            }
            if (assertFailed) {
                result.Set(L"isError", json::Value::MakeBool(true));
            }
        }
        result.Set(L"content", content);
        // 机器码与 isError 并存：MCP 客户端能读 isError，但读不出"哪一类失败"。
        // 客户端要决定"改参数 / 补命令 / 要审批"，靠这个字段而不是解析中文。
        if (!r.errorCode.empty()) result.Set(L"errorCode", json::Value::MakeStr(r.errorCode));
        if (!result.Find(L"isError")) {
            result.Set(L"isError", json::Value::MakeBool(!r.ok));
        }
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- exbar/capabilities：命令总数/已实现/已暴露 + 两类缺口清单 ----
    // 与 exbar/state 同一命名约定（厂商前缀，不占 MCP 标准方法名）。
    if (method == L"exbar/capabilities") {
        json::Value result;
        std::wstring perr;
        if (!json::Parse(CapabilitiesToJson(), result, &perr)) result = json::Value::MakeObject();
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- exbar/state：非 MCP 标准方法，用厂商前缀避免与将来标准冲突 ----
    if (method == L"exbar/state") {
        json::Value st;
        std::wstring perr;
        json::Value result = json::Value::MakeObject();
        if (json::Parse(StateToJson(GetState()), st, &perr)) result = st;
        return json::Stringify(RpcResult(id, result), 0);
    }

    return json::Stringify(RpcError(id, kMethodNotFound, L"未知方法"), 0);
}

}  // namespace ai
