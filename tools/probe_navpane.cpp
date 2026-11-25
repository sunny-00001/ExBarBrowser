// 探针 4：能否在进程内长出**系统原生的导航窗格**（EP_NavPane）。
// 成功 = 图标/分组/驱动器全部与资源管理器一致，不用自己造树。
// 只用于诊断，不进产品构建。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <commctrl.h>
#include <commoncontrols.h>

#include <cstdio>
#include <string>

static std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

static void DumpWindowTree(HWND root, int depth) {
    if (depth > 4) return;
    wchar_t cls[128] = L"";
    GetClassNameW(root, cls, 128);
    RECT rc{};
    GetWindowRect(root, &rc);
    printf("%*s%s [%s] vis=%d\n", depth * 2, "", U8(cls).c_str(), "", IsWindowVisible(root) ? 1 : 0);
    for (HWND c = GetWindow(root, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        DumpWindowTree(c, depth + 1);
    }
}

static void Pump(int ms) {
    const DWORD end = GetTickCount() + ms;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

static bool SaveBmp(HWND hwnd, const wchar_t* path) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC dc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    const size_t stride = (static_cast<size_t>(w) * 4 + 3) & ~3u;
    std::string raw(stride * h, '\0');
    BITMAPINFO* pbi = &bi;
    GetDIBits(mem, bmp, 0, h, raw.data(), pbi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = static_cast<DWORD>(sizeof(fh) + sizeof(bi) + raw.size());
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(raw.data(), 1, raw.size(), f);
    fclose(f);
    return true;
}

class Site : public IServiceProvider, public IExplorerPaneVisibility {
public:
    LONG ref = 1;
    bool navOn = true;
    bool contentOn = true;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IServiceProvider) {
            *ppv = static_cast<IServiceProvider*>(this);
        } else if (riid == IID_IExplorerPaneVisibility) {
            *ppv = static_cast<IExplorerPaneVisibility*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { return (ULONG)InterlockedDecrement(&ref); }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID guid, REFIID riid, void** ppv) override {
        if (guid == SID_ExplorerPaneVisibility && riid == IID_IExplorerPaneVisibility) {
            *ppv = static_cast<IExplorerPaneVisibility*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetPaneState(REFEXPLORERPANE ep,
                                           EXPLORERPANESTATE* peps) override {
        if (!peps) return E_POINTER;
        if (ep == EP_NavPane) {
            *peps = navOn ? (EXPLORERPANESTATE)(EPS_DEFAULT_ON | EPS_INITIALSTATE | EPS_FORCE)
                          : (EXPLORERPANESTATE)(EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        } else if (ep == EP_Commands || ep == EP_Ribbon) {
            *peps = (EXPLORERPANESTATE)(EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        } else {
            *peps = contentOn ? (EXPLORERPANESTATE)(EPS_DEFAULT_ON | EPS_INITIALSTATE | EPS_FORCE)
                              : (EXPLORERPANESTATE)(EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        }
        return S_OK;
    }
};

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

static void Run(HWND host, bool navOn, bool contentOn, bool frames, const wchar_t* tag) {
    printf("\n===== nav=%d content=%d frames=%d =====\n", navOn ? 1 : 0, contentOn ? 1 : 0,
           frames ? 1 : 0);
    IExplorerBrowser* browser = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IExplorerBrowser, reinterpret_cast<void**>(&browser));
    printf("CoCreateInstance hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr) || !browser) {
        return;
    }
    auto* site = new Site();
    site->navOn = navOn;
    site->contentOn = contentOn;
    printf("SetSite...\n");
    IUnknown_SetSite(browser, static_cast<IUnknown*>(static_cast<IServiceProvider*>(site)));
    printf("SetSite ok\n");
    using InitFn = HRESULT(STDMETHODCALLTYPE*)(IExplorerBrowser*, HWND, const RECT*, int);
    void** vt = *reinterpret_cast<void***>(browser);
    auto init = reinterpret_cast<InitFn>(vt[3]);
    RECT rc{0, 0, 900, 600};
    printf("init...\n");
    hr = init(browser, host, &rc, 1 /*SHCWN_SHOW*/);
    printf("init done\n");
    printf("Initialize hr=0x%08lX\n", (unsigned long)hr);
    browser->SetOptions(frames ? EBO_SHOWFRAMES : EBO_NONE);
    IShellItem* pc = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr,
                                       IID_IShellItem, reinterpret_cast<void**>(&pc))) &&
        pc) {
        browser->BrowseToObject(pc, 0);
        pc->Release();
    }
    Pump(3000);
    DumpWindowTree(host, 0);
    wchar_t path[160];
    swprintf(path, 160, L"C:\\Users\\NoteBook\\probe_navpane_%s.bmp", tag);
    SaveBmp(host, path);
    browser->Destroy();
    browser->Release();
    Pump(300);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ProbeNavPaneHost";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"ProbeNavPaneHost", L"probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 920, 620, nullptr, nullptr, inst, nullptr);

    printf("probe start\n");
    Run(host, true, true, true, L"a_nav_content_frames");
    Run(host, true, false, true, L"b_nav_only_frames");
    Run(host, true, true, false, L"c_nav_content_noframes");
    Run(host, true, false, false, L"d_nav_only_noframes");
    fflush(stdout);
    return 0;
}
