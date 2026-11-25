// SSE 解析器自测。全部离线、纯字符串输入 —— 不需要网络、不需要窗口。
//
// 【为什么这些用例是这些】每一条都对应一个"在真机上偶发、靠点界面测不出来"的形态：
//   1. 行被 chunk 边界切开
//   2. UTF-8 多字节字符被切开（最容易写出乱码的一条）
//   3. [DONE] 哨兵不是 JSON
//   4. tool_calls.arguments 分片累积
//   5. 保活注释 / event: 行不能当错误
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -Isrc -static \
//     tests/ai_sse_test.cpp src/ai_sse.cpp src/json.cpp src/str.cpp \
//     -o /tmp/ai_sse_test.exe
#include <cstdio>
#include <string>

#include "ai_sse.h"
#include "json.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 收集一段流里所有 kText 拼接的结果
std::wstring AllText(ai::sse::SseParser& p) {
    std::wstring s;
    for (const auto& ev : p.TakeEvents())
        if (ev.kind == ai::sse::StreamEvent::kText) s += ev.text;
    return s;
}

// 一条标准的 OpenAI chunk
std::string Chunk(const char* content) {
    std::string s = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"";
    s += content;
    s += "\"},\"finish_reason\":null}]}\n\n";
    return s;
}

}  // namespace

int main() {
    // ---- 1) 基本的逐行解析 ----
    {
        std::printf("[1] 基本流\n");
        ai::sse::SseParser p;
        p.Feed(Chunk("Hello"));
        p.Feed(Chunk(" world"));
        p.Feed("data: [DONE]\n\n");
        Check(AllText(p) == L"Hello world", "两段增量按序拼出 Hello world");
        Check(p.done(), "[DONE] 之后 done()==true");
    }

    // ---- 2) 一行被切成两半（最核心的坑）----
    {
        std::printf("[2] 行被 chunk 边界切开\n");
        ai::sse::SseParser p;
        std::string stream = Chunk("abc") + Chunk("def");
        // 按 7 字节粗暴切 —— 一定会切在行中间，甚至切在 JSON 中间
        for (size_t i = 0; i < stream.size(); i += 7)
            p.Feed(stream.substr(i, 7));
        Check(AllText(p) == L"abcdef", "任意切分后仍能还原 abcdef");
    }

    // ---- 3) UTF-8 多字节字符被切开（中文乱码的来源）----
    {
        std::printf("[3] UTF-8 多字节被切开\n");
        ai::sse::SseParser p;
        // "中文" = E4B8AD E69687，共 6 字节；按 1 字节喂，必然切在字符中间
        std::string stream = Chunk("\xE4\xB8\xAD\xE6\x96\x87");
        for (size_t i = 0; i < stream.size(); ++i)
            p.Feed(stream.data() + i, 1);
        Check(AllText(p) == L"中文", "逐字节喂入仍得到完整的中文（无 U+FFFD）");
    }

    // ---- 4) [DONE] 不能当 JSON 解析 ----
    {
        std::printf("[4] [DONE] 哨兵\n");
        ai::sse::SseParser p;
        p.Feed("data: [DONE]\n");
        auto evs = p.TakeEvents();
        bool gotDone = false, gotText = false;
        for (const auto& ev : evs) {
            if (ev.kind == ai::sse::StreamEvent::kDone) gotDone = true;
            if (ev.kind == ai::sse::StreamEvent::kText) gotText = true;
        }
        Check(gotDone, "[DONE] 产出 kDone 事件");
        Check(!gotText, "[DONE] 不产出文本（没有被当 JSON 解析）");
        Check(p.done(), "done()==true");
    }

    // ---- 5) 保活注释与 event: 行 ----
    {
        std::printf("[5] 注释 / event / 空行\n");
        ai::sse::SseParser p;
        p.Feed(": keepalive\n\n");
        p.Feed("event: message\n");
        p.Feed("id: 42\n");
        p.Feed(Chunk("ok"));
        Check(AllText(p) == L"ok", "注释与 event:/id: 被忽略，不产生假事件");
        Check(!p.done(), "保活行不结束流");
    }

    // ---- 6) tool_calls 分片累积 ----
    {
        std::printf("[6] tool_calls 分片\n");
        ai::sse::SseParser p;
        // 第一片：带 id 和 name，arguments 只到 "{"
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"fs.delete\","
               "\"arguments\":\"{\\\"tar\"}}]},\"finish_reason\":null}]}\n\n");
        // 第二片：只有 arguments 的后半
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"function\":{\"arguments\":\"gets\\\":[\\\"a\\\"]}\"}}]},\"finish_reason\":null}]}\n\n");
        // 之前不应该有 kToolCall（参数还没拼完）
        {
            auto early = p.TakeEvents();
            bool anyCall = false;
            for (const auto& ev : early)
                if (ev.kind == ai::sse::StreamEvent::kToolCall) anyCall = true;
            Check(!anyCall, "参数未拼完时不产出 kToolCall");
        }
        // 终结片：finish_reason = tool_calls
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n");
        auto evs = p.TakeEvents();
        bool got = false;
        for (const auto& ev : evs) {
            if (ev.kind != ai::sse::StreamEvent::kToolCall) continue;
            got = true;
            Check(ev.toolName == L"fs.delete", "工具名正确 fs.delete");
            Check(ev.toolId == L"call_1", "工具 id 正确 call_1");
            Check(ev.toolArgs == L"{\"targets\":[\"a\"]}", "两片 arguments 拼成完整 JSON");
        }
        Check(got, "finish_reason==tool_calls 时产出 kToolCall");
    }

    // ---- 7) 多个 tool_call 并行（index 0 和 1）----
    {
        std::printf("[7] 两个 tool_call 并行\n");
        ai::sse::SseParser p;
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
               "{\"index\":0,\"id\":\"c0\",\"function\":{\"name\":\"fs.list\",\"arguments\":\"{}\"}},"
               "{\"index\":1,\"id\":\"c1\",\"function\":{\"name\":\"nav.back\",\"arguments\":\"{}\"}}"
               "]},\"finish_reason\":null}]}\n\n");
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n");
        int n = 0;
        bool sawList = false, sawBack = false;
        for (const auto& ev : p.TakeEvents()) {
            if (ev.kind != ai::sse::StreamEvent::kToolCall) continue;
            ++n;
            if (ev.toolName == L"fs.list") sawList = true;
            if (ev.toolName == L"nav.back") sawBack = true;
        }
        Check(n == 2, "两个 tool_call 都被产出");
        Check(sawList && sawBack, "两个工具名都对");
    }

    // ---- 8) reasoning_content 不能混进正文 ----
    {
        std::printf("[8] reasoning_content 分离\n");
        ai::sse::SseParser p;
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":"
               "{\"reasoning_content\":\"想一下\"},\"finish_reason\":null}]}\n\n");
        p.Feed(Chunk("答案"));
        std::wstring text, reasoning;
        for (const auto& ev : p.TakeEvents()) {
            if (ev.kind == ai::sse::StreamEvent::kText) text += ev.text;
            if (ev.kind == ai::sse::StreamEvent::kReasoning) reasoning += ev.text;
        }
        Check(text == L"答案", "正文只含正式回答（不含思考）");
        Check(reasoning == L"想一下", "思考内容单独成 kReasoning");
    }

    // ---- 9) 连接中断时，已累积的 tool_calls 要吐出来 ----
    {
        std::printf("[9] 断流时交付不完整 tool_call\n");
        ai::sse::SseParser p;
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"id\":\"c9\",\"function\":{\"name\":\"fs.delete\",\"arguments\":\"{}\"}}]}}]}\n\n");
        p.TakeEvents();  // 清掉
        auto evs = p.FinishIncomplete();
        bool got = false;
        for (const auto& ev : evs) {
            if (ev.kind != ai::sse::StreamEvent::kToolCall) continue;
            got = true;
            Check(ev.finishReason == L"incomplete", "不完整标记为 incomplete");
        }
        Check(got, "断流时已累积的 tool_call 不被静默丢弃");
    }

    // ---- 10) 错误帧要变成可见文本 ----
    {
        std::printf("[10] 错误帧\n");
        ai::sse::SseParser p;
        p.Feed("data: {\"error\":{\"message\":\"invalid api key\"}}\n\n");
        std::wstring text;
        bool gotDone = false;
        for (const auto& ev : p.TakeEvents()) {
            if (ev.kind == ai::sse::StreamEvent::kText) text += ev.text;
            if (ev.kind == ai::sse::StreamEvent::kDone && ev.finishReason == L"error")
                gotDone = true;
        }
        Check(text.find(L"invalid api key") != std::wstring::npos,
              "错误消息变成可见文本（用户能看到原因）");
        Check(gotDone, "错误帧后流结束（不会无限等）");
    }

    // ---- 11) 网关在每个 chunk 重发整段 arguments ----
    //
    // 这不是假想：部分 vLLM / one-api 构建就是这么干的。盲目 += 会拼出
    // {"a":1}{"a":1} 这种废 JSON，模型侧看起来像"工具参数永远解析失败"。
    //
    // 刻意用**不含反斜杠**的载荷：三层转义（C++ 字面量 → 外层 JSON → 内层 JSON）
    // 数错一层就会写出一条"永远失败"的假测试，把真问题掩盖成转义问题。
    // 这里只问行为：重发时该替换还是该追加。
    {
        std::printf("[11] 整段重发（不要重复拼接）\n");
        ai::sse::SseParser p;
        for (int i = 0; i < 3; ++i) {
            p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
                   "\"id\":\"c1\",\"function\":{\"name\":\"fs.list\","
                   "\"arguments\":\"{\\\"pattern\\\":\\\"*.obj\\\"}\"}}]},"
                   "\"finish_reason\":null}]}\n\n");
        }
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n");
        bool sawCall = false;
        for (const auto& ev : p.TakeEvents()) {
            if (ev.kind != ai::sse::StreamEvent::kToolCall) continue;
            sawCall = true;
            json::Value args;
            const bool parsed = json::Parse(ev.toolArgs, args, nullptr);
            Check(parsed, "重发多次后 toolArgs 仍是合法 JSON（没有重复拼接）");
            Check(parsed && args.GetStr(L"pattern") == L"*.obj",
                  "参数值没被拼接破坏（pattern 正确）");
        }
        Check(sawCall, "重发场景下仍产出 kToolCall");
    }

    // ---- 12) 一帧多条 data: 行（SSE 规范允许，网关会拆行）----
    {
        std::printf("[12] 多 data 行合成一帧\n");
        ai::sse::SseParser p;
        // 把一个 JSON 拆成两条 data: 行，中间没有空行
        p.Feed("data: {\"choices\":[{\"index\":0,\n");
        p.Feed("data: \"delta\":{\"content\":\"分段\"},\"finish_reason\":null}]}\n\n");
        Check(AllText(p) == L"分段", "拆成两条 data: 行的帧被正确合成");
    }

    // ---- 13) 断流时最后一帧没有空行结尾 ----
    {
        std::printf("[13] 末帧无空行\n");
        ai::sse::SseParser p;
        // 完整 JSON，但**没有**结尾的空行（连接恰好在这之后断）
        p.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"尾巴\"},"
               "\"finish_reason\":null}]}\n");
        auto evs = p.FinishIncomplete();
        std::wstring text;
        for (const auto& ev : evs)
            if (ev.kind == ai::sse::StreamEvent::kText) text += ev.text;
        Check(text == L"尾巴", "断流时末帧仍被交付（不丢最后一段回复）");
    }

    std::printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail;
}