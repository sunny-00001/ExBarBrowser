// 对外通道协议自测（第 7 步判据）。
//
// 核心断言：**MCP 客户端能直接说话**。
// 这意味着必须满足 JSON-RPC 2.0 的形状契约：
//   · 响应必须带 jsonrpc:"2.0" 与相同的 id
//   · 请求本身不合法 -> JSON-RPC error（带 code）
//   · 工具执行失败 -> result 里 isError:true（**不是** JSON-RPC error）
// 第三条最容易做错：混在一起客户端就没法区分"我调错了"和"环境不满足"。
//
// 编译（在 ExBarBrowser/ 下，需要窗口无关的那部分）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/rpc_test.cpp src/ai_server.cpp src/ai_api.cpp src/commands.cpp \
//     src/cmd_naming.cpp src/json.cpp src/text_file.cpp /tmp/config_stub.cpp \
//     -o /tmp/rpc_test.exe -lole32 -luuid -lshell32 -ladvapi32 -luser32
#include "common.h"

#include <cstdio>
#include <string>

#include "ai_server.h"
#include "json.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

json::Value ParseJson(const std::wstring& text, bool* ok = nullptr) {
    json::Value v;
    std::wstring err;
    const bool good = json::Parse(text, v, &err);
    if (ok) *ok = good;
    return v;
}

// 发一个请求（JSON 文本），返回解析后的响应
json::Value Rpc(const std::wstring& reqText) {
    return ParseJson(ai::HandleRequestLine(reqText));
}

}  // namespace

int main() {
    std::printf("JSON-RPC 2.0 基本形状\n");
    {
        // 未启用通道（无令牌）时，任何请求都必须被拒 —— 且拒绝形状要合规
        json::Value r = Rpc(L"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
        Check(r.GetStr(L"jsonrpc") == L"2.0", "响应带 jsonrpc:2.0");
        Check(r.Find(L"id") != nullptr, "响应带 id");
        Check(r.Find(L"error") != nullptr, "无令牌 -> error");
        const json::Value* err = r.Find(L"error");
        Check(err && err->Find(L"code"), "error 带 code（JSON-RPC 规范要求）");
        // 关键：未授权时不泄露工具清单
        Check(!r.Find(L"result"), "error 响应里没有 result（不泄露能力面）");
    }

    std::printf("\nid 原样回传（含数字与字符串）\n");
    {
        json::Value r = Rpc(L"{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/list\"}");
        const json::Value* id = r.Find(L"id");
        Check(id && id->type == json::Value::kNumber && id->num == 42, "数字 id 原样回传");

        r = Rpc(L"{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"tools/list\"}");
        id = r.Find(L"id");
        Check(id && id->IsString() && id->str == L"abc", "字符串 id 原样回传");
    }

    std::printf("\n非法请求\n");
    {
        json::Value r = Rpc(L"not json");
        Check(r.Find(L"error") != nullptr, "非 JSON -> error");
        Check(r.GetBool(L"ok", true) == false, "ok=false");

        r = Rpc(L"{\"id\":1,\"method\":\"tools/list\"}");   // 无 jsonrpc 字段
        // 无 jsonrpc 会被当成旧格式，因无 token 而被拒 —— 但不应崩溃
        Check(r.Find(L"error") != nullptr, "缺 jsonrpc 走旧格式并被拒");

        r = Rpc(L"{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"tools/list\"}");
        Check(r.Find(L"error") != nullptr, "版本不是 2.0 -> error");
    }

    std::printf("\n未授权时统一 bad_token（不区分方法是否存在）\n");
    {
        // 这是有意的安全设计：令牌检查在方法分派**之前**。
        // 否则攻击者可以靠"这个方法是 method-not-found 还是 bad-token"
        // 来枚举有哪些方法 —— 能力面泄露。
        json::Value r1 = Rpc(L"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"no/such\"}");
        json::Value r2 = Rpc(L"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
        const json::Value* e1 = r1.Find(L"error");
        const json::Value* e2 = r2.Find(L"error");
        Check(e1 && e1->GetStr(L"message") == L"bad_token", "未知方法也回 bad_token");
        Check(e2 && e2->GetStr(L"message") == L"bad_token", "已知方法也回 bad_token");
        // 两者完全一样 -> 无法据此区分方法是否存在
        Check(e1 && e2 && e1->GetStr(L"message") == e2->GetStr(L"message"),
              "两种请求的响应无法区分（不泄露方法清单）");
    }

    std::printf("\n旧格式仍然可用（向后兼容）\n");
    {
        json::Value r = Rpc(L"{\"token\":\"x\",\"method\":\"tools/list\"}");
        Check(r.GetBool(L"ok", true) == false, "旧格式无有效令牌被拒");
        Check(r.GetStr(L"error") == L"bad_token", "错误是 bad_token");
        // 旧格式的响应里不该出现 jsonrpc —— 两种协议不混
        Check(r.Find(L"jsonrpc") == nullptr, "旧格式响应不带 jsonrpc");
    }

    std::printf("\n两种协议靠 jsonrpc 字段区分，互不干扰\n");
    {
        json::Value rpc = Rpc(L"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
        json::Value old = Rpc(L"{\"token\":\"x\",\"method\":\"tools/list\"}");
        Check(rpc.Find(L"jsonrpc") != nullptr, "JSON-RPC 请求走新路径");
        Check(old.Find(L"jsonrpc") == nullptr, "旧格式走旧路径");
    }

    std::printf("\ninitialize 握手（MCP 客户端一定会问）\n");
    {
        // 未授权时 initialize 也应被拒（不泄露能力）
        json::Value r = Rpc(L"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
        Check(r.Find(L"error") != nullptr, "无令牌时 initialize 也被拒");
    }

    std::printf("\n响应始终是单行合法 JSON\n");
    {
        const wchar_t* reqs[] = {
            L"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}",
            L"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"no/such\"}",
            L"garbage",
            L"{}",
            L"[]",
        };
        int bad = 0;
        for (const wchar_t* q : reqs) {
            const std::wstring resp = ai::HandleRequestLine(q);
            bool ok = false;
            ParseJson(resp, &ok);
            if (!ok) ++bad;
            if (resp.find(L'\n') != std::wstring::npos) ++bad;   // 必须单行
        }
        Check(bad == 0, "所有响应都是单行合法 JSON");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
