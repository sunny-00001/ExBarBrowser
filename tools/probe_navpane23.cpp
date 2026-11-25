// 探针 23：导航列宽度规律 + 裁剪法参数
//   对不同 RECT 宽度，测出：导航列(tree)宽度、内容列宽度、树窗口 x 坐标。
//   由此确定"宿主裁剪宽度"该取多少，以及能否把导航列拉宽。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <servprov.h>
#include <cstdio>
#include <string>

static const GUID kEP_NavPane = {0xcb316b22, 0x25f7, 0x42b8,
                                 {0x8a, 0x09, 0x54, 0x0d, 0x23, 0xa4, 0x3c, 0x2f}};
static std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}
static HWND FindByClass(HWND root, const wchar_t* want) {
    struct C { const wchar_t* w; HWND f; } c{want, nullptr};
    EnumChildWindows(root, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<C*>(lp);
        wchar_t cls[128] = L"";
        GetClassNameW(h, cls, 128);
        if (wcscmp(cls, c->w) == 0) { c->f = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    return c.f;
}
static void Pump(int ms) {
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}
class Site : public IServiceProvider, public IExplorerPaneVisibility {
public:
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IServiceProvider)
            *ppv = static_cast<IServiceProvider*>(this);
        else if (riid == IID_IExplorerPaneVisibility)
            *ppv = static_cast<IExplorerPaneVisibility*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { return (ULONG)InterlockedDecrement(&ref); }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID g, REFIID r, void** p) override {
        if (g == SID_ExplorerPaneVisibility && r == IID_IExplorerPaneVisibility) {
            *p = static_cast<IExplorerPaneVisibility*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetPaneState(REFEXPLORERPANE ep,
                                           EXPLORERPANESTATE* pe) override {
        if (!pe) return E_POINTER;
        if (ep == kEP_NavPane)
            *pe = (EXPLORERPANESTATE)(EPS_DEFAULT_ON | EPS_INITIALSTATE | EPS_FORCE);
        else *pe = EPS_DONTCARE;
        return S_OK;
    }
};
class Events : public IExplorerBrowserEvents {
public:
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExplorerBrowserEvents) {
            *ppv = static_cast<IExplorerBrowserEvents*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { return (ULONG)InterlockedDecrement(&ref); }
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override { return S_OK; }
};
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.lpszClassName = L"P23Host"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND root = CreateWindowExW(0, L"P23Host", L"p", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                1200, 700, nullptr, nullptr, inst, nullptr);
    // 宿主固定 1000 宽（够宽，不会被裁），只改 browser RECT 宽度看布局
    HWND navArea = CreateWindowExW(0, L"P23Host", L"nav",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 1000, 640, root,
                                  nullptr, inst, nullptr);
    auto* site = new Site();
    IExplorerBrowser* browser = nullptr;
    CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_IExplorerBrowser,
                     reinterpret_cast<void**>(&browser));
    IUnknown_SetSite(browser, static_cast<IUnknown*>(static_cast<IServiceProvider*>(site)));
    using InitFn = HRESULT(STDMETHODCALLTYPE*)(IExplorerBrowser*, HWND, const RECT*, int);
    void** vt = *reinterpret_cast<void***>(browser);
    auto init = reinterpret_cast<InitFn>(vt[3]);
    RECT rc{0, 0, 1000, 640};
    init(browser, navArea, &rc, 0);
    browser->SetOptions(EBO_SHOWFRAMES);
    DWORD cookie = 0;
    browser->Advise(new Events(), &cookie);
    IShellItem* pc = nullptr;
    SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                         reinterpret_cast<void**>(&pc));
    if (pc) { browser->BrowseToObject(pc, 0); pc->Release(); }
    Pump(3000);
    HWND tree = FindByClass(navArea, L"SysTreeView32");
    HWND defview = FindByClass(navArea, L"SHELLDLL_DefView");
    printf("tree=%p defview=%p\n", (void*)tree, (void*)defview);

    auto measure = [&](const char* tag) {
        RECT tr{}, dr{};
        GetWindowRect(tree, &tr);
        if (defview) GetWindowRect(defview, &dr);
        MapWindowPoints(nullptr, navArea, reinterpret_cast<POINT*>(&tr), 2);
        if (defview) MapWindowPoints(nullptr, navArea, reinterpret_cast<POINT*>(&dr), 2);
        printf("  %-22s tree: x=%4d w=%4d | defview: x=%4d w=%4d\n", tag, tr.left,
               tr.right - tr.left, dr.left, dr.right - dr.left);
    };
    printf("\n=== A. 初始化 RECT 宽度 -> 布局 ===\n");
    measure("init rc=1000");
    for (int w : {900, 700, 500, 400, 350, 300, 250, 200}) {
        RECT r{0, 0, w, 640};
        browser->SetRect(nullptr, r);
        Pump(700);
        char buf[64];
        snprintf(buf, sizeof(buf), "SetRect w=%d", w);
        measure(buf);
    }

    printf("\n=== B. 窄宿主 + 宽 browser RECT（裁剪法）===\n");
    SetWindowPos(navArea, nullptr, 0, 0, 200, 640, SWP_NOZORDER | SWP_NOACTIVATE);
    RECT r{0, 0, 300, 640};
    browser->SetRect(nullptr, r);
    Pump(900);
    measure("host=200 rc=300");
    SetWindowPos(navArea, nullptr, 0, 0, 160, 640, SWP_NOZORDER | SWP_NOACTIVATE);
    Pump(600);
    measure("host=160 rc=300");
    SetWindowPos(navArea, nullptr, 0, 0, 150, 640, SWP_NOZORDER | SWP_NOACTIVATE);
    Pump(600);
    measure("host=150 rc=300");

    printf("\n=== C. 再 SetRect 一次 300 会不会重排到宿主宽度 ===\n");
    browser->SetRect(nullptr, RECT{0, 0, 300, 640});
    Pump(800);
    measure("again rc=300");
    fflush(stdout);
    return 0;
}
