// WebView2 最小绑定实现。接口的 vtable 顺序与槽位说明在这里集中列出。
//
// 【必须完整列出的槽位】COM 的虚函数按声明顺序寻址。每个接口都从
// IUnknown 的 3 个方法（QueryInterface / AddRef / Release）开始，
// 然后严格按官方 IDL 顺序列出，中间**不能缺** —— 缺一个，后面全部错位，
// 表现为随机崩溃（调到不相干的函数上）。改动时对照官方文档逐个核槽位序号。
//
// 【为什么用 Pimpl】这些 IEnv / ICtrl / ICore 是**只在本文件使用的私有契约**，
// 放进公共头会把 25 个槽位暴露给所有包含者，还要求它们看到 EventRegistrationToken。
// 公共头只留 WebView 的 5 个方法与一个不透明指针。
//
// 【引用计数纪律】
//   · 完成回调拿到的 result 由 WebView2 持有，Invoke 返回后即失效。
//     要留住它必须自己 QueryInterface 一次（等于 AddRef）。
//   · 我们 new 出来的 handler 交给 WebView2，它负责 Release 一次；
//     若调用本身失败（没交出去），由我们自己 Release。
#include "aichat_webview.h"

#include "aichat_webview_msg.h"
#include "log.h"
#include "text_file.h"   // tf::EnsureDir（预建 WebView2 用户数据目录）

#include <cwchar>
#include <memory>

namespace aiui {
namespace {

// HRESULT 转成可读文案：保留 0x%08lX（排查时要精确码），再附上系统描述。
// 【为什么需要】原来三处失败分支只报十进制 hr，例如共享冲突是 -2147024864，
// 用户无从判断；而多实例共用 user-data-dir 时的报错和"目录不存在"长得一样。
// FormatMessage 走系统资源，不引第三方库；拿不到描述时退回纯十六进制。
static std::wstring WebViewHrText(HRESULT hr) {
    wchar_t raw[32];
    swprintf(raw, 32, L"0x%08lX", static_cast<unsigned long>(hr));
    std::wstring text = std::wstring(L"hr=") + raw;
    wchar_t* sys = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&sys), 0, nullptr);
    if (n && sys) {
        std::wstring desc(sys, n);
        while (!desc.empty() && (desc.back() == L'\r' || desc.back() == L'\n'))
            desc.pop_back();
        if (!desc.empty()) text += L" " + desc;
        LocalFree(sys);
    }
    return text;
}

// ---- WebView2Loader.dll 的函数指针 ----
// 【签名必须与官方 IDL 逐字一致 —— 少一个/多一个参数都会崩】
//
// 官方定义（webview2.idl，4 个参数）：
//   HRESULT CreateCoreWebView2EnvironmentWithOptions(
//       PCWSTR browserExecutableFolder,
//       PCWSTR userDataFolder,
//       ICoreWebView2EnvironmentOptions* environmentOptions,       // 可为 null
//       ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
//
// 【曾经踩过的坑】以前这里多写了一个 `IUnknown** environment` 出参 ——
// 大概是照着"工厂函数有 out 参数"的惯性写的。结果 loader 把第 5 个实参当垃圾，
// 在调用内部崩溃（0xC0000005），崩溃地址落在**我们自己的 .rdata** 上
// （因为它把 handler 指针当成了输出缓冲往里写）。
// 教训：跨 DLL 的函数指针必须按 IDL 逐字抄，不要凭印象补参数。
using PFN_CreateEnvironmentWithOptions = HRESULT(STDMETHODCALLTYPE*)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    IUnknown* environmentOptions, IUnknown* environmentCompletedHandler);
using PFN_GetVersion = HRESULT(STDMETHODCALLTYPE*)(PCWSTR browserExecutableFolder,
                                                 LPWSTR* versionInfo);

constexpr wchar_t kLoaderDll[] = L"WebView2Loader.dll";

HMODULE g_loader = nullptr;
PFN_CreateEnvironmentWithOptions g_createEnv = nullptr;
PFN_GetVersion g_getVersion = nullptr;
bool g_loaded = false;

// GUID 常量（来自官方 IDL）。
const GUID kIidEnvironment = {
    0xB96D755E, 0x0319, 0x4E92, {0xA2, 0x96, 0x23, 0x43, 0x6F, 0x46, 0xA1, 0xFC}};
const GUID kIidController = {
    0x4D00C0D1, 0x9434, 0x4EB6, {0x80, 0x78, 0x86, 0x97, 0xA5, 0x60, 0x33, 0x4F}};
const GUID kIidCore = {
    0x76ECEACB, 0x0462, 0x4D94, {0xAC, 0x83, 0x42, 0x3A, 0x67, 0x93, 0x77, 0x5E}};
const GUID kIidSettings = {
    0xE562E4F0, 0xD7FA, 0x43AC, {0x8D, 0x71, 0xC0, 0x51, 0x50, 0x49, 0x9F, 0x00}};
// 完成回调接口（环境 / 控制器形状相同：Invoke(HRESULT, IUnknown*)）。
const GUID kIidEnvCompleted = {
    0x4E8A3389, 0xC9D8, 0x4BD2, {0xB6, 0xB5, 0x12, 0x4F, 0xEE, 0x6C, 0xC1, 0x4D}};
const GUID kIidCtrlCompleted = {
    0x6C4819F0, 0x85B7, 0x4B71, {0x8C, 0x49, 0xA4, 0x0D, 0x85, 0x2A, 0xE8, 0xE3}};
// ICoreWebView2NavigationCompletedEventHandler（官方 WebView2.h 的 MIDL_INTERFACE）。
// 用途：页面自 reload / 导航后重发主题 WebMessage。首帧 URL 参数只覆盖第一次加载。
const GUID kIidNavCompleted = {
    0xD33A35BF, 0x1C49, 0x4F98, {0x93, 0xAB, 0x00, 0x6E, 0x05, 0x33, 0xFE, 0x1C}};

// ICoreWebView2Environment
//   3   CreateCoreWebView2Controller(HWND, handler)
//   4   CreateWebResourceResponse(...)
//   5   get_BrowserVersionString(LPWSTR*)
//   6   add_NewBrowserVersionAvailable(...)
//   7   remove_NewBrowserVersionAvailable(...)
struct IEnv : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2Controller(
        HWND parentWindow, IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateWebResourceResponse(
        IUnknown* content, int statusCode, PCWSTR reasonPhrase,
        PCWSTR headers, IUnknown** response) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BrowserVersionString(LPWSTR* versionInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NewBrowserVersionAvailable(
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NewBrowserVersionAvailable(
        EventRegistrationToken token) = 0;
};

// ICoreWebView2Controller：槽位 3..25。**一个都不能省**。
struct ICtrl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_IsVisible(BOOL* isVisible) = 0;              // 3
    virtual HRESULT STDMETHODCALLTYPE put_IsVisible(BOOL isVisible) = 0;               // 4
    virtual HRESULT STDMETHODCALLTYPE get_Bounds(RECT* bounds) = 0;                    // 5
    virtual HRESULT STDMETHODCALLTYPE put_Bounds(RECT bounds) = 0;                     // 6
    virtual HRESULT STDMETHODCALLTYPE get_ZoomFactor(double* zoomFactor) = 0;          // 7
    virtual HRESULT STDMETHODCALLTYPE put_ZoomFactor(double zoomFactor) = 0;           // 8
    virtual HRESULT STDMETHODCALLTYPE add_ZoomFactorChanged(                           // 9
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ZoomFactorChanged(                        // 10
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBoundsAndZoomFactor(                          // 11
        RECT bounds, double zoomFactor) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveFocus(int reason) = 0;                        // 12
    virtual HRESULT STDMETHODCALLTYPE add_MoveFocusRequested(                          // 13
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_MoveFocusRequested(                       // 14
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_GotFocus(                                    // 15
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_GotFocus(EventRegistrationToken token) = 0;  // 16
    virtual HRESULT STDMETHODCALLTYPE add_LostFocus(                                   // 17
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_LostFocus(EventRegistrationToken token) = 0; // 18
    virtual HRESULT STDMETHODCALLTYPE add_AcceleratorKeyPressed(                        // 19
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_AcceleratorKeyPressed(                     // 20
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ParentWindow(HWND* parentWindow) = 0;         // 21
    virtual HRESULT STDMETHODCALLTYPE put_ParentWindow(HWND parentWindow) = 0;          // 22
    virtual HRESULT STDMETHODCALLTYPE NotifyParentWindowPositionChanged() = 0;          // 23
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;                                      // 24
    virtual HRESULT STDMETHODCALLTYPE get_CoreWebView2(IUnknown** coreWebView2) = 0;    // 25
};

// ICoreWebView2Settings：只用到 put_IsWebMessageEnabled（槽位 6）。
struct ISettings : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_IsScriptEnabled(BOOL* enabled) = 0;            // 3
    virtual HRESULT STDMETHODCALLTYPE put_IsScriptEnabled(BOOL enabled) = 0;             // 4
    virtual HRESULT STDMETHODCALLTYPE get_IsWebMessageEnabled(BOOL* enabled) = 0;        // 5
    virtual HRESULT STDMETHODCALLTYPE put_IsWebMessageEnabled(BOOL enabled) = 0;         // 6
};

// ICoreWebView2：列到 PostWebMessageAsJson（槽位 32）。中间槽位只用占位声明
// 保持 vtable 顺序 —— COM 按序号寻址，少一个就会调到错误的方法上。
// 顺序对照官方 IDL / 生成的 Go 绑定（get_Settings=3 起）。
struct ICore : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Settings(IUnknown** settings) = 0;            // 3
    virtual HRESULT STDMETHODCALLTYPE get_Source(LPWSTR* uri) = 0;                      // 4
    virtual HRESULT STDMETHODCALLTYPE Navigate(PCWSTR uri) = 0;                         // 5
    virtual HRESULT STDMETHODCALLTYPE NavigateToString(PCWSTR htmlContent) = 0;         // 6
    virtual HRESULT STDMETHODCALLTYPE add_NavigationStarting(                           // 7
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NavigationStarting(                        // 8
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ContentLoading(                               // 9
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ContentLoading(                            // 10
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_SourceChanged(                                // 11
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_SourceChanged(                             // 12
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_HistoryChanged(                               // 13
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_HistoryChanged(                            // 14
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NavigationCompleted(                          // 15
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NavigationCompleted(                       // 16
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameNavigationStarting(                      // 17
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameNavigationStarting(                   // 18
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameNavigationCompleted(                     // 19
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameNavigationCompleted(                  // 20
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ScriptDialogOpening(                          // 21
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ScriptDialogOpening(                       // 22
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PermissionRequested(                          // 23
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PermissionRequested(                       // 24
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ProcessFailed(                                // 25
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ProcessFailed(                             // 26
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddScriptToExecuteOnDocumentCreated(              // 27
        PCWSTR script, IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveScriptToExecuteOnDocumentCreated(           // 28
        PCWSTR id) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteScript(PCWSTR script, IUnknown* handler) = 0;  // 29
    virtual HRESULT STDMETHODCALLTYPE CapturePreview(                                   // 30
        int imageFormat, IUnknown* stream, IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reload() = 0;                                     // 31
    virtual HRESULT STDMETHODCALLTYPE PostWebMessageAsJson(PCWSTR json) = 0;             // 32
    virtual HRESULT STDMETHODCALLTYPE PostWebMessageAsString(PCWSTR text) = 0;           // 33
    virtual HRESULT STDMETHODCALLTYPE add_WebMessageReceived(                            // 34
        IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_WebMessageReceived(                         // 35
        EventRegistrationToken token) = 0;
};

// 从 IUnknown* 取指定接口（AddRef 一次，调用方负责 Release）。
template <typename T>
T* GetIface(IUnknown* p, const GUID& iid) {
    if (!p) return nullptr;
    void* out = nullptr;
    if (FAILED(p->QueryInterface(iid, &out)) || !out) return nullptr;
    return static_cast<T*>(out);
}

// 从完整 URL 取出 origin（scheme://host:port），不带路径/查询串。
std::wstring UrlOrigin(const std::wstring& url) {
    const size_t scheme = url.find(L"://");
    if (scheme == std::wstring::npos) return L"";
    const size_t host = scheme + 3;
    const size_t slash = url.find(L'/', host);
    return slash == std::wstring::npos ? url : url.substr(0, slash);
}

bool LoadLoader() {
    if (g_loaded) return g_createEnv != nullptr;

    // 先按 exe 同目录的绝对路径加载（随包放了一份），失败再让系统按名字搜。
    // 反过来会让 CWD/PATH 里的同名 DLL 抢先，属于真实 DLL 搜索劫持风险。
    DWORD firstErr = ERROR_FILE_NOT_FOUND;
    wchar_t exeDir[MAX_PATH] = L"";
    if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exeDir, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            const std::wstring full = std::wstring(exeDir) + kLoaderDll;
            SetLastError(0);
            g_loader = LoadLibraryW(full.c_str());
            firstErr = GetLastError();
        }
    }
    if (!g_loader) {
        SetLastError(0);
        g_loader = LoadLibraryW(kLoaderDll);
    }
    if (!g_loader) {
        // 错误码是唯一能区分"文件不在"(2) 与"被策略拦住"(193/577) 的证据 ——
        // 别只记"加载失败"，那两种情况的修法完全不同。
        LogDbg(L"[aiui] LoadLibrary(%s) 失败：exe 目录 err=%lu，按名 err=%lu",
               kLoaderDll, firstErr, GetLastError());
        return false;
    }
    g_loaded = true;

    g_createEnv = reinterpret_cast<PFN_CreateEnvironmentWithOptions>(
        GetProcAddress(g_loader, "CreateCoreWebView2EnvironmentWithOptions"));
    g_getVersion = reinterpret_cast<PFN_GetVersion>(
        GetProcAddress(g_loader, "GetAvailableCoreWebView2BrowserVersionString"));
    g_loaded = g_createEnv != nullptr;
    return g_createEnv != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// 实现体（Pimpl）
// ---------------------------------------------------------------------------
class WebViewImpl {
public:
    ~WebViewImpl() { Stop(); }

    bool Start(HWND parent, const std::wstring& url, const std::wstring& userDataDir);
    void SetBounds(const RECT& rc);
    void SetVisible(bool visible);
    void NotifyParentMoved() {
        if (ctrl_) static_cast<ICtrl*>(ctrl_)->NotifyParentWindowPositionChanged();
    }
    void Stop();

    bool Ready() const { return core_ != nullptr; }
    bool IsCurrent(uint64_t generation) const {
        return gate_ && gate_->generation.load() == generation;
    }
    uint64_t BumpGeneration() { return ++gate_->generation; }
    uint64_t Generation() const { return gate_ ? gate_->generation.load() : 0; }
    const std::wstring& Reason() const { return reason_; }

    void OnEnvironmentReady(IUnknown* env, HRESULT hr);
    void OnControllerReady(IUnknown* ctrl, HRESULT hr);
    void OnNavigationCompleted() { PostTheme(); }   // 页面 reload 后恢复运行时主题

    // 控制器就绪后要主动从父窗口实时客户区校正一次尺寸并强制可见 ——
    // 这是 WebView2 嵌入的头号白屏根因：控件是在面板收起（宿主 0×0）期间
    // 发起创建的，等异步回调回来时面板已展开，若仍用旧的/零 bounds，或没
    // 显式 put_IsVisible(TRUE)，WebView2 不会建立可见合成表面 —— 进程齐全、
    // 导航成功，但画面一片空白（业界多次复现，见 docs/DECISIONS.md D14）。
    void ApplyBounds() {
        if (!ctrl_ || !boundsValid_) return;
        // 0×0 的 bounds 会让 WebView2 不建立合成表面。宁可保留上一次的
        // 有效尺寸，也不要把控件缩成 0（收起面板时宿主会收到空矩形）。
        if (bounds_.right - bounds_.left <= 0 || bounds_.bottom - bounds_.top <= 0) return;
        static_cast<ICtrl*>(ctrl_)->put_Bounds(bounds_);
    }
    // 用父窗口当前客户区刷新 bounds；控制器就绪/面板展开后调用。
    void SyncBoundsFromParent() {
        if (!parent_ || !IsWindow(parent_)) return;
        RECT real{};
        if (GetClientRect(parent_, &real) && real.right > 0 && real.bottom > 0) {
            SetBounds(real);   // 内部会 ApplyBounds
        }
    }

    // 主题通过 WebMessage 下发：不重载页面、不丢聊天上下文。
    // 控制器还没就绪时只记状态；OnControllerReady 里会补发一次。
    void SetTheme(bool dark) {
        themeDark_ = dark;
        PostTheme();
    }
    void PostTheme() {
        if (!core_) return;
        const wchar_t* json = themeDark_
            ? L"{\"type\":\"exbar.theme\",\"theme\":\"dark\"}"
            : L"{\"type\":\"exbar.theme\",\"theme\":\"light\"}";
        const HRESULT hr = static_cast<ICore*>(core_)->PostWebMessageAsJson(json);
        LogDbg(L"[aiui] 下发主题 %s -> hr=0x%08lX", themeDark_ ? L"dark" : L"light",
               static_cast<unsigned long>(hr));
    }

    IEnv* env_ = nullptr;
    ICtrl* ctrl_ = nullptr;
    ICore* core_ = nullptr;

    HWND parent_ = nullptr;
    std::wstring url_;
    // 页面来源白名单：只取启动时 URL 的 scheme://host:port，避免带 token 的查询串。
    std::wstring expectedOrigin_;
    std::wstring userDataDir_;
    std::wstring reason_;

    RECT bounds_{0, 0, 0, 0};
    bool boundsValid_ = false;
    bool visible_ = true;
    bool themeDark_ = true;
    bool startIssued_ = false;
    std::shared_ptr<WebViewCallbackGate> gate_ = std::make_shared<WebViewCallbackGate>();
    EventRegistrationToken navToken_{0};
    bool navHooked_ = false;
    EventRegistrationToken msgToken_{0};
    bool msgHooked_ = false;
    std::function<void()> onReady_;
    std::function<void(const std::wstring&)> onError_;
    std::function<void(const std::wstring&)> onWindowAction_;
    std::function<void(const AiWebStatus&)> onStatus_;

    bool SourceAllowed(const std::wstring& source) const {
        return IsWebViewSourceAllowed(expectedOrigin_, source);
    }

    void OnWindowAction(const std::wstring& action) {
        LogDbg(L"[aiui] 收到页面窗口动作：%s", action.c_str());
        if (onWindowAction_) onWindowAction_(action);
    }

    void OnStatus(const AiWebStatus& status) {
        LogDbg(L"[aiui] 收到页面 AI 状态：%s / %s", status.state.c_str(),
               status.summary.c_str());
        if (onStatus_) onStatus_(status);
    }

    void Fail(const std::wstring& text) {
        reason_ = text;
        LogDbg(L"[aiui] %s", reason_.c_str());
        if (onError_) onError_(reason_);
    }
};

namespace {

// -------------------------------------------------------------------------
// 完成回调的接口声明
// -------------------------------------------------------------------------
//
// 【这一段是本次排查最贵的教训 —— 必须显式声明纯虚 Invoke】
//
// 官方 IDL：
//   interface ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
//       : public IUnknown {
//     HRESULT Invoke(HRESULT errorCode, ICoreWebView2Environment* createdEnvironment);
//   }
//
// 错误的写法（之前就是它）：类直接 `: public IUnknown`，然后写一个**普通**
// 成员函数 `Invoke(...)`。它不带 virtual，所以**根本不进 vtable** ——
// vtable 里槽位 3 仍然是"虚析构的 complete destructor"。WebView2 按槽位 3
// 调它的"Invoke"，实际调到了析构函数：对象被就地销毁，随后访问成员
// → 0xC0000005，且崩溃地址落在我们自己的 .rdata 上（看起来像"loader 崩了"）。
//
// 实测证据（tools/ 下的独立探针，逐槽位调用）：
//   vt[0]=QI ✓  vt[1]=AddRef ✓  vt[2]=Release ✓  vt[3]=崩溃
//   改成显式纯虚接口后：vt[3] → Invoke ✓
//
// 【第二个教训】必须按 IDL 逐字抄函数签名。之前 CreateCoreWebView2EnvironmentWithOptions
// 多写了一个 IUnknown** 出参（官方只有 4 个参数、无出参），loader 把第 5 个实参
// 当输出缓冲往里写，同样在调用内部崩。
//
// 【为什么析构不放进 vtable 靠前的位置】Itanium ABI 下虚析构占 2 个槽，
// 但它排在**所有已声明虚函数之后**。只要接口里的 Invoke 是纯虚且先声明，
// 槽位 3 就稳定是 Invoke。
// 【为什么只声明一个接口】环境完成回调与控制器完成回调的**字节形状完全相同**
// （IUnknown 的 3 个方法 + 一个 Invoke(HRESULT, IUnknown*)），只是 IID 不同。
// 声明两个结构再让一个类同时继承，会引入"两个 IUnknown 子对象"的歧义，
// 而 COM 调用方只关心前 4 个槽位是否按序排列 —— 一个结构足够，用成员 iid_
// 决定 QueryInterface 认哪个 IID。
struct IWebView2CompletedHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode,
                                             IUnknown* result) = 0;
};

// 完成回调的实现。Kind 区分该把结果交给环境的下一步还是控制器的下一步。
class CompletedHandler : public IWebView2CompletedHandler {
public:
    enum class Kind { kEnvironment, kController };

    CompletedHandler(WebViewImpl* owner, std::weak_ptr<WebViewCallbackGate> gate,
                     uint64_t generation, Kind kind, const GUID& iid)
        : owner_(owner), gate_(std::move(gate)), generation_(generation),
          kind_(kind), iid_(iid) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        // 接受本回调接口的 IID 与 IUnknown，其余一律 E_NOINTERFACE。
        //
        // 【排查记录】曾为定位"回调不派发"把这里放宽成"任何 IID 都返回自己"，
        // 结果依然收不到回调 —— 证明问题不在 QI。已恢复严格判定：
        // 放宽会让调用方拿到一个不属于该接口的 vtable，是真实的崩溃隐患。
        if (IsEqualGUID(riid, iid_) || IsEqualGUID(riid, IID_IUnknown)) {
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

    // 槽位 3：完成回调的协议方法。**必须是 override** —— 它的位置由基类的
    // 纯虚声明决定（见上面 IWebView2CompletedHandler 的说明）。
    //
    // 【保留这条日志】它是"回调到底有没有送到"的唯一分界点：
    // 没有它就无法区分「回调没派发」（消息泵/COM 问题）与「回调到了但某步失败」
    // （接口转型/几何问题）—— 两者的修法完全不同，而现象都是"面板一片空白"。
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, IUnknown* result) override {
        LogDbg(L"[aiui] CompletedHandler::Invoke kind=%d hr=0x%08lX gen=%llu",
               static_cast<int>(kind_), static_cast<unsigned long>(hr),
               static_cast<unsigned long long>(generation_));
        // 门闩消失 = Owner 已析构；代际不同 = Stop/Retry 后的旧回调。两者都不能
        // 再碰 owner_，否则就是 use-after-free 或对新建实例的跨代写污染。
        auto gate = gate_.lock();
        if (!gate || gate->generation.load() != generation_) return S_OK;
        if (kind_ == Kind::kEnvironment) owner_->OnEnvironmentReady(result, hr);
        else owner_->OnControllerReady(result, hr);
        return S_OK;
    }

private:
    virtual ~CompletedHandler() = default;
    std::atomic<ULONG> refs_{1};
    WebViewImpl* owner_;
    std::weak_ptr<WebViewCallbackGate> gate_;
    uint64_t generation_;
    Kind kind_;
    GUID iid_;
};

// NavigationCompleted 事件处理器：官方接口同样是 IUnknown + Invoke，
// 只是 Invoke 签名变成 (sender, args)。槽位 3 依然是 Invoke。
struct INavCompletedHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IUnknown* sender, IUnknown* args) = 0;
};

class NavCompletedHandler : public INavCompletedHandler {
public:
    NavCompletedHandler(WebViewImpl* owner, std::weak_ptr<WebViewCallbackGate> gate,
                        uint64_t generation)
        : owner_(owner), gate_(std::move(gate)), generation_(generation) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, kIidNavCompleted) || IsEqualGUID(riid, IID_IUnknown)) {
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

    HRESULT STDMETHODCALLTYPE Invoke(IUnknown*, IUnknown*) override {
        auto gate = gate_.lock();
        if (!gate || gate->generation.load() != generation_) return S_OK;
        owner_->OnNavigationCompleted();
        return S_OK;
    }

private:
    virtual ~NavCompletedHandler() = default;
    std::atomic<ULONG> refs_{1};
    WebViewImpl* owner_;
    std::weak_ptr<WebViewCallbackGate> gate_;
    uint64_t generation_;
};

}  // namespace

// ---------------------------------------------------------------------------
// WebViewImpl
// ---------------------------------------------------------------------------

bool WebViewImpl::Start(HWND parent, const std::wstring& url, const std::wstring& userDataDir) {
    if (Ready()) return true;
    // 失败是终态但可重建：返回 false 让面板显示原因；Stop/Retry 会清掉 reason_。
    if (startIssued_ && !reason_.empty()) return false;
    if (startIssued_) return true;   // 仍在异步创建中
    BumpGeneration();               // 让上一轮 Stop 前的回调彻底失效
    reason_.clear();
    startIssued_ = true;
    parent_ = parent;
    url_ = url;
    expectedOrigin_ = UrlOrigin(url);
    userDataDir_ = userDataDir;

    if (!parent) { Fail(L"父窗口无效"); return false; }
    if (!LoadLoader()) {
        Fail(L"找不到 WebView2Loader.dll");
        return false;
    }

    auto* handler = new CompletedHandler(this, gate_, Generation(),
        CompletedHandler::Kind::kEnvironment, kIidEnvCompleted);
    // 预建用户数据目录：WebView2 自己也会建，但目录**不存在且父目录权限受限**时
    // 它会静默失败（S_OK 返回、回调永不触发）。先建好能把这种情况排除掉。
    if (!userDataDir_.empty()) tf::EnsureDir(userDataDir_);
    // 4 个参数，没有出参：环境通过 handler 的 Invoke 异步交付。
    const HRESULT hr = g_createEnv(
        nullptr,
        userDataDir_.empty() ? nullptr : userDataDir_.c_str(),
        nullptr,
        static_cast<IUnknown*>(handler));
    LogDbg(L"[aiui] CreateCoreWebView2EnvironmentWithOptions -> hr=0x%08lX",
           static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        Fail(L"CreateCoreWebView2EnvironmentWithOptions 失败（" +
             WebViewHrText(hr) + L"；用户目录=" + userDataDir_ + L"）");
        handler->Release();   // 没交出去，自己放
        return false;
    }
    // 没有出参可放：环境对象只能从 handler->Invoke 里拿（见官方 IDL）。
    return true;
}

void WebViewImpl::OnEnvironmentReady(IUnknown* env, HRESULT hr) {
    // 无条件入口日志：排查"回调到底有没有送到"时，这条是唯一的分界点。
    // 没有它就无法区分「回调没派发」和「回调到了但内部某步失败」——
    // 两者的修法完全不同（前者是消息循环/COM 问题，后者是接口转型问题）。
    LogDbg(L"[aiui] OnEnvironmentReady 进入：hr=0x%08lX env=%p",
           static_cast<unsigned long>(hr), static_cast<void*>(env));
    if (FAILED(hr) || !env) {
        // 多实例/多 profile 若共用同一个 user-data-dir，Chromium 单例锁会让
        // 后创建的一方拿到失败回调。把目录打进文案，用户能直接看出是不是撞了。
        Fail(L"WebView2 环境创建失败（" + WebViewHrText(hr) +
             L"；用户目录=" + userDataDir_ + L"）。若该目录正被另一个 ExBarBrowser "
             L"实例使用，请关闭后者或为当前实例指定独立 profile");
        return;
    }
    if (env_) { env_->Release(); env_ = nullptr; }
    env_ = GetIface<IEnv>(env, kIidEnvironment);
    if (!env_) {
        // 每个失败分支都必须落盘。只在内存里写 reason_，日志里就只剩
        // "进入" 而没有后续 —— 看起来跟"回调没送到"一模一样，
        // 而这正是排查时最不该混的两种情况。
        Fail(L"环境接口转型失败");
        return;
    }
    LogDbg(L"[aiui] 环境接口就绪，开始创建控制器（父窗口=%p）",
           static_cast<void*>(parent_));

    auto* handler = new CompletedHandler(this, gate_, Generation(),
        CompletedHandler::Kind::kController, kIidCtrlCompleted);
    const HRESULT hr2 = env_->CreateCoreWebView2Controller(parent_,
                                                           static_cast<IUnknown*>(handler));
    LogDbg(L"[aiui] CreateCoreWebView2Controller -> hr=0x%08lX",
           static_cast<unsigned long>(hr2));
    if (FAILED(hr2)) {
        Fail(L"CreateCoreWebView2Controller 失败（" + WebViewHrText(hr2) + L"）");
        handler->Release();
    }
}

void WebViewImpl::OnControllerReady(IUnknown* ctrl, HRESULT hr) {
    LogDbg(L"[aiui] OnControllerReady 进入：hr=0x%08lX ctrl=%p",
           static_cast<unsigned long>(hr), static_cast<void*>(ctrl));
    if (FAILED(hr) || !ctrl) {
        Fail(L"WebView2 控制器创建失败（" + WebViewHrText(hr) + L"）");
        return;
    }
    if (ctrl_) { ctrl_->Close(); ctrl_->Release(); ctrl_ = nullptr; }
    ctrl_ = GetIface<ICtrl>(ctrl, kIidController);
    if (!ctrl_) {
        Fail(L"控制器接口转型失败");
        return;
    }

    // 顺序：先用父窗口实时客户区校正尺寸（控件可能在 0×0 宿主上创建），
    // 再确保可见，最后才导航。反过来会出现"内容已渲染但没有可见合成表面"的
    // 白屏 —— 导航和 JS 都成功，用户却只看到一片空白。
    SyncBoundsFromParent();
    ApplyBounds();
    ctrl_->put_IsVisible(visible_ ? TRUE : FALSE);   // 尊重面板此前的隐藏意图

    IUnknown* core = nullptr;
    if (FAILED(ctrl_->get_CoreWebView2(&core)) || !core) {
        Fail(L"取 CoreWebView2 失败");
        if (ctrl_) ctrl_->put_IsVisible(FALSE);   // 失败路径不能留下白色子窗口
        return;
    }
    if (core_) {
        if (navHooked_) static_cast<ICore*>(core_)->remove_NavigationCompleted(navToken_);
        if (msgHooked_) static_cast<ICore*>(core_)->remove_WebMessageReceived(msgToken_);
        navHooked_ = false;
        msgHooked_ = false;
        core_->Release();
        core_ = nullptr;
    }
    core_ = GetIface<ICore>(core, kIidCore);   // 自己的引用
    core->Release();                           // 放掉 get 给出的那份

    if (!core_) {
        Fail(L"CoreWebView2 接口转型失败");
        if (ctrl_) ctrl_->put_IsVisible(FALSE);
        return;
    }

    // 宿主→页面主题消息依赖 IsWebMessageEnabled。默认值会随运行时版本变化，
    // 这里显式打开；设置改动要在首次导航前做，否则要等下一次导航才生效。
    IUnknown* settingsRaw = nullptr;
    if (SUCCEEDED(core_->get_Settings(&settingsRaw)) && settingsRaw) {
        ISettings* settings = GetIface<ISettings>(settingsRaw, kIidSettings);
        if (settings) {
            settings->put_IsWebMessageEnabled(TRUE);
            settings->Release();
        }
        settingsRaw->Release();
    }

    // 页面自己 reload 时不会重新带 ?exbarTheme=，也不保留 WebMessage；
    // 注册 NavigationCompleted，每次文档完成后重发一次当前主题。
    if (!navHooked_) {
        auto* nav = new NavCompletedHandler(this, gate_, Generation());
        const HRESULT navHr = core_->add_NavigationCompleted(
            static_cast<IUnknown*>(nav), &navToken_);
        if (SUCCEEDED(navHr)) {
            navHooked_ = true;                 // WebView2 已接管这份引用
        } else {
            LogDbg(L"[aiui] add_NavigationCompleted 失败 hr=0x%08lX",
                   static_cast<unsigned long>(navHr));
            nav->Release();                    // 没交出去，自己回收
        }
    }

    // 页面 → 宿主：窗口动作 + AI 状态，二者都过来源与白名单校验。
    // 必须先注册再导航，否则首帧 postMessage 会在监听器装好前被丢掉。
    if (!msgHooked_) {
        auto* msg = new WebMessageHandler(
            gate_, Generation(),
            [this](const std::wstring& source) { return SourceAllowed(source); },
            [this](const std::wstring& action) { OnWindowAction(action); },
            [this](const AiWebStatus& status) { OnStatus(status); });
        const HRESULT msgHr = core_->add_WebMessageReceived(
            static_cast<IUnknown*>(msg), &msgToken_);
        if (SUCCEEDED(msgHr)) {
            msgHooked_ = true;   // WebView2 已接管这份引用
        } else {
            LogDbg(L"[aiui] add_WebMessageReceived 失败 hr=0x%08lX",
                   static_cast<unsigned long>(msgHr));
            msg->Release();      // 没交出去，自己回收
        }
    }

    if (!url_.empty()) {
        const HRESULT navHr = core_->Navigate(url_.c_str());
        LogDbg(L"[aiui] 导航到 %s -> hr=0x%08lX", url_.c_str(),
               static_cast<unsigned long>(navHr));
    }
    PostTheme();   // 首次页面可能还没装好监听器，URL 上的 exbarTheme 是首帧兜底
    reason_.clear();
    LogDbg(L"[aiui] WebView2 已就绪");
    // 通知宿主补一次真实尺寸：控件常在宿主 0×0 时发起创建，就绪后必须
    // 从父窗口重新取一次客户区，否则会停在零尺寸而不建立可见合成表面（白屏）。
    if (onReady_) onReady_();
}

void WebViewImpl::SetBounds(const RECT& rc) {
    bounds_ = rc;
    boundsValid_ = true;
    ApplyBounds();
}

void WebViewImpl::SetVisible(bool visible) {
    visible_ = visible;
    if (ctrl_) ctrl_->put_IsVisible(visible ? TRUE : FALSE);
}

void WebViewImpl::Stop() {
    BumpGeneration();   // 所有在途回调失效
    if (core_) {
        if (navHooked_) {
            static_cast<ICore*>(core_)->remove_NavigationCompleted(navToken_);
            navHooked_ = false;
        }
        if (msgHooked_) {
            static_cast<ICore*>(core_)->remove_WebMessageReceived(msgToken_);
            msgHooked_ = false;
        }
        core_->Release();
        core_ = nullptr;
    }
    if (ctrl_) { ctrl_->Close(); ctrl_->Release(); ctrl_ = nullptr; }
    if (env_) { env_->Release(); env_ = nullptr; }
    startIssued_ = false;
    reason_.clear();
    parent_ = nullptr;
    url_.clear();
    expectedOrigin_.clear();
    userDataDir_.clear();
}

// ---------------------------------------------------------------------------
// WebView（公共门面）
// ---------------------------------------------------------------------------

WebView::WebView() : impl_(new WebViewImpl()) {}

WebView::~WebView() {
    delete impl_;
    impl_ = nullptr;
}

bool WebView::Start(HWND parent, const std::wstring& url, const std::wstring& userDataDir) {
    return impl_->Start(parent, url, userDataDir);
}

void WebView::SetBounds(const RECT& rc) { impl_->SetBounds(rc); }
void WebView::SetVisible(bool visible) { impl_->SetVisible(visible); }
void WebView::NotifyParentMoved() { impl_->NotifyParentMoved(); }
void WebView::SetTheme(bool dark) { impl_->SetTheme(dark); }
void WebView::Stop() { impl_->Stop(); }
void WebView::SetReadyCallback(std::function<void()> cb) { impl_->onReady_ = std::move(cb); }
void WebView::SetErrorCallback(std::function<void(const std::wstring&)> cb) {
    impl_->onError_ = std::move(cb);
}
void WebView::SetWindowActionCallback(std::function<void(const std::wstring&)> cb) {
    impl_->onWindowAction_ = std::move(cb);
}
void WebView::SetStatusCallback(std::function<void(const AiWebStatus&)> cb) {
    impl_->onStatus_ = std::move(cb);
}
bool WebView::Ready() const { return impl_->Ready(); }
const std::wstring& WebView::Reason() const { return impl_->Reason(); }

void WebView::OnEnvironmentReady(IUnknown* env, HRESULT hr) {
    impl_->OnEnvironmentReady(env, hr);
}
void WebView::OnControllerReady(IUnknown* ctrl, HRESULT hr) {
    impl_->OnControllerReady(ctrl, hr);
}

// ---------------------------------------------------------------------------
// 运行时可探测性
// ---------------------------------------------------------------------------

bool RuntimeAvailable(std::wstring* version, std::wstring* reason) {
    if (version) version->clear();
    if (reason) reason->clear();

    if (!LoadLoader()) {
        if (reason) *reason = L"找不到 WebView2Loader.dll（应与 ExBarBrowser.exe 同目录）";
        return false;
    }
    if (!g_getVersion) {
        if (reason) *reason = L"WebView2Loader.dll 缺少版本探测导出";
        return false;
    }
    LPWSTR v = nullptr;
    const HRESULT hr = g_getVersion(nullptr, &v);
    if (SUCCEEDED(hr) && v) {
        if (version) *version = v;
        CoTaskMemFree(v);
        return true;
    }
    if (reason) *reason = L"未安装 WebView2 Runtime（可安装微软的 Evergreen Runtime）";
    return false;
}

}  // namespace aiui