// 探针 49：验证核心假设 ——
//   「explorer 导航窗格」= 桌面命名空间（Desktop Namespace）整体展开，根节点隐藏。
//
// 依据（tools/baseline_explorer_nav.txt，MSAA 只读导出）：
//   - 第一个项是隐藏根「桌面」[INVISIBLE OFFSCREEN, EXPANDED]
//   - 其余 27 项**缩进完全相同**（x 全 = 846）-> 它们是隐藏根的**子节点**，
//     而不是「主页」的子节点。
//   - 分组空隙（y 376->436、616->676）说明是 shell 画的分组分隔线。
//
// 若假设成立，则 NSTC 用
//     AppendRoot(DesktopNamespace, SHCONTF_*, NSTCRS_HIDDEN | NSTCRS_EXPANDED)
// 就能得到与 explorer 一致的整棵左栏，且**无需手拼任何根**。
//
// 本探针做三件事：
//   (1) 用 IShellFolder::EnumObjects 枚举桌面命名空间的真实子项（多组 flags）
//   (2) 在进程内建 NSTC，以桌面命名空间为根（VISIBLE / HIDDEN 两种）dump 实际树
//   (3) 与 tools/baseline_explorer_nav.txt 的名字列表做交集/差集
//
// 合规：只读 shell 命名空间 + 自建控件，不发任何合成输入（R21）。
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

// 桌面命名空间的 CLSID = CLSID_DesktopNamespace
// {B4BFCC3A-DB2C-424C-B029-7FE99A87C641D}
constexpr GUID kDesktopNamespace = {
    0xb4bfcc3a, 0xdb2c, 0x424c, {0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41}};

IShellItem* Known(const GUID& id) {
    IShellItem* it = nullptr;
    if (FAILED(SHGetKnownFolderItem(id, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                                    reinterpret_cast<void**>(&it))) ||
        !it) {
        return nullptr;
    }
    return it;
}

// 桌面命名空间拿不到 FOLDERID，只能用 CLSID 解析。
// 三条路依次试：shell:::{GUID} / ::{GUID} / desktop->BindToObject(empty pidl)
IShellItem* DesktopNamespaceItem() {
    wchar_t buf[80];
    swprintf_s(buf, L"shell:::{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               kDesktopNamespace.Data1, kDesktopNamespace.Data2, kDesktopNamespace.Data3,
               kDesktopNamespace.Data4[0], kDesktopNamespace.Data4[1], kDesktopNamespace.Data4[2],
               kDesktopNamespace.Data4[3], kDesktopNamespace.Data4[4], kDesktopNamespace.Data4[5],
               kDesktopNamespace.Data4[6], kDesktopNamespace.Data4[7]);
    IShellItem* it = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(buf, nullptr, IID_IShellItem,
                                               reinterpret_cast<void**>(&it))) &&
        it) {
        printf("DesktopNamespace via shell::: OK\n");
        return it;
    }
    printf("DesktopNamespace via shell::: FAILED\n");
    // desktop->BindToObject(空 PIDL) = 桌面命名空间根本身
    IShellFolder* desktop = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        // 空绝对 PIDL：只含一个 2 字节的 IL end（cb=0）
        pidl = reinterpret_cast<PIDLIST_ABSOLUTE>(CoTaskMemAlloc(2));
        if (pidl) {
            pidl->mkid.cb = 0;
            HRESULT hr = desktop->BindToObject(pidl, nullptr, IID_IShellItem,
                                               reinterpret_cast<void**>(&it));
            printf("desktop->BindToObject(empty) hr=0x%08lX\n", (unsigned long)hr);
            CoTaskMemFree(pidl);
        }
        desktop->Release();
    }
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
        printf("   <拿不到 IShellFolder>\n");
        return;
    }
    IEnumIDList* eidl = nullptr;
    hr = folder->EnumObjects(nullptr, flags, &eidl);
    printf("   EnumObjects hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && eidl) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        int n = 0;
        while (n < 80 && SUCCEEDED(eidl->Next(1, &pidl, nullptr))) {
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
    wc.lpszClassName = L"Probe49Host";
    if (!clsRegistered) {
        RegisterClassExW(&wc);
        clsRegistered = true;
    }
    HWND host = CreateWindowExW(0, L"Probe49Host", L"", WS_OVERLAPPEDWINDOW, 0, 0, 420, 700,
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

    Pump(2500);

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
                int kids = 0;
                TVITEMW tv{};
                tv.mask = TVIF_CHILDREN;
                tv.hItem = r;
                SendMessageW(inner, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&tv));
                kids = tv.cChildren;
                printf("   [%2d] %-40s expanded=%d kids=%d\n", idx, U8(t.c_str()).c_str(),
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
        // UTF-8 -> UTF-16
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
    printf("[交集] %u 项\n", static_cast<unsigned>(b.size() & m.size()));
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    IShellItem* dn = DesktopNamespaceItem();
    printf("DesktopNamespace item = %p\n", reinterpret_cast<void*>(dn));

    if (dn) {
        DumpNamespaceChildren(dn, "DesktopNamespace", SHCONTF_FOLDERS | SHCONTF_NONFOLDERS);
        DumpNamespaceChildren(dn, "DesktopNamespace", SHCONTF_FOLDERS);
        DumpNamespaceChildren(dn, "DesktopNamespace", 0x10 /* SHCONTF_SHELL: 含隐藏 shell 项 */);
    }

    // NSTC：桌面命名空间根，隐藏根 + 展开
    ProbeNstc("DesktopNamespace HIDDEN|EXPANDED", dn,
              static_cast<NSTCROOTSTYLE>(NSTCRS_HIDDEN | NSTCRS_EXPANDED));
    Compare(ReadBaseline("tools/baseline_explorer_nav.txt"), g_topLevel);

    // 再来一份：根可见，方便确认根节点文本
    ProbeNstc("DesktopNamespace VISIBLE|EXPANDED", dn,
              static_cast<NSTCROOTSTYLE>(NSTCRS_VISIBLE | NSTCRS_EXPANDED));
    Compare(ReadBaseline("tools/baseline_explorer_nav.txt"), g_topLevel);

    if (dn) dn->Release();
    CoUninitialize();
    return 0;
}
