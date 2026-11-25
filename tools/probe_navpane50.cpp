// 探针 50：验证核心假设 ——
//   explorer 导航窗格 = CLSID_ShellDesktop {00021400-0000-0000-C000-000000000046}
//   这个「桌面命名空间」的**子节点**集合，根节点自己隐藏。
//
// 基线（tools/baseline_explorer_nav.txt，MSAA 只读导出）的第一个隐藏项叫「桌面」，
// 且 27 个可见项缩进完全一致 -> 它们是同一个父节点的兄弟。
//
// 桌面命名空间的已知子项（Win11 理论）：
//   主文件夹(Home 679F85CB) / 图库(Gallery E88865EA) /
//   第三方命名空间扩展(TAN-个人/123云盘/Nextcloud/WPS云盘/七牛云/天翼云/小米云盘) /
//   此电脑(20D04FE0) / 库(1B3EA5DC) / 网络(F02C1A0D)
// 而「桌面/Downloads/文档/图片/音乐/视频/tools」这一组来自 **快速访问固定项**，
// 是 explorer 额外拼进去的，不在命名空间枚举里。
//
// 本探针：
//   (1) 用 SHCreateItemFromParsingName("shell:::{00021400-...}") 取该根，
//       用 IShellFolder::EnumObjects 枚举真实子项
//   (2) 在进程内建 NSTC，以该根为根（VISIBLE / HIDDEN）dump 实际树
//   (3) 与基线逐项比对
//
// 合规：只读 shell 命名空间 + 自建控件，不发合成输入（R21）。
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <knownfolders.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

constexpr DWORD kNstcStyle = NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT | NSTCS_SINGLECLICKEXPAND |
                             NSTCS_AUTOHSCROLL | NSTCS_NOINFOTIP | NSTCS_TABSTOP |
                             NSTCS_SHOWSELECTIONALWAYS;

void Pump(int ms) {
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

HWND FindTree(HWND root) {
    struct Ctx {
        HWND found = nullptr;
    } ctx;
    EnumChildWindows(root, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64] = L"";
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"SysTreeView32") == 0) {
            c->found = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

std::wstring ItemText(HWND tree, HTREEITEM it) {
    wchar_t buf[512] = L"";
    TVITEMW tv{};
    tv.mask = TVIF_TEXT;
    tv.hItem = it;
    tv.pszText = buf;
    tv.cchTextMax = 512;
    SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&tv));
    return buf;
}

IShellItem* Parse(const wchar_t* psz) {
    IShellItem* it = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(psz, nullptr, IID_IShellItem,
                                              reinterpret_cast<void**>(&it));
    printf("  parse %-52s hr=0x%08lX item=%p\n", psz, (unsigned long)hr,
           reinterpret_cast<void*>(it));
    return it;
}

// ---------- (1) IShellFolder::EnumObjects ----------
void DumpNamespaceChildren(IShellItem* item, const char* label, UINT flags) {
    printf("\n--- EnumObjects of %s (flags=0x%X) ---\n", label, flags);
    if (!item) {
        printf("   <item null>\n");
        return;
    }
    IShellFolder* folder = nullptr;
    HRESULT hr = item->BindToHandler(nullptr, BHID_SFUIObject, IID_IShellFolder,
                                     reinterpret_cast<void**>(&folder));
    if (FAILED(hr) || !folder) {
        IShellFolder* desktop = nullptr;
        if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHGetIDListFromObject(item, &pidl)) && pidl) {
                hr = desktop->BindToObject(pidl, nullptr, IID_IShellFolder,
                                           reinterpret_cast<void**>(&folder));
                ILFree(pidl);
            }
            desktop->Release();
        }
    }
    if (!folder) {
        printf("   <拿不到 IShellFolder hr=0x%08lX>\n", (unsigned long)hr);
        return;
    }
    IEnumIDList* eidl = nullptr;
    hr = folder->EnumObjects(nullptr, flags, &eidl);
    printf("   EnumObjects hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && eidl) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        int n = 0;
        while (n < 60 && SUCCEEDED(eidl->Next(1, &pidl, nullptr))) {
            wchar_t name[256] = L"";
            PWSTR pname = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &pname)) && pname) {
                wcsncpy_s(name, pname, _TRUNCATE);
                CoTaskMemFree(pname);
            }
            printf("   [%2d] %s\n", n, U8(name).c_str());
            ++n;
            ILFree(pidl);
            pidl = nullptr;
        }
        eidl->Release();
    }
    folder->Release();
}

// ---------- (2) NSTC 侧 ----------
std::vector<std::wstring> g_topLevel;

void ProbeNstc(const char* label, IShellItem* item, NSTCROOTSTYLE style) {
    printf("\n===== NSTC ROOT: %s style=0x%X =====\n", label, style);
    if (!item) {
        printf("   <item null>\n");
        return;
    }
    static bool clsRegistered = false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Probe50Host";
    if (!clsRegistered) {
        RegisterClassExW(&wc);
        clsRegistered = true;
    }
    HWND host = CreateWindowExW(0, L"Probe50Host", L"", WS_OVERLAPPEDWINDOW, 0, 0, 420, 700,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!host) {
        printf("   <CreateWindowEx failed %lu>\n", GetLastError());
        return;
    }
    ShowWindow(host, SW_SHOW);
    UpdateWindow(host);

    INameSpaceTreeControl* tree = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree));
    if (FAILED(hr) || !tree) {
        printf("   <CoCreateInstance 0x%08lX>\n", (unsigned long)hr);
        DestroyWindow(host);
        return;
    }
    RECT rc{0, 0, 400, 660};
    tree->Initialize(host, &rc, static_cast<NSTCSTYLE>(kNstcStyle));
    hr = tree->AppendRoot(item, static_cast<SHCONTF>(SHCONTF_FOLDERS | SHCONTF_NONFOLDERS),
                          style, nullptr);
    printf("   AppendRoot hr=0x%08lX\n", (unsigned long)hr);

    Pump(3000);

    IOleWindow* oleWnd = nullptr;
    if (SUCCEEDED(tree->QueryInterface(IID_IOleWindow, reinterpret_cast<void**>(&oleWnd))) &&
        oleWnd) {
        HWND nstcWnd = nullptr;
        oleWnd->GetWindow(&nstcWnd);
        HWND inner = nstcWnd ? FindTree(nstcWnd) : nullptr;
        if (!inner) inner = FindTree(host);
        if (inner) {
            g_topLevel.clear();
            HTREEITEM root = reinterpret_cast<HTREEITEM>(
                SendMessageW(inner, TVM_GETNEXTITEM, TVGN_ROOT, 0));
            int idx = 0;
            for (HTREEITEM r = root; r;
                 r = reinterpret_cast<HTREEITEM>(
                     SendMessageW(inner, TVM_GETNEXTITEM, TVGN_NEXT,
                                  reinterpret_cast<LPARAM>(r)))) {
                std::wstring t = ItemText(inner, r);
                bool exp = (SendMessageW(inner, TVM_GETITEMSTATE, reinterpret_cast<WPARAM>(r),
                                          TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
                TVITEMW tv{};
                tv.mask = TVIF_CHILDREN;
                tv.hItem = r;
                SendMessageW(inner, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&tv));
                printf("   [%2d] %-44s expanded=%d kids=%d\n", idx, U8(t.c_str()).c_str(),
                       exp ? 1 : 0, tv.cChildren);
                g_topLevel.push_back(t);
                ++idx;
            }
        } else {
            printf("   <找不到内部 SysTreeView32>\n");
        }
        oleWnd->Release();
    }
    tree->Release();
    DestroyWindow(host);
    Pump(100);
}

// ---------- (3) 与基线比对 ----------
std::vector<std::wstring> ReadBaseline(const char* path) {
    std::vector<std::wstring> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return out;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("- ", 0) != 0) continue;
        size_t lb = s.find('[');
        if (lb == std::string::npos) continue;
        std::string name = s.substr(2, lb - 2);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
        if (n <= 0) continue;
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, w.data(), n);
        w.resize(static_cast<size_t>(n - 1));
        out.push_back(w);
    }
    fclose(f);
    return out;
}

void Compare(const std::vector<std::wstring>& baseline,
             const std::vector<std::wstring>& mine) {
    printf("\n===== 与 explorer 基线比对 =====\n");
    std::set<std::wstring> b(mine.begin(), mine.end());
    std::set<std::wstring> m(baseline.begin(), baseline.end());
    printf("[基线有、我方缺]\n");
    int miss = 0;
    for (const auto& x : baseline) {
        if (!m.count(x)) {
            printf("   - %s\n", U8(x.c_str()).c_str());
            ++miss;
        }
    }
    if (!miss) printf("   (无)\n");
    printf("[我方多出来的]\n");
    int extra = 0;
    for (const auto& x : mine) {
        if (!m.count(x)) {
            printf("   + %s\n", U8(x.c_str()).c_str());
            ++extra;
        }
    }
    if (!extra) printf("   (无)\n");
    printf("[基线 %u 项 / 我方 %u 项 / 交集 %u 项]\n", static_cast<unsigned>(baseline.size()),
           static_cast<unsigned>(mine.size()), static_cast<unsigned>(b.size() & m.size()));
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    printf("=== 解析桌面命名空间根 ===\n");
    IShellItem* shellDesktop = Parse(L"shell:::{00021400-0000-0000-C000-000000000046}");
    IShellItem* shellDesktop2 = Parse(L"::{00021400-0000-0000-C000-000000000046}");
    IShellItem* root = shellDesktop ? shellDesktop : shellDesktop2;

    if (root) {
        DumpNamespaceChildren(root, "ShellDesktop", SHCONTF_FOLDERS | SHCONTF_NONFOLDERS);
        DumpNamespaceChildren(root, "ShellDesktop", SHCONTF_FOLDERS);
    }

    ProbeNstc("ShellDesktop HIDDEN|EXPANDED", root,
              static_cast<NSTCROOTSTYLE>(NSTCRS_HIDDEN | NSTCRS_EXPANDED));
    Compare(ReadBaseline("tools/baseline_explorer_nav.txt"), g_topLevel);

    ProbeNstc("ShellDesktop VISIBLE|EXPANDED", root,
              static_cast<NSTCROOTSTYLE>(NSTCRS_VISIBLE | NSTCRS_EXPANDED));
    Compare(ReadBaseline("tools/baseline_explorer_nav.txt"), g_topLevel);

    if (shellDesktop) shellDesktop->Release();
    if (shellDesktop2) shellDesktop2->Release();
    CoUninitialize();
    return 0;
}
