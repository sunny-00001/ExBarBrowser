// 探针 84：量化"建视图 / 关视图"的完整成本 —— 关标签卡顿的归属。
//
// 【要回答什么】App::CloseTab 里 tabs_.erase(id) 会析构 Tab（含 1~2 个 ShellView）。
// ShellView::Destroy 做的事全在 UI 线程：
//     IUnknown_SetSite(nullptr) -> Unadvise -> IExplorerBrowser::Destroy -> Release
//     DestroyWindow(container)
// 其中 browser_->Destroy() 要 shell 把视图拆掉（可能释放 NSE、等线程）。
// 关标签卡不卡，取决于这一段到底多少毫秒。
//
// 本探针在一个隐藏宿主窗口上重复"建 -> 导航 -> 销毁"，分段计时：
//   A. CoCreateInstance(IExplorerBrowser)
//   B. Initialize
//   C. BrowseToIDList（首次导航）
//   D. Destroy（关视图的全部清理）
//
// 合规：只读 shell，自建隐藏窗口；不发合成输入、不动光标、不抢焦点（R21）。
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>

namespace {

struct Timer {
    LARGE_INTEGER f{}, a{}, b{};
    Timer() {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
    }
    double Ms() {
        QueryPerformanceCounter(&b);
        return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 /
               static_cast<double>(f.QuadPart);
    }
};

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// 建一个 IExplorerBrowser 并导航到 path，返回三段耗时；销毁段单独测。
struct Costs {
    double create = 0, init = 0, browse = 0, destroy = 0, destroyWin = 0;
    bool ok = false;
};

Costs OneRound(HWND host, const wchar_t* path, const wchar_t* cls) {
    Costs c;

    IExplorerBrowser* eb = nullptr;
    {
        Timer t;
        HRESULT hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IExplorerBrowser, reinterpret_cast<void**>(&eb));
        c.create = t.Ms();
        if (FAILED(hr) || !eb) return c;
    }

    HWND container = CreateWindowExW(0, cls, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 200, 400,
                                     host, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!container) {
        eb->Release();
        return c;
    }

    RECT rc{0, 0, 200, 400};
    FOLDERSETTINGS fs{};
    fs.ViewMode = FVM_DETAILS;
    {
        Timer t;
        HRESULT hr = eb->Initialize(container, &rc, &fs);
        c.init = t.Ms();
        if (FAILED(hr)) {
            eb->Destroy();
            eb->Release();
            DestroyWindow(container);
            return c;
        }
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path, nullptr, &pidl, 0, nullptr)) && pidl) {
        Timer t;
        eb->BrowseToIDList(pidl, SBSP_ABSOLUTE);
        c.browse = t.Ms();
        ILFree(pidl);
    }

    {
        Timer t;
        eb->Destroy();
        c.destroy = t.Ms();
    }
    eb->Release();
    {
        Timer t;
        DestroyWindow(container);
        c.destroyWin = t.Ms();
    }
    c.ok = true;
    return c;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Probe84Host";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"Probe84Host", L"", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!host) {
        printf("建宿主失败\n");
        CoUninitialize();
        return 1;
    }
    RegisterClassExW(&wc);   // container 复用同一类

    printf("=== 建视图 / 关视图 分段成本（每轮 = 一个标签的视图生命周期）===\n");
    printf("%-4s %-22s %8s %8s %8s %9s %9s\n", "轮", "目录", "Create", "Init", "Browse",
           "Destroy", "DestroyW");

    const wchar_t* dirs[] = {L"C:\\Windows", L"E:\\ExBarBrowser\\tools"};
    for (int i = 0; i < 6; ++i) {
        const wchar_t* p = dirs[i % 2];
        Costs c = OneRound(host, p, L"Probe84Host");
        if (!c.ok) {
            printf("  第 %d 轮失败\n", i + 1);
            continue;
        }
        printf("%-4d %-22ls %8.2f %8.2f %8.2f %9.2f %9.2f\n", i + 1, p, c.create, c.init,
               c.browse, c.destroy, c.destroyWin);
    }

    DestroyWindow(host);
    printf("\n【判读】\n");
    printf("  · Destroy 那列就是关标签里 tabs_.erase 触发的清理成本。\n");
    printf("  · 若它达到几十毫秒，关标签的卡顿就来自这里 —— 可以改成\n");
    printf("    先把视图从可见树上摘掉（用户立刻看到关闭），再延后 Destroy。\n");
    printf("  · 若它很小，说明关闭慢在别处（Layout / SyncChrome / 会话落盘）。\n");
    CoUninitialize();
    return 0;
}