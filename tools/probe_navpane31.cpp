// 探针 31：INameSpaceTreeControl 的宽度到底能不能改？（决定方案可行性的唯一硬问题）
//
// 探针 30 的 B 组：QI IOleWindow 拿到了 NSTC 自身窗口，但 SetWindowPos 它之后
// 内部 SysTreeView32 仍是 x=0 w=300（= Initialize 时的 RECT 宽），纹丝不动。
// 用户的核心诉求恰恰是"导航窗格要能跟着拖动缩放" —— 所以这一条必须先测死。
//
// 逐条验证：
//   A. NSTC 窗口与内部树的层级关系（谁是树的直接父窗口）
//   B. SetWindowPos 各种组合：直接改树、改中间层、带 SWP_FRAMECHANGED、补发 WM_SIZE
//   C. 重新 Initialize 是否合法/可行（若可行，宽度变化时重建的代价可评估）
//   D. 副作用复核：OnGetDefaultIconIndex 返回 S_OK 但不写输出值（探针 29 的写法，
//      当时 img 全是 0）—— 确认它是否就是"图标空白"的真凶
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <commctrl.h>

#include <cstdio>
#include <string>
#include <vector>

static std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
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

static HWND FindByClass(HWND root, const wchar_t* want) {
    struct Ctx {
        const wchar_t* want;
        HWND found;
    } ctx{want, nullptr};
    EnumChildWindows(root,
                     [](HWND h, LPARAM lp) -> BOOL {
                         auto* c = reinterpret_cast<Ctx*>(lp);
                         wchar_t cls[128] = L"";
                         GetClassNameW(h, cls, 128);
                         if (wcscmp(cls, c->want) == 0) {
                             c->found = h;
                             return FALSE;
                         }
                         return TRUE;
                     },
                     reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// 打印从 host 到 tree 的窗口链（每层的类名 + 矩形）
static void DumpChain(HWND host, HWND tree, const char* tag) {
    printf("    [%s] 链：\n", tag);
    std::vector<HWND> chain;
    for (HWND w = tree; w && w != host; w = GetParent(w)) chain.push_back(w);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        wchar_t cls[128] = L"";
        GetClassNameW(*it, cls, 128);
        RECT r{};
        GetWindowRect(*it, &r);
        MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&r), 2);
        printf("      %-30s hwnd=%p x=%-4d w=%-4d\n", U8(cls).c_str(), static_cast<void*>(*it),
               static_cast<int>(r.left), static_cast<int>(r.right - r.left));
    }
}

static void ReportTree(HWND host, HWND tree, const char* tag) {
    RECT r{};
    if (tree) {
        GetWindowRect(tree, &r);
        MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&r), 2);
    }
    printf("    %-34s 树 x=%-4d w=%-4d\n", tag, static_cast<int>(r.left),
           static_cast<int>(r.right - r.left));
}

static IShellItem* Known(REFKNOWNFOLDERID id) {
    IShellItem* it = nullptr;
    if (FAILED(SHGetKnownFolderItem(id, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                                    reinterpret_cast<void**>(&it))) ||
        !it) {
        return nullptr;
    }
    return it;
}

static INameSpaceTreeControl* MakeNstc(HWND host, int w, int h, HWND* nstcWndOut) {
    INameSpaceTreeControl* tree = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree))) ||
        !tree) {
        return nullptr;
    }
    RECT rc{0, 0, w, h};
    NSTCSTYLE style = static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                                            NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL |
                                            NSTCS_NOINFOTIP | NSTCS_TABSTOP);
    tree->Initialize(host, &rc, style);
    if (nstcWndOut) {
        *nstcWndOut = nullptr;
        IOleWindow* ow = nullptr;
        if (SUCCEEDED(tree->QueryInterface(IID_IOleWindow, reinterpret_cast<void**>(&ow))) && ow) {
            ow->GetWindow(nstcWndOut);
            ow->Release();
        }
    }
    return tree;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"Probe31Host";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"Probe31Host", L"probe31", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 460, 700, nullptr, nullptr, inst, nullptr);

    IShellItem* pc = Known(FOLDERID_ComputerFolder);
    if (!pc) {
        printf("拿不到此电脑\n");
        return 1;
    }

    printf("=== A. 层级关系 ===\n");
    SetWindowPos(host, nullptr, 0, 0, 300, 640, SWP_NOZORDER | SWP_NOACTIVATE);
    HWND nstcWnd = nullptr;
    INameSpaceTreeControl* tree = MakeNstc(host, 300, 640, &nstcWnd);
    if (!tree) {
        printf("创建失败\n");
        return 1;
    }
    tree->AppendRoot(pc, SHCONTF_FOLDERS, NSTCRS_EXPANDED, nullptr);
    Pump(1800);
    HWND tv = FindByClass(host, L"SysTreeView32");
    printf("    NSTC 窗口=%p  树窗口=%p\n", static_cast<void*>(nstcWnd), static_cast<void*>(tv));
    if (tv) {
        DumpChain(host, tv, "host -> tree");
        ReportTree(host, tv, "基线");
    }

    printf("\n=== B. 各种改宽尝试 ===\n");
    if (tv && nstcWnd) {
        // B1 直接改树窗口
        SetWindowPos(tv, nullptr, 0, 0, 420, 640, SWP_NOZORDER | SWP_NOACTIVATE);
        Pump(400);
        ReportTree(host, tv, "B1 SetWindowPos(树, 420)");

        // B2 带 SWP_FRAMECHANGED
        SetWindowPos(tv, nullptr, 0, 0, 380, 640,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        Pump(400);
        ReportTree(host, tv, "B2 树 +FRAMECHANGED(380)");

        // B3 改 NSTC 窗口 + 补发 WM_SIZE
        SetWindowPos(nstcWnd, nullptr, 0, 0, 420, 640, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(nstcWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(420, 640));
        Pump(500);
        ReportTree(host, tv, "B3 NSTC(420)+WM_SIZE");

        // B4 树的父窗口改宽（如果树不是 NSTC 的直接孩子，改中间层才有意义）
        HWND parent = GetParent(tv);
        if (parent && parent != nstcWnd) {
            wchar_t cls[128] = L"";
            GetClassNameW(parent, cls, 128);
            SetWindowPos(parent, nullptr, 0, 0, 440, 640, SWP_NOZORDER | SWP_NOACTIVATE);
            Pump(400);
            char buf[160];
            snprintf(buf, sizeof(buf), "B4 中间层(%s) -> 440", U8(cls).c_str());
            ReportTree(host, tv, buf);
        } else {
            printf("    B4 跳过：树就是 NSTC 的直接孩子\n");
        }
    }

    printf("\n=== C. 重新 Initialize 是否可行 ===\n");
    {
        // 用同一对象再 Initialize 一次（宽度改 260）
        RECT rc2{0, 0, 260, 640};
        NSTCSTYLE style = static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                                                NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL);
        HRESULT hr = tree->Initialize(host, &rc2, style);
        printf("    二次 Initialize hr=0x%08lX\n", static_cast<unsigned long>(hr));
        Pump(600);
        if (tv) ReportTree(host, tv, "C 二次 Initialize(260) 后");
        // 注意：二次 Initialize 后原有根可能被清掉，行为要看清
        HWND tv2 = FindByClass(host, L"SysTreeView32");
        printf("    树窗口变为=%p（若与之前不同=控件重建了）\n", static_cast<void*>(tv2));
    }
    tree->Release();
    pc->Release();

    printf("\n=== D. 新对象的初始化宽度是否被尊重（重建方案的可行性）===\n");
    for (int w : {160, 260, 400}) {
        SetWindowPos(host, nullptr, 0, 0, w, 640, SWP_NOZORDER | SWP_NOACTIVATE);
        Pump(200);
        HWND nw = nullptr;
        INameSpaceTreeControl* t2 = MakeNstc(host, w, 640, &nw);
        if (!t2) continue;
        IShellItem* c2 = Known(FOLDERID_ComputerFolder);
        if (c2) {
            t2->AppendRoot(c2, SHCONTF_FOLDERS, NSTCRS_EXPANDED, nullptr);
            c2->Release();
        }
        Pump(1300);
        HWND tv3 = FindByClass(host, L"SysTreeView32");
        if (tv3) {
            RECT r{};
            GetWindowRect(tv3, &r);
            MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&r), 2);
            printf("    Initialize 宽=%-4d -> 树 x=%-3d w=%-4d\n", w, static_cast<int>(r.left),
                   static_cast<int>(r.right - r.left));
        }
        t2->Release();
        Pump(400);
    }

    printf("\n=== 速览 ===\n");
    printf("  B 组任一行树宽真的变了 = 可外部控制宽度\n");
    printf("  C 组 hr=0 且树被重建 = 宽度变化时可重建（代价要评估）\n");
    printf("  D 组显示'每个新对象都尊重自己的 Initialize 宽度'= 重建方案有效\n");
    fflush(stdout);
    return 0;
}