// 探针 18：验证两件决定架构的事
//   A. EP_ContentPane = OFF 时，ExplorerBrowser 是否"只显示导航窗格"（免裁剪 hack）
//      并打印导航列真实宽度
//   B. 「神谕」通道：TVM_SELECTITEM 选中树节点 -> nav browser 导航 ->
//      OnNavigationComplete 回传 PIDL -> 反解出该节点的真实路径。
//      若成立，就有了节点<->路径的可靠映射手段（可按显示名匹配 + 神谕消歧）。
//   C. 全树文本扫描：统计同名节点（如 "Downloads"/"文档"）的数量，评估歧义规模。
//   D. 端到端：树选中 -> 神谕得路径 -> 用该路径导航另一个 browser（视图）成功。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <servprov.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>

// mingw 只导出 EP_NavPane，其余面板 GUID 手写。
static const GUID kEP_NavPane = {0xcb316b22, 0x25f7, 0x42b8,
                                 {0x8a, 0x09, 0x54, 0x0d, 0x23, 0xa4, 0x3c, 0x2f}};
static const GUID kEP_ContentPane = {0xe57c5f6d, 0xa203, 0x4a25,
                                     {0xa5, 0xdc, 0x59, 0x02, 0x12, 0x7d, 0x67, 0xf1}};

static std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}
static std::wstring FSPathOf(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return L"";
    PWSTR p = nullptr;
    if (FAILED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &p)) || !p) return L"";
    std::wstring r = p;
    CoTaskMemFree(p);
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
static std::wstring ItemText(HWND tree, HTREEITEM it) {
    wchar_t t[256] = L"";
    TVITEMW tv{}; tv.mask = TVIF_TEXT; tv.hItem = it; tv.pszText = t; tv.cchTextMax = 256;
    SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&tv));
    return t;
}
static std::wstring CaretText(HWND tree) {
    if (!tree) return L"(no tree)";
    HTREEITEM it = reinterpret_cast<HTREEITEM>(SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CARET, 0));
    return it ? ItemText(tree, it) : L"(none)";
}

// ---------- Site：控制各面板开关 ----------
class Site : public IServiceProvider, public IExplorerPaneVisibility {
public:
    bool navPaneOn = true;
    bool contentPaneOn = true;
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
        if (ep == kEP_NavPane) {
            *pe = navPaneOn ? (EXPLORERPANESTATE)(EPS_DEFAULT_ON | EPS_INITIALSTATE | EPS_FORCE)
                            : (EXPLORERPANESTATE)(EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        } else if (ep == kEP_ContentPane) {
            *pe = contentPaneOn ? (EXPLORERPANESTATE)(EPS_DEFAULT_ON | EPS_INITIALSTATE | EPS_FORCE)
                                : (EXPLORERPANESTATE)(EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        } else {
            *pe = EPS_DONTCARE;
        }
        return S_OK;
    }
};

// ---------- Events：记录导航回调（神谕来源） ----------
struct NavRecord {
    int seq = 0;
    std::wstring fsPath;
    std::wstring display;
};
static std::vector<NavRecord> g_navLog;
static bool g_self = false;
static int g_navSeq = 0;

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
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE pidl) override {
        NavRecord r;
        r.seq = ++g_navSeq;
        r.fsPath = FSPathOf(pidl);
        PWSTR d = nullptr;
        if (pidl) SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &d);
        r.display = d ? d : L"";
        if (d) CoTaskMemFree(d);
        g_navLog.push_back(r);
        printf("      >> nav#%d%s: fs=%s  disp=%s\n", r.seq, g_self ? " [自]" : "",
               U8(r.fsPath.c_str()).c_str(), U8(r.display.c_str()).c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override { return S_OK; }
};

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

// 用 RECT 初始化 ExplorerBrowser（mingw 的 Initialize 声明有 ABI 坑，走 vtable）
static IExplorerBrowser* MakeBrowser(HWND host, int rcW, int rcH, Site* site) {
    IExplorerBrowser* browser = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IExplorerBrowser, reinterpret_cast<void**>(&browser))) ||
        !browser)
        return nullptr;
    IUnknown_SetSite(browser, static_cast<IUnknown*>(static_cast<IServiceProvider*>(site)));
    using InitFn = HRESULT(STDMETHODCALLTYPE*)(IExplorerBrowser*, HWND, const RECT*, int);
    void** vt = *reinterpret_cast<void***>(browser);
    auto init = reinterpret_cast<InitFn>(vt[3]);
    RECT rc{0, 0, rcW, rcH};
    init(browser, host, &rc, 0);
    browser->SetOptions(EBO_SHOWFRAMES);
    DWORD cookie = 0;
    browser->Advise(new Events(), &cookie);
    return browser;
}
static void DumpChildren(HWND root, const char* tag) {
    printf("  [%s] 窗口树：\n", tag);
    struct C { HWND h; int d; } c{root, 0};
    // 递归打印（用栈式：先收集所有后代）
    std::vector<HWND> all;
    EnumChildWindows(root, [](HWND h, LPARAM lp) -> BOOL {
        static_cast<std::vector<HWND>*>(reinterpret_cast<void*>(lp))->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&all));
    for (HWND h : all) {
        wchar_t cls[128] = L"", txt[128] = L"";
        GetClassNameW(h, cls, 128);
        GetWindowTextW(h, txt, 128);
        RECT rc{};
        GetWindowRect(h, &rc);
        int w = rc.right - rc.left, ht = rc.bottom - rc.top;
        // 深度：用父链算
        int d = 0;
        HWND p = GetParent(h);
        while (p && p != root) { ++d; p = GetParent(p); }
        printf("      %*s%-28s %4dx%-4d  %s\n", d * 2, L"", cls, w, ht, U8(txt).c_str());
    }
}
static bool SaveBmp(HWND hwnd, const wchar_t* path) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC dc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    HANDLE hf = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    BITMAPINFO bi{};
    bi.bmiHeader = ih;
    GetDIBits(mem, bmp, 0, h, nullptr, &bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(ih);
    fh.bfSize = static_cast<DWORD>(sizeof(fh) + sizeof(ih) + static_cast<size_t>(w) * h * 4);
    WriteFile(hf, &fh, sizeof(fh), nullptr, nullptr);
    WriteFile(hf, &ih, sizeof(ih), nullptr, nullptr);
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    WriteFile(hf, px.data(), static_cast<DWORD>(px.size()), nullptr, nullptr);
    CloseHandle(hf);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    printf("  截图已保存: %s (%dx%d)\n", path, w, h);
    return true;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.lpszClassName = L"P18Host"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND root = CreateWindowExW(0, L"P18Host", L"p", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                1200, 700, nullptr, nullptr, inst, nullptr);
    HWND navArea = CreateWindowExW(0, L"P18Host", L"nav",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 300, 640, root,
                                  nullptr, inst, nullptr);
    HWND viewArea = CreateWindowExW(0, L"P18Host", L"view",
                                   WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 300, 0, 700, 640, root,
                                   nullptr, inst, nullptr);

    // ---- A. 导航 browser：NavPane=ON, ContentPane=OFF ----
    printf("=== A. NavPane=ON / ContentPane=OFF ===\n");
    auto* navSite = new Site();
    navSite->navPaneOn = true;
    navSite->contentPaneOn = false;
    IExplorerBrowser* navBrowser = MakeBrowser(navArea, 300, 640, navSite);
    printf("  navBrowser=%p\n", (void*)navBrowser);
    Pump(3000);
    DumpChildren(navArea, "nav(ContentPane=OFF)");
    HWND tree = FindByClass(navArea, L"SysTreeView32");
    printf("  tree=%p caret=%s\n", (void*)tree, U8(CaretText(tree).c_str()).c_str());
    SaveBmp(navArea, L"C:\\Users\\NoteBook\\probe18_navonly.bmp");

    // ---- D. 视图 browser：NavPane=OFF, ContentPane=ON ----
    printf("\n=== D. 视图 browser：NavPane=OFF / ContentPane=ON ===\n");
    auto* viewSite = new Site();
    viewSite->navPaneOn = false;
    viewSite->contentPaneOn = true;
    IExplorerBrowser* viewBrowser = MakeBrowser(viewArea, 700, 640, viewSite);
    Pump(2500);
    DumpChildren(viewArea, "view(NavPane=OFF)");

    // 视图 browser 导航到 Documents
    IShellItem* docItem = nullptr;
    SHCreateItemFromParsingName(L"C:\\Users\\NoteBook\\Documents", nullptr, IID_IShellItem,
                                reinterpret_cast<void**>(&docItem));
    if (docItem) {
        viewBrowser->BrowseToObject(docItem, 0);
        docItem->Release();
    }
    Pump(2000);

    // ---- B. 神谕：选中树节点 -> nav browser 导航 -> 回传路径 ----
    printf("\n=== B. 神谕通道 ===\n");
    struct F {
        static void Walk(HWND t, HTREEITEM i, std::vector<HTREEITEM>* out) {
            while (i) {
                out->push_back(i);
                Walk(t, reinterpret_cast<HTREEITEM>(SendMessageW(
                           t, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(i))),
                       out);
                i = reinterpret_cast<HTREEITEM>(SendMessageW(
                    t, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(i)));
            }
        }
    };
    std::vector<HTREEITEM> nodes;
    HTREEITEM rootItem =
        reinterpret_cast<HTREEITEM>(SendMessageW(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    F::Walk(tree, rootItem, &nodes);
    printf("  树节点数=%zu\n", nodes.size());

    // C. 同名统计
    std::map<std::wstring, int> nameCount;
    for (HTREEITEM it : nodes) nameCount[ItemText(tree, it)]++;
    printf("  同名节点（计数>1）：\n");
    for (auto& kv : nameCount) {
        if (kv.second > 1)
            printf("    「%s」x%d\n", U8(kv.first.c_str()).c_str(), kv.second);
    }

    // 神谕实验：挑几个节点，逐个选中，看回传路径
    printf("\n  -- 逐节点神谕（选前 6 个 + 所有名为 Downloads/文档 的） --\n");
    int tried = 0;
    for (HTREEITEM it : nodes) {
        std::wstring nm = ItemText(tree, it);
        bool pick = tried < 6 || nm == L"Downloads" || nm == L"文档" || nm == L"桌面";
        if (!pick) continue;
        if (tried >= 10) break;
        ++tried;
        int before = g_navSeq;
        g_self = true;
        SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(it));
        Pump(1200);
        printf("    节点「%s」-> 导航回调数=%d  caret=%s\n", U8(nm.c_str()).c_str(),
               g_navSeq - before, U8(CaretText(tree).c_str()).c_str());
    }
    g_self = false;

    // D. 端到端：树选中 -> 神谕路径 -> 导航视图 browser
    printf("\n=== D. 端到端：树 -> 视图 browser ===\n");
    for (HTREEITEM it : nodes) {
        if (ItemText(tree, it) == L"文档") {
            int before = g_navSeq;
            g_self = true;
            SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(it));
            Pump(1200);
            g_self = false;
            if (g_navSeq > before) {
                const NavRecord& r = g_navLog.back();
                printf("  神谕路径 = %s\n", U8(r.fsPath.c_str()).c_str());
                IShellItem* si = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(r.fsPath.c_str(), nullptr,
                                                           IID_IShellItem,
                                                           reinterpret_cast<void**>(&si))) &&
                    si) {
                    viewBrowser->BrowseToObject(si, 0);
                    si->Release();
                    Pump(1500);
                    printf("  视图 browser 已导航到该路径\n");
                }
            }
            break;
        }
    }
    SaveBmp(viewArea, L"C:\\Users\\NoteBook\\probe18_view.bmp");
    fflush(stdout);
    return 0;
}
