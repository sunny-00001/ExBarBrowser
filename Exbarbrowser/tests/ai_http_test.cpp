// AI HTTP 层的纯函数自测：URL 拆分 + 请求体拼装。
//
// 【为什么不测网络】发真实请求要 API key、要网络、要计费 —— 不适合放进单测。
// 但"URL 怎么拆"和"请求体怎么拼"是纯逻辑，且**错了两者都会表现为
// 'AI 不说话'**（一个连不上，一个服务端不认），是最该被守住的两处。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -Isrc -static \
//     tests/ai_http_test.cpp src/ai_http.cpp src/ai_sse.cpp src/json.cpp src/str.cpp \
//     -o /tmp/ai_http_test.exe -lwinhttp -lcrypt32 -lole32 -luuid
#include <cstdio>
#include <string>

#include "ai_http.h"
#include "json.h"
#include "str.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    // ---- 1) 只给 host（最常见的配置写法）----
    {
        std::printf("[1] 只给 host\n");
        ai::http::Endpoint ep;
        std::wstring why;
        const bool ok = ai::http::ParseBaseUrl(L"https://api.deepseek.com", ep, &why);
        Check(ok, "https://api.deepseek.com 被接受");
        Check(ep.host == L"api.deepseek.com", "host 正确");
        Check(ep.path == L"/v1/chat/completions", "path 补成默认 chat completions");
        Check(ep.port == 443, "默认端口 443");
    }

    // ---- 2) 带 base path（DashScope 那种）----
    {
        std::printf("[2] 带 base path\n");
        ai::http::Endpoint ep;
        const bool ok = ai::http::ParseBaseUrl(
            L"https://dashscope.aliyuncs.com/compatible-mode/v1", ep);
        Check(ok, "带路径的地址被接受");
        Check(ep.host == L"dashscope.aliyuncs.com", "host 正确");
        Check(ep.path == L"/compatible-mode/v1", "path 保留原样（由调用方补 /chat/completions）");
    }

    // ---- 3) 完整路径（用户直接贴完整 URL）----
    {
        std::printf("[3] 完整 URL\n");
        ai::http::Endpoint ep;
        const bool ok = ai::http::ParseBaseUrl(
            L"https://open.bigmodel.cn/api/paas/v4/chat/completions", ep);
        Check(ok, "完整 URL 被接受");
        Check(ep.host == L"open.bigmodel.cn", "host 正确");
        Check(ep.path == L"/api/paas/v4/chat/completions", "完整路径原样保留");
    }

    // ---- 4) 拒绝明文 http（会泄露密钥）----
    {
        std::printf("[4] 拒绝 http://\n");
        ai::http::Endpoint ep;
        std::wstring why;
        const bool ok = ai::http::ParseBaseUrl(L"http://api.example.com", ep, &why);
        Check(!ok, "明文 http 被拒绝");
        Check(!why.empty(), "给出了原因（不是静默失败）");
    }

    // ---- 5) 端口 ----
    {
        std::printf("[5] 自定义端口\n");
        ai::http::Endpoint ep;
        const bool ok = ai::http::ParseBaseUrl(L"https://localhost:8443/v1", ep);
        Check(ok, "带端口的地址被接受");
        Check(ep.host == L"localhost", "host 不含端口");
        Check(ep.port == 8443, "端口被解析");
    }

    // ---- 6) 空串 / 非法 ----
    {
        std::printf("[6] 非法输入\n");
        ai::http::Endpoint ep;
        std::wstring why;
        Check(!ai::http::ParseBaseUrl(L"", ep, &why), "空串被拒绝");
        Check(!ai::http::ParseBaseUrl(L"api.deepseek.com", ep, &why), "缺 scheme 被拒绝");
    }

    // ---- 7) 请求体：必须能被 JSON 解析回来 ----
    {
        std::printf("[7] 请求体\n");
        const std::string body = ai::http::BuildChatBody(L"deepseek-chat", L"你好", 0.7);
        const std::wstring w = str::FromUtf8(body);
        Check(w.find(L"\"stream\":true") != std::wstring::npos ||
              w.find(L"\"stream\": true") != std::wstring::npos,
              "body 含 stream:true");
        Check(w.find(L"deepseek-chat") != std::wstring::npos, "body 含 model");
        Check(w.find(L"你好") != std::wstring::npos, "body 含用户输入");
        // 能被自己的 json 解析器读回来 = 语法合法
        json::Value v;
        Check(json::Parse(w, v, nullptr), "body 是合法 JSON");
    }

    // ---- 8) 用户输入含特殊字符时不能破坏 JSON ----
    {
        std::printf("[8] 特殊字符转义\n");
        const std::wstring nasty = L"引号\" 反斜杠\\ 换行\n制表\t";
        const std::string body = ai::http::BuildChatBody(L"m", nasty, 0.0);
        json::Value v;
        const bool parsed = json::Parse(str::FromUtf8(body), v, nullptr);
        Check(parsed, "含引号/反斜杠/换行的输入仍生成合法 JSON");
        if (parsed) {
            const json::Value* msgs = v.Find(L"messages");
            bool same = false;
            if (msgs && msgs->IsArray() && !msgs->arr.empty()) {
                const json::Value* c = msgs->arr[0].Find(L"content");
                same = c && c->IsString() && c->str == nasty;
            }
            Check(same, "取回来的 content 与原文逐字相同（转义无损）");
        }
    }

    std::printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail;
}