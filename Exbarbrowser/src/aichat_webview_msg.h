// WebView2 页面 → 宿主的 WebMessage 最小绑定。
//
// 【为什么单独一个头】aichat_webview.cpp 已经超过 700 行；把 COM 事件处理器、
// GUID 与动作白名单放进来会让它更难读。这里只做一件事：把
// window.chrome.webview.postMessage 送来的 JSON 收敛成窄动作名，并校验来源。
//
// 【安全边界】只接受：
//   1) 来源以启动时 URL 的 origin 开头（防止嵌入页导航到外部站点后发指令）；
//   2) type == "window"（窄动作白名单）或 type == "status"（状态白名单）；
//   3) 动作 / 状态字段各自落在白名单里。
// 白名单之外一律丢弃，不进入宿主回调。
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "json.h"

// MinGW 的 windows.h 未必带 EventRegistrationToken；这里与原先 cpp 内定义保持
// 同一个 __int64 形状，所有注册/注销都只在本绑定内部使用。
struct EventRegistrationToken {
    __int64 value;
};

namespace aiui {

// 完成回调与事件回调共用同一个代际门闩。Stop/Retry 后旧回调不能再碰宿主。
struct WebViewCallbackGate {
    std::atomic<uint64_t> generation{0};
};

// 宿主允许页面请求的窗口动作。drag 不在其中：面板是主窗口的子窗口，
// 页面无法安全拖动顶层窗口；前端在 WebView2 模式下也不再发送它。
inline bool IsWebViewWindowAction(const std::wstring& action) {
    return action == L"minimize" || action == L"close" || action == L"maximize"
        || action == L"pin" || action == L"unpin";
}

// 页面上报给状态栏的 AI 状态。它是"通知"不是"命令"：白名单之外的状态、
// 过长的摘要、伪造的撤销域都不会进宿主。
struct AiWebStatus {
    std::wstring state;       // idle / busy / approval / done / error
    std::wstring summary;     // 一行摘要（气泡标题）
    std::wstring detail;      // 补充信息（气泡正文）
    bool canUndo = false;
    std::wstring undoDomain;  // file / bookmark；canUndo 时必须合法
    int turnId = 0;
};

inline bool IsWebViewStatusState(const std::wstring& state) {
    return state == L"idle" || state == L"busy" || state == L"approval"
        || state == L"done" || state == L"error";
}

inline bool IsWebViewUndoDomain(const std::wstring& domain) {
    return domain.empty() || domain == L"file" || domain == L"bookmark";
}

inline std::wstring ClipWebViewStatusText(const std::wstring& text, size_t cap) {
    return text.size() <= cap ? text : text.substr(0, cap) + L"…";
}

// 把 WebMessageAsJson 解析成白名单状态。返回 false = 不是合法状态消息。
inline bool ParseWebViewStatus(const std::wstring& jsonText, AiWebStatus* out,
                               std::wstring* error = nullptr) {
    json::Value v;
    std::wstring err;
    if (!json::Parse(jsonText, v, &err) || !v.IsObject()) {
        if (error) *error = L"消息不是 JSON 对象";
        return false;
    }
    if (v.GetStr(L"type") != L"status") {
        if (error) *error = L"消息类型不是 status";
        return false;
    }
    const std::wstring state = v.GetStr(L"state");
    if (!IsWebViewStatusState(state)) {
        if (error) *error = L"AI 状态不在白名单：" + (state.empty() ? L"(空)" : state);
        return false;
    }
    AiWebStatus parsed;
    parsed.state = state;
    parsed.summary = ClipWebViewStatusText(v.GetStr(L"summary"), 120);
    parsed.detail = ClipWebViewStatusText(v.GetStr(L"detail"), 400);
    parsed.canUndo = v.GetBool(L"canUndo", false);
    parsed.undoDomain = v.GetStr(L"undoDomain");
    parsed.turnId = v.GetInt(L"turnId", 0);
    if (parsed.canUndo && !IsWebViewUndoDomain(parsed.undoDomain)) {
        if (error) *error = L"撤销域不在白名单：" + parsed.undoDomain;
        return false;
    }
    if (!parsed.canUndo) parsed.undoDomain.clear();
    if (out) *out = std::move(parsed);
    return true;
}

// 把 WebMessageAsJson 解析成白名单动作。返回 false = 不是合法窗口动作。
inline bool ParseWebViewWindowAction(const std::wstring& jsonText,
                                     std::wstring* action,
                                     std::wstring* error = nullptr) {
    json::Value v;
    std::wstring err;
    if (!json::Parse(jsonText, v, &err) || !v.IsObject()) {
        if (error) *error = L"消息不是 JSON 对象";
        return false;
    }
    if (v.GetStr(L"type") != L"window") {
        if (error) *error = L"消息类型不是 window";
        return false;
    }
    const std::wstring a = v.GetStr(L"action");
    if (!IsWebViewWindowAction(a)) {
        if (error) *error = L"窗口动作不在白名单：" + (a.empty() ? L"(空)" : a);
        return false;
    }
    if (action) *action = a;
    return true;
}

// 来源校验：期望 origin 后必须紧跟 /、? 或结束，不能把
// http://127.0.0.1:1234.evil.com 误当成 http://127.0.0.1:1234。
inline bool IsWebViewSourceAllowed(const std::wstring& expectedOrigin,
                                   const std::wstring& source) {
    if (expectedOrigin.empty() || source.size() < expectedOrigin.size()) return false;
    if (source.compare(0, expectedOrigin.size(), expectedOrigin) != 0) return false;
    if (source.size() == expectedOrigin.size()) return true;
    const wchar_t next = source[expectedOrigin.size()];
    return next == L'/' || next == L'?';
}

// ICoreWebView2WebMessageReceivedEventHandler 的官方 IID。
const GUID kIidWebMessageHandler = {
    0x57213f19, 0x00e6, 0x49fa, {0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2}};
// ICoreWebView2WebMessageReceivedEventArgs 的官方 IID。
const GUID kIidWebMessageArgs = {
    0x0f99a40c, 0xe962, 0x4207, {0x9e, 0x92, 0xe3, 0xd5, 0x42, 0xef, 0xf8, 0x49}};

struct IWebMessageHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IUnknown* sender, IUnknown* args) = 0;
};

struct IWebMessageArgs : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Source(LPWSTR* source) = 0;       // 3
    virtual HRESULT STDMETHODCALLTYPE get_WebMessageAsJson(LPWSTR* json) = 0; // 4
    virtual HRESULT STDMETHODCALLTYPE TryGetWebMessageAsString(LPWSTR* msg) = 0; // 5
};

template <typename T>
T* QueryIface(IUnknown* p, const GUID& iid) {
    if (!p) return nullptr;
    void* out = nullptr;
    if (FAILED(p->QueryInterface(iid, &out)) || !out) return nullptr;
    return static_cast<T*>(out);
}

// 页面消息事件处理器。只把通过三道校验的 action 交给 sink。
class WebMessageHandler : public IWebMessageHandler {
public:
    using SourceGate = std::function<bool(const std::wstring&)>;
    using ActionSink = std::function<void(const std::wstring&)>;
    using StatusSink = std::function<void(const AiWebStatus&)>;

    WebMessageHandler(std::weak_ptr<WebViewCallbackGate> gate, uint64_t generation,
                      SourceGate allowSource, ActionSink sink,
                      StatusSink statusSink = nullptr)
        : gate_(std::move(gate)), generation_(generation),
          allowSource_(std::move(allowSource)), sink_(std::move(sink)),
          statusSink_(std::move(statusSink)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, kIidWebMessageHandler) || IsEqualGUID(riid, IID_IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IUnknown*, IUnknown* rawArgs) override {
        auto gate = gate_.lock();
        if (!gate || gate->generation.load() != generation_) return S_OK;
        if (!rawArgs || !sink_) return S_OK;

        IWebMessageArgs* args = QueryIface<IWebMessageArgs>(rawArgs, kIidWebMessageArgs);
        if (!args) return S_OK;

        LPWSTR sourceRaw = nullptr;
        bool sourceOk = false;
        if (SUCCEEDED(args->get_Source(&sourceRaw)) && sourceRaw) {
            sourceOk = !allowSource_ || allowSource_(sourceRaw);
            CoTaskMemFree(sourceRaw);
        }
        if (!sourceOk) {
            args->Release();
            return S_OK;
        }

        LPWSTR jsonRaw = nullptr;
        if (SUCCEEDED(args->get_WebMessageAsJson(&jsonRaw)) && jsonRaw) {
            std::wstring action;
            std::wstring error;
            if (ParseWebViewWindowAction(jsonRaw, &action, &error)) {
                sink_(action);
            } else {
                AiWebStatus status;
                if (ParseWebViewStatus(jsonRaw, &status, &error) && statusSink_)
                    statusSink_(status);
            }
            CoTaskMemFree(jsonRaw);
        }
        args->Release();
        return S_OK;
    }

private:
    virtual ~WebMessageHandler() = default;
    std::atomic<ULONG> refs_{1};
    std::weak_ptr<WebViewCallbackGate> gate_;
    uint64_t generation_ = 0;
    SourceGate allowSource_;
    ActionSink sink_;
    StatusSink statusSink_;
};

}  // namespace aiui
