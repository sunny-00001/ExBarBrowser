// 离屏探针 v2：判定"IExplorerBrowser 托管的 DefView 选中项是否绘制高亮"。
//
// 判据：同一窗口"选中前/选中后"两次截图的像素差分。
//   选中会画出一整行高亮块 => 差分面积大，且集中在一行带上（peakCount 接近窗口宽）
//   没有高亮                => 差分只有零星文字/图标像素
// 这个判据自校准：不依赖背景色、不依赖主题、不受 DPI 影响。
//
// 对照实验（回答"高亮丢失是不是我们自己改主题造成的"）：
//   (无参数)                 基线：只导航 + 选中
//   --settheme-explorer      对视图子树 SetWindowTheme(L"Explorer")     ← 主程序亮色路径
//   --settheme-dark          对视图子树 SetWindowTheme(L"DarkMode_Explorer")
//   --themechanged           对视图子树发 WM_THEMECHANGED              ← 主程序 PokeTheme
//   --both                   两者都做（等于主程序 ApplyTheme 的完整动作）
//
// 纪律：窗口建在屏幕外、SW_SHOWNOACTIVATE，不抢焦点、不模拟鼠标、不改用户数据。
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <vector>

// 直接用 Win32 写日志：静态链接 libstdc++ 时 CRT 的宽字符格式化在部分
// mingw 版本上会静默失败（探针历史踩过），这里不依赖 stdio 的宽字符路径。
static HANDLE g_logFile = INVALID_HANDLE_VALUE;

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048] = L"";
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    char utf8[8192] = "";
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (n <= 1) return;
    utf8[n - 1] = '\n';
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logFile, utf8, static_cast<DWORD>(n), &written, nullptr);
        FlushFileBuffers(g_logFile);
    }
    fwrite(utf8, 1, static_cast<size_t>(n), stdout);
    fflush(stdout);
}

static void Pump(int ms) {
    const DWORD end = GetTickCount() + ms;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

static HWND FindChildClass(HWND parent, const wchar_t* name) {
    struct Ctx {
        const wchar_t* name;
        HWND found;
    } ctx{name, nullptr};
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[64] = L"";
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, c->name) == 0) {
                c->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// shell 会同时存在两个 DefView：导航窗格一个、文件列表一个。
// 只取客户区最大者 = 文件列表，否则会误测导航树。
static HWND FindLargestChildClass(HWND parent, const wchar_t* name) {
    struct Ctx {
        const wchar_t* name;
        HWND best;
        long area;
    } ctx{name, nullptr, 0};
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[64] = L"";
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, c->name) == 0) {
                RECT rc{};
                GetClientRect(h, &rc);
                const long a = (rc.right - rc.left) * (rc.bottom - rc.top);
                if (a > c->area) {
                    c->area = a;
                    c->best = h;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// 把窗口客户区抓成 32bpp 像素缓冲。PrintWindow(PW_CLIENTONLY) 对 DirectUI 有效。
static bool Grab(HWND h, int* outW, int* outH, std::vector<DWORD>* out) {
    RECT rc;
    GetClientRect(h, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return false;

    HDC dc = GetDC(h);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL ok = PrintWindow(h, mem, PW_CLIENTONLY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    out->assign(static_cast<size_t>(W) * H, 0);
    const int lines = GetDIBits(mem, bmp, 0, H, out->data(), &bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(h, dc);

    if (!ok || lines != H) return false;
    *outW = W;
    *outH = H;
    return true;
}

// 差分统计：差异像素总数、差异最大行的 y、该行差异像素数。
struct Diff {
    int changed = 0;
    int peakRow = 0;
    int peakCount = 0;
};

static Diff Compare(const std::vector<DWORD>& a, const std::vector<DWORD>& b, int W, int H) {
    Diff d;
    if (a.size() != b.size()) return d;
    std::vector<int> perRow(static_cast<size_t>(H), 0);
    for (int y = 0; y < H; ++y) {
        const DWORD* ra = &a[static_cast<size_t>(y) * W];
        const DWORD* rb = &b[static_cast<size_t>(y) * W];
        for (int x = 0; x < W; ++x) {
            const DWORD pa = ra[x] & 0x00FFFFFF, pb = rb[x] & 0x00FFFFFF;
            if (pa == pb) continue;
            // 忽略抗锯齿抖动，只看真实变化
            const int db = abs(static_cast<int>(pa & 0xFF) - static_cast<int>(pb & 0xFF));
            const int dg = abs(static_cast<int>((pa >> 8) & 0xFF) - static_cast<int>((pb >> 8) & 0xFF));
            const int dr = abs(static_cast<int>((pa >> 16) & 0xFF) - static_cast<int>((pb >> 16) & 0xFF));
            if (db + dg + dr < 12) continue;
            ++d.changed;
            ++perRow[static_cast<size_t>(y)];
        }
    }
    for (int y = 0; y < H; ++y) {
        if (perRow[static_cast<size_t>(y)] > d.peakCount) {
            d.peakCount = perRow[static_cast<size_t>(y)];
            d.peakRow = y;
        }
    }
    return d;
}

static void ApplySetTheme(HWND root, const wchar_t* theme) {
    struct Ctx {
        const wchar_t* theme;
    } ctx{theme};
    SetWindowTheme(root, theme, nullptr);
    EnumChildWindows(
        root,
        [](HWND h, LPARAM lp) -> BOOL {
            SetWindowTheme(h, reinterpret_cast<Ctx*>(lp)->theme, nullptr);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
}

// 只对顶层（ExplorerBrowserControl）设主题，不递归子窗口 —— 若干同类项目就是这么做的。
static void ApplySetThemeTopOnly(HWND top, const wchar_t* theme) {
    SetWindowTheme(top, theme, nullptr);
}

// 深色滚动条这类独立控件是否也能单独设主题而不破坏高亮？需要实测。
static void ApplyScrollbarOnly(HWND root, const wchar_t* theme) {
    struct Ctx {
        const wchar_t* theme;
        int n;
    } ctx{theme, 0};
    EnumChildWindows(
        root,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[64] = L"";
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, L"ScrollBar") == 0) {
                SetWindowTheme(h, c->theme, nullptr);
                c->n++;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    Log(L"SetWindowTheme 只作用于 ScrollBar，命中 %d 个", ctx.n);
}

// 主程序还对 shell 视图窗口调了 AllowDarkForWindow（uxtheme ordinal 133）。
// 它改的是"该窗口是否允许深色"，理论上不该影响样式字典，但必须实测确认。
static void ApplyAllowDark(HWND root, bool dark) {
    struct Ctx {
        bool dark;
        int n;
    } ctx{dark, 0};
    auto fn = reinterpret_cast<BOOL(WINAPI*)(HWND, BOOL)>(
        GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(133)));
    if (!fn) {
        Log(L"ordinal 133 不可用，跳过");
        return;
    }
    fn(root, dark ? TRUE : FALSE);
    EnumChildWindows(
        root,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            auto f = reinterpret_cast<BOOL(WINAPI*)(HWND, BOOL)>(
                GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(133)));
            if (f) {
                f(h, c->dark ? TRUE : FALSE);
                c->n++;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    Log(L"AllowDarkForWindow 已作用于 %d 个子窗口", ctx.n);
}

// SetWindowTheme(h, NULL, NULL) 是文档化的"移除先前关联"，用它验证能否把画坏的状态恢复。
static void ApplyResetTheme(HWND root) {
    SetWindowTheme(root, nullptr, nullptr);
    EnumChildWindows(
        root,
        [](HWND h, LPARAM) -> BOOL {
            SetWindowTheme(h, nullptr, nullptr);
            return TRUE;
        },
        0);
}

static void ApplyThemeChanged(HWND root) {
    SendMessageW(root, WM_THEMECHANGED, 0, 0);
    EnumChildWindows(
        root,
        [](HWND h, LPARAM) -> BOOL {
            SendMessageW(h, WM_THEMECHANGED, 0, 0);
            return TRUE;
        },
        0);
}

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t** argv) {
    int setTheme = 0;  // 0=不改 1=Explorer 2=DarkMode_Explorer 3=顶层 Explorer 4=顶层 Dark
    bool themeChanged = false;
    bool resetAfter = false;  // 先画坏再移除关联，验证可恢复
    bool scrollbarOnly = false;
    bool allowDarkTree = false;
    const wchar_t* folder = L"D:\\Windhawk";
    const wchar_t* logPath = L"D:\\Windhawk\\ExBarBrowser\\tools\\probe_result.txt";

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--settheme-explorer") == 0) setTheme = 1;
        if (wcscmp(argv[i], L"--settheme-dark") == 0) setTheme = 2;
        if (wcscmp(argv[i], L"--settheme-top") == 0) setTheme = 3;
        if (wcscmp(argv[i], L"--settheme-top-dark") == 0) setTheme = 4;
        if (wcscmp(argv[i], L"--themechanged") == 0) themeChanged = true;
        if (wcscmp(argv[i], L"--both") == 0) {
            setTheme = 1;
            themeChanged = true;
        }
        if (wcscmp(argv[i], L"--reset-after") == 0) resetAfter = true;
        if (wcscmp(argv[i], L"--scrollbar-only") == 0) scrollbarOnly = true;
        if (wcscmp(argv[i], L"--allowdark-tree") == 0) allowDarkTree = true;
        if (wcscmp(argv[i], L"--folder") == 0 && i + 1 < argc) folder = argv[++i];
        if (wcscmp(argv[i], L"--log") == 0 && i + 1 < argc) logPath = argv[++i];
    }

    g_logFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    Log(L"=== setTheme=%d themeChanged=%d folder=%ls ===", setTheme, themeChanged ? 1 : 0, folder);

    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ProbeHost2";
    RegisterClassW(&wc);

    HWND host = CreateWindowExW(0, L"ProbeHost2", L"", WS_POPUP, -6000, -6000, 900, 600, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(host, SW_SHOWNOACTIVATE);

    IExplorerBrowser* eb = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IExplorerBrowser, reinterpret_cast<void**>(&eb)))) {
        Log(L"CoCreateInstance 失败");
        if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
        return 1;
    }

    using InitFn = HRESULT(STDMETHODCALLTYPE*)(IExplorerBrowser*, HWND, const RECT*, int);
    void** vt = *reinterpret_cast<void***>(eb);
    RECT rc{0, 0, 900, 600};
    HRESULT hr = reinterpret_cast<InitFn>(vt[3])(eb, host, &rc, 0);
    Log(L"Initialize hr=0x%08lX", static_cast<unsigned long>(hr));
    eb->SetOptions(EBO_SHOWFRAMES);
    eb->SetPropertyBag(L"ProbeHighlight2");

    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(folder, nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item)))) {
        Log(L"解析路径失败");
        if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
        return 1;
    }
    eb->BrowseToObject(item, 0);
    item->Release();

    IShellView* view = nullptr;
    for (int i = 0; i < 200 && !view; ++i) {
        Pump(50);
        if (FAILED(eb->GetCurrentView(IID_IShellView, reinterpret_cast<void**>(&view))))
            view = nullptr;
    }
    if (!view) {
        Log(L"视图未就绪");
        if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
        return 1;
    }
    Pump(500);

    HWND viewWnd = nullptr;
    view->GetWindow(&viewWnd);
    // 文件列表 = 客户区最大的 DefView（导航窗格的那个更窄）
    HWND defView = viewWnd ? FindLargestChildClass(viewWnd, L"SHELLDLL_DefView") : nullptr;
    if (!defView) defView = viewWnd;
    Log(L"viewWnd=%p defView=%p", static_cast<void*>(viewWnd), static_cast<void*>(defView));

    // 先清空选择，抓"未选中"基线
    view->SelectItem(nullptr, SVSI_DESELECTOTHERS);
    Pump(400);

    int W = 0, H = 0;
    std::vector<DWORD> before, after;
    if (!Grab(defView, &W, &H, &before)) {
        Log(L"抓图失败");
        view->Release();
        eb->Destroy();
        eb->Release();
        DestroyWindow(host);
        OleUninitialize();
        if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
        return 1;
    }

    // 复现主程序动作（在"已选中"之前做，模拟 ApplyTheme 发生在导航之后）
    if (setTheme) {
        if (setTheme <= 2) {
            const wchar_t* name = (setTheme == 2) ? L"DarkMode_Explorer" : L"Explorer";
            ApplySetTheme(viewWnd, name);
            Log(L"SetWindowTheme(%ls) 已对视图【全子树】生效", name);
        } else {
            const wchar_t* name = (setTheme == 4) ? L"DarkMode_Explorer" : L"Explorer";
            ApplySetThemeTopOnly(viewWnd, name);
            Log(L"SetWindowTheme(%ls) 只对视图【顶层窗口】生效", name);
        }
        Pump(400);
    }
    if (themeChanged) {
        ApplyThemeChanged(viewWnd);
        Log(L"WM_THEMECHANGED 已对视图子树广播");
        Pump(500);
    }
    if (allowDarkTree) {
        ApplyAllowDark(viewWnd, true);
        Pump(400);
    }
    if (scrollbarOnly) {
        ApplyScrollbarOnly(viewWnd, L"DarkMode_Explorer");
        Pump(400);
    }
    if (resetAfter) {
        ApplyResetTheme(viewWnd);
        Log(L"已移除子树所有主题关联（SetWindowTheme NULL）");
        Pump(500);
    }

    // 选中第一项
    IFolderView* fv = nullptr;
    bool selected = false;
    if (SUCCEEDED(view->QueryInterface(IID_IFolderView, reinterpret_cast<void**>(&fv))) && fv) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(fv->Item(0, &pidl)) && pidl) {
            hr = view->SelectItem(pidl, SVSI_SELECT | SVSI_FOCUSED | SVSI_ENSUREVISIBLE |
                                            SVSI_DESELECTOTHERS);
            selected = SUCCEEDED(hr);
            Log(L"SelectItem hr=0x%08lX", static_cast<unsigned long>(hr));
            CoTaskMemFree(pidl);
        } else {
            Log(L"Item(0) 失败（目录可能为空）");
        }
        fv->Release();
    }
    Pump(600);

    int W2 = 0, H2 = 0;
    if (!Grab(defView, &W2, &H2, &after) || W2 != W || H2 != H) {
        Log(L"第二次抓图失败/尺寸变化");
    } else {
        const Diff d = Compare(before, after, W, H);
        Log(L"size=%dx%d selected=%d changed=%d peakRow=%d peakCount=%d", W, H, selected ? 1 : 0,
            d.changed, d.peakRow, d.peakCount);
        // 高亮块 = 一整行几乎全宽变化（≥60% 宽度）且总变化量可观
        const bool highlight = d.peakCount >= W * 3 / 5 && d.changed >= 2000;
        Log(L"verdict=%ls", highlight ? L"HIGHLIGHT-PRESENT" : L"HIGHLIGHT-MISSING");
    }

    view->Release();
    eb->Destroy();
    eb->Release();
    DestroyWindow(host);
    OleUninitialize();
    if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
    return 0;
}
