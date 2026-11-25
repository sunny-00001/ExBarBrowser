// WebView2 环境探测（独立可执行，用于区分"机器环境问题"与"绑定层问题"）。
//
// 不发合成输入，只读；不动光标、不抢焦点。
//
// 输出写到同目录的 wvprobe.log。四段结果分别回答：
//   1. loader DLL 能不能加载（err 码是关键证据）
//   2. 能不能探测到运行时版本（loader 与 runtime 能否通信）
//   3. CreateCoreWebView2EnvironmentWithOptions 返回什么
//   4. 完成回调到底有没有被调用（跑 12 秒消息循环等它）
#include <windows.h>
#include <psapi.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <atomic>

static FILE* g_log = nullptr;

// 兜底：用 WriteFile 直写 UTF-8 文件，避免 _wfopen 的编码参数在不同 CRTL
// 版本下行为不一致（实测 `L"w, ccs=UTF-8"` 在本机写出空文件）。
static void RawW(const wchar_t* line) {
    static HANDLE h = INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        wchar_t p[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        wchar_t* s = wcsrchr(p, L'\\');
        if (s) *(s + 1) = L'\0';
        wcscat_s(p, L"wvprobe.txt");
        h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) return;
    // 转 UTF-8
    const int need = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return;
    char buf[1024];
    const int got = WideCharToMultiByte(CP_UTF8, 0, line, -1, buf, sizeof(buf) - 3,
                                        nullptr, nullptr);
    if (got <= 1) return;
    DWORD wrote = 0;
    WriteFile(h, buf, static_cast<DWORD>(got - 1), &wrote, nullptr);
    WriteFile(h, "\r\n", 2, &wrote, nullptr);
}

static void L(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // 同时写 FILE* 与 RawW
    if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
    wchar_t wbuf[1024];
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 1024);
    RawW(wbuf);
}

// 【必须显式声明纯虚 Invoke】直接从 IUnknown 继承再写普通成员函数 Invoke
// 的话，它不进 vtable，槽位 3 会是虚析构 → 崩溃。详见 src/aichat_webview.cpp 的说明。
struct ICompletedHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, IUnknown* result) = 0;
};

class Handler : public ICompletedHandler {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        L("    QI 被问");
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    // vtable 槽位 3（IUnknown 的 3 个方法之后）
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, IUnknown* result) override {
        L(">>> INVOKE 到达：hr=0x%08lX result=%p", static_cast<unsigned long>(hr),
          static_cast<void*>(result));
        g_invoked = true;
        PostQuitMessage(0);
        return S_OK;
    }
    static bool g_invoked;
    std::atomic<ULONG> refs_{1};
};

bool Handler::g_invoked = false;

// 崩溃兜底：MinGW 不支持 __try/__except，用顶层异常过滤器。
//
// 【必须次序小心】崩溃处理器里不能做任何可能再崩的事 —— 它一旦再崩，
// 我们看到的"崩溃地址"会是处理器自己的地址（实测：落在 .rdata 上的写违例，
// 那是 static 局部变量初始化在异常上下文里被破坏），真因反而被盖住。
// 这里只用**栈上**缓冲与已打开的句柄，不碰任何 static 初始化、不调用 CRT 格式化。
static HANDLE g_crashFile = INVALID_HANDLE_VALUE;

static void CrashRaw(const char* ascii) {
    if (g_crashFile == INVALID_HANDLE_VALUE) {
        wchar_t p[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        wchar_t* s = wcsrchr(p, L'\\');
        if (s) *(s + 1) = L'\0';
        wcscat_s(p, L"wvprobe_crash.txt");
        g_crashFile = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_crashFile == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    const size_t n = lstrlenA(ascii);
    WriteFile(g_crashFile, ascii, static_cast<DWORD>(n), &wrote, nullptr);
    WriteFile(g_crashFile, "\r\n", 2, &wrote, nullptr);
}

static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    char buf[256];
    if (ep && ep->ExceptionRecord) {
        // 自己拼十六进制，不用 printf（避免 CRT 在异常上下文里的 static 初始化）
        const auto code = static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode);
        const auto addr = reinterpret_cast<unsigned long long>(
            ep->ExceptionRecord->ExceptionAddress);
        char* p = buf;
        const char* pre = "CRASH code=0x";
        while (*pre) *p++ = *pre++;
        for (int i = 28; i >= 0; i -= 4) *p++ = "0123456789ABCDEF"[(code >> i) & 0xF];
        const char* mid = " addr=0x";
        while (*mid) *p++ = *mid++;
        for (int i = 60; i >= 0; i -= 4) *p++ = "0123456789ABCDEF"[((addr >> i) & 0xF)];
        *p = '\0';
        CrashRaw(buf);

        if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            const auto kind = ep->ExceptionRecord->ExceptionInformation[0];
            const auto target = ep->ExceptionRecord->ExceptionInformation[1];
            char b2[128];
            char* q = b2;
            const char* k = (kind == 0) ? "  READ target=0x" : "  WRITE target=0x";
            while (*k) *q++ = *k++;
            for (int i = 60; i >= 0; i -= 4)
                *q++ = "0123456789ABCDEF"[(target >> i) & 0xF];
            *q = '\0';
            CrashRaw(b2);
        }
    } else {
        CrashRaw("CRASH (no exception record)");
    }
    if (g_crashFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_crashFile);
        g_crashFile = INVALID_HANDLE_VALUE;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// 【按官方 IDL 逐字抄：4 个参数，没有出参】
// 以前这里多写了一个 IUnknown** 出参，loader 会把第 5 个实参当输出缓冲往里写，
// 在调用内部崩溃（0xC0000005，落在我们自己的 .rdata）。
using PFN_CreateEnv = HRESULT(STDMETHODCALLTYPE*)(PCWSTR, PCWSTR, IUnknown*, IUnknown*);
using PFN_GetVersion = HRESULT(STDMETHODCALLTYPE*)(PCWSTR, LPWSTR*);

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t logPath[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, logPath, MAX_PATH);
    wchar_t* slash = wcsrchr(logPath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(logPath, L"wvprobe.log");
    g_log = _wfopen(logPath, L"w, ccs=UTF-8");
    if (!g_log) return 1;

    L("=== WebView2 环境探测 ===");

    // 【为什么不用 OleInitialize】它在 STA 上会额外装 OLE 的消息过滤与剪贴板挂钩，
    // 排查阶段这些是干扰源：崩溃可能来自 OLE 的钩子而不是 WebView2 本身。
    // 这里用最干净的 CoInitializeEx(STA) —— WebView2 只要求 STA。
    const HRESULT hrOle = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    L("CoInitializeEx(STA) -> 0x%08lX", static_cast<unsigned long>(hrOle));

    // 1) 找 loader
    SetLastError(0);
    HMODULE dll = LoadLibraryW(L"WebView2Loader.dll");
    DWORD e1 = GetLastError();
    L("LoadLibraryW(WebView2Loader.dll) -> %p err=%lu", static_cast<void*>(dll), e1);
    if (!dll) {
        wchar_t dir[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        wchar_t* s = wcsrchr(dir, L'\\');
        if (s) *(s + 1) = L'\0';
        const std::wstring full = std::wstring(dir) + L"WebView2Loader.dll";
        SetLastError(0);
        dll = LoadLibraryW(full.c_str());
        L("LoadLibraryW(full=%ls) -> %p err=%lu", full.c_str(), static_cast<void*>(dll),
          GetLastError());
    }
    if (!dll) {
        L("结论：loader 加载失败。err=193 是格式/策略拒绝，err=126 是找不到。");
        fclose(g_log);
        return 2;
    }

    // 2) 探测运行时版本
    auto getVer = reinterpret_cast<PFN_GetVersion>(
        GetProcAddress(dll, "GetAvailableCoreWebView2BrowserVersionString"));
    L("GetProcAddress(GetAvailable...VersionString) -> %p", reinterpret_cast<void*>(getVer));
    if (getVer) {
        LPWSTR v = nullptr;
        const HRESULT h = getVer(nullptr, &v);
        L("  -> hr=0x%08lX version=%ls", static_cast<unsigned long>(h), v ? v : L"(null)");
        if (v) CoTaskMemFree(v);
    }

    // 3) 创建环境
    auto createEnv = reinterpret_cast<PFN_CreateEnv>(
        GetProcAddress(dll, "CreateCoreWebView2EnvironmentWithOptions"));
    L("GetProcAddress(Create...WithOptions) -> %p", reinterpret_cast<void*>(createEnv));
    if (!createEnv) { fclose(g_log); return 3; }

    // 【关键排查分支】用环境变量 WVPROBE_NOUDF=1 可跳过自定义用户数据目录，
    // 让 WebView2 用默认位置（%LOCALAPPDATA%\EBWebView 之类）。
    // 定制目录引发崩溃是常见原因：目录不存在、路径含非 ASCII、或被安全软件锁定。
    wchar_t flag[8] = L"";
    const DWORD fn = GetEnvironmentVariableW(L"WVPROBE_NOUDF", flag, 8);
    const bool useDefaultUdf = (fn > 0 && flag[0] == L'1');

    wchar_t udf[MAX_PATH] = L"";
    if (!useDefaultUdf) {
        GetModuleFileNameW(nullptr, udf, MAX_PATH);
        wchar_t* s2 = wcsrchr(udf, L'\\');
        if (s2) *(s2 + 1) = L'\0';
        wcscat_s(udf, L"data\\webview2probe");
        L("userDataFolder = %ls", udf);
    } else {
        L("userDataFolder = (默认，不指定)");
    }
    const PCWSTR udfArg = useDefaultUdf ? nullptr : udf;

    Handler* h = new Handler();

    // 【隔离变量：handler 是否为 null】环境变量 WVPROBE_NOHANDLER=1 时传 nullptr。
    // 这能把"崩溃源于我们的回调对象"与"崩溃源于环境/loader 本身"分开 ——
    // 两者修法完全不同（前者查 vtable，后者查运行时装没装对）。
    wchar_t hflag[8] = L"";
    const bool noHandler = GetEnvironmentVariableW(L"WVPROBE_NOHANDLER", hflag, 8) > 0 &&
                           hflag[0] == L'1';

    L("调用 Create...WithOptions 前：handler=%p%s", static_cast<void*>(h),
      noHandler ? L"（本次传 nullptr）" : L"");
    const HRESULT hr = createEnv(nullptr, udfArg, nullptr,
                                 noHandler ? nullptr : static_cast<IUnknown*>(h));
    L("CreateCoreWebView2EnvironmentWithOptions -> 0x%08lX（无出参，等回调）",
      static_cast<unsigned long>(hr));

    // 4) 等回调
    L("进入消息循环（12 秒）…");
    MSG msg{};
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 12000) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }

    L("=== 结论 ===");
    if (Handler::g_invoked) {
        L("回调已到达 → 绑定层与运行时都正常。");
        L("若主程序仍不显示，问题在主程序的 STA/消息泵或面板宿主几何。");
    } else {
        L("回调从未到达 → 运行时未能完成环境创建（本机限制或版本不匹配）。");
    }
    fclose(g_log);
    return Handler::g_invoked ? 0 : 4;
}