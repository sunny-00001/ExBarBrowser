#include "ai_http.h"

#include <cstddef>

#include "common.h"

#include <winhttp.h>

#include "json.h"
#include "str.h"

namespace ai {
namespace http {

namespace {

// HINTERNET 的 RAII。WinHTTP 的句柄必须逐层释放（request → connect → session），
// 漏一个就是句柄泄漏 —— 长时间反复请求会耗尽。用 RAII 保证任何早退路径都释放。
class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : h_(h) {}
    ~WinHttpHandle() { if (h_) WinHttpCloseHandle(h_); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept {
        if (this != &o) { if (h_) WinHttpCloseHandle(h_); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }

    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_ = nullptr;
};

// WinHTTP 错误码 → 人话。用户看到"连接失败"和"证书错误"要做的事完全不同，
// 所以不能压成一句废话（这是本工程反复强调的一条）。
std::wstring ErrorText(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:              return L"连接超时（网络不通或服务端无响应）";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:    return L"域名解析失败（检查网络或代理）";
        case ERROR_WINHTTP_CANNOT_CONNECT:       return L"连不上服务器（可能被墙或服务端宕机）";
        case ERROR_WINHTTP_SECURE_FAILURE:       return L"TLS 握手失败（证书问题或系统过旧）";
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return L"服务端返回了无法识别的响应";
        case ERROR_WINHTTP_OPERATION_CANCELLED:  return L"请求已被取消";
        case ERROR_WINHTTP_CONNECTION_ERROR:     return L"连接中断";
        default: break;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"WinHTTP 错误 0x%08X", static_cast<unsigned>(code));
    return buf;
}

}  // namespace

bool ParseBaseUrl(const std::wstring& base, Endpoint& out, std::wstring* why) {
    std::wstring s = str::Trim(base);
    if (s.empty()) { if (why) *why = L"地址为空"; return false; }

    // 只接受 https：明文 http 会把密钥暴露在网络上。这是硬规则，不给开关。
    const std::wstring kHttps = L"https://";
    if (s.size() < kHttps.size() ||
        str::Lower(s.substr(0, kHttps.size())) != kHttps) {
        if (why) *why = L"只接受 https:// 开头的地址（明文 http 会泄露密钥）";
        return false;
    }
    s.erase(0, kHttps.size());

    const size_t slash = s.find(L'/');
    if (slash == std::wstring::npos) {
        out.host = s;
        out.path = L"/v1/chat/completions";   // 只给了 host，用默认路径
    } else {
        out.host = s.substr(0, slash);
        out.path = s.substr(slash);
    }

    // 端口（host:8080）
    const size_t colon = out.host.find(L':');
    if (colon != std::wstring::npos) {
        const std::wstring port = out.host.substr(colon + 1);
        out.host.erase(colon);
        const int p = _wtoi(port.c_str());
        if (p <= 0 || p > 65535) { if (why) *why = L"端口不合法"; return false; }
        out.port = p;
    }

    if (out.host.empty()) { if (why) *why = L"主机名为空"; return false; }
    return true;
}

std::string BuildChatBody(const std::wstring& model,
                          const std::wstring& userText,
                          double temperature) {
    // 用工程自带的 json 构造，不手拼字符串 —— 手拼一定会在转义上出错
    // （用户输入里带引号、换行、反斜杠，都是常见输入）。
    json::Value root = json::Value::MakeObject();
    root.Set(L"model", json::Value::MakeStr(model));
    root.Set(L"stream", json::Value::MakeBool(true));

    json::Value msgs = json::Value::MakeArray();
    json::Value m = json::Value::MakeObject();
    m.Set(L"role", json::Value::MakeStr(L"user"));
    m.Set(L"content", json::Value::MakeStr(userText));
    msgs.arr.push_back(std::move(m));
    root.Set(L"messages", std::move(msgs));

    root.Set(L"temperature", json::Value::MakeNum(temperature));

    return str::ToUtf8(json::Stringify(root, 0));
}

StreamResult PostChatStreaming(const Endpoint& ep,
                               const std::string& requestBody,
                               const std::function<void(const sse::StreamEvent&)>& onEvent,
                               const Cancel& cancel) {
    StreamResult result;

    // ---- 1. session ----
    WinHttpHandle session(WinHttpOpen(L"ExBarBrowser/1.0 (AI panel)",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        result.error = ErrorText(GetLastError());
        return result;
    }

    // 接收超时给足：流式生成可能持续很久，默认 30s 会把连接掐断。
    // 这个数字不是"性能调优"，是"功能能不能用"。
    WinHttpSetTimeouts(session.get(), 10000, 15000, 30000, /*receive*/ 180000);

    // 强制 TLS 1.2+：Win10 早期版本的默认协议集可能不含 TLS1.2，
    // 不设会表现为"证书/TLS 失败"。
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS,
                     &protocols, sizeof(protocols));

    // ---- 2. connect ----
    WinHttpHandle connect(WinHttpConnect(session.get(), ep.host.c_str(),
                                         static_cast<INTERNET_PORT>(ep.port), 0));
    if (!connect) {
        result.error = ErrorText(GetLastError());
        return result;
    }

    // ---- 3. request ----
    WinHttpHandle request(WinHttpOpenRequest(connect.get(), L"POST", ep.path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request) {
        result.error = ErrorText(GetLastError());
        return result;
    }

    std::wstring headers = L"Content-Type: application/json\r\n"
                           L"Accept: text/event-stream\r\n";
    if (!ep.apiKey.empty())
        headers += L"Authorization: Bearer " + ep.apiKey + L"\r\n";

    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1L),
                            const_cast<char*>(requestBody.data()),
                            static_cast<DWORD>(requestBody.size()),
                            static_cast<DWORD>(requestBody.size()), 0)) {
        result.error = ErrorText(GetLastError());
        return result;
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        result.error = ErrorText(GetLastError());
        return result;
    }

    // ---- 4. 状态码 ----
    {
        DWORD code = 0, len = sizeof(code);
        if (WinHttpQueryHeaders(request.get(),
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                WINHTTP_NO_HEADER_INDEX)) {
            result.statusCode = static_cast<int>(code);
        }
        if (result.statusCode >= 400) {
            // 4xx/5xx 时服务端通常返回一段 JSON 说明原因，读出来给用户看。
            // 只给"HTTP 401"等于让用户去猜 —— 这里把正文一并带上。
            std::string body;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request.get(), &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(request.get(), chunk.data(), avail, &got)) break;
                chunk.resize(got);
                body += chunk;
                if (body.size() > 4096) break;   // 错误正文不可能是长文，防异常服务端
            }
            result.error = L"HTTP " + std::to_wstring(result.statusCode);
            const std::wstring detail = str::FromUtf8(body);
            if (!detail.empty()) result.error += L"：" + detail;
            return result;
        }
    }

    // ---- 5. 流式读 ----
    sse::SseParser parser;
    for (;;) {
        if (cancel.IsSet()) {
            result.canceled = true;
            result.error = L"已取消";
            for (const auto& ev : parser.FinishIncomplete())
                if (onEvent) onEvent(ev);
            return result;
        }

        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &avail)) {
            // 连接在这一步断开：把已累积的 tool_calls 交付，标注不完整
            for (const auto& ev : parser.FinishIncomplete())
                if (onEvent) onEvent(ev);
            result.error = ErrorText(GetLastError());
            return result;
        }
        if (avail == 0) break;   // 流正常结束（服务端关闭）

        std::string buf(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), buf.data(), avail, &got)) {
            for (const auto& ev : parser.FinishIncomplete())
                if (onEvent) onEvent(ev);
            result.error = ErrorText(GetLastError());
            return result;
        }
        buf.resize(got);

        // **关键**：把原始字节直接喂给解析器，不在这里转码、不在这里按长度切。
        // 跨 chunk 的残行与跨 chunk 的多字节字符都由 SseParser 内部处理。
        parser.Feed(buf);

        for (const auto& ev : parser.TakeEvents()) {
            if (onEvent) onEvent(ev);
        }
        if (parser.done()) break;
    }

    result.ok = true;
    return result;
}

}  // namespace http
}  // namespace ai