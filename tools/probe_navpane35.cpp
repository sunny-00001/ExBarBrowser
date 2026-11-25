// 探针 35：主页(Home) / 图库(Gallery) 作为 NSTC 的根，效果如何？
//
// 探针 34 已确定解析方式（那些 0x800401F3 全是 swprintf 的 %s 截断 bug，用 %ls 后）：
//   图库 "::{E88865EA-...}"          SHCreateItemFromParsingName 成功，name="图库"
//   主页 "shell:::{679F85CB-...}"    SHCreateItemFromParsingName 成功（不想 shell 前缀对
//                                    图库也能用，两者都试一遍）
//
// 本探针把这两个当根 AppendRoot，看树里长出来什么：
//   A. 主页作根（预期：桌面/Downloads/文档/图片/音乐/视频 + 用户固定的项）
//   B. 图库作根
//   C. 主页 + 图库 + 此电脑 + 网络 四根并列（= explorer 左栏整体）
//
// 一例一窗，避免探针 32 那种"摸到上一组的旧树"的污染。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <commctrl.h>
#include <knownfolders.h>

#include <cstdio>
#include <string>
#include <vector>

static const GUID kCLSID_Home = {0x679f85cb, 0x0220, 0x4080,
                                 {0xb2, 0x9b, 0x55, 0x40, 0xcc, 0x05, 0xaa, 0xb6}};
static const GUID kCLSID_Gallery = {0xe88865ea, 0x0e1c, 0x4e20,
                                    {0x9a, 0xa6, 0xed, 0xcd, 0x02, 0x12, 0xc8, 0x7c}};

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

static void DumpTree(HWND tv, int maxDepth) {
    struct Frame {
        HTREEITEM item;
        int depth;
    };
    HTREEITEM root =
        reinterpret_cast<HTREEITEM>(SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    std::vector<Frame> stack{{root, 0}};
    int count = 0;
    while (!stack.empty() && count < 90) {
        Frame f = stack.back();
        stack.pop_back();
        for (HTREEITEM cur = f.item; cur && count < 90 && f.depth <= maxDepth;
             cur = reinterpret_cast<HTREEITEM>(SendMessageW(
                 tv, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(cur)))) {
            wchar_t buf[256] = L"";
            TVITEMW t{};
            t.hItem = cur;
            t.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_CHILDREN | TVIF_STATE;
            t.stateMask = TVIS_EXPANDED;
            t.pszText = buf;
            t.cchTextMax = 256;
            SendMessageW(tv, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&t));
            printf("      %*s%s (img=%d kids=%d)%s\n", f.depth * 2, "", U8(buf).c_str(), t.iImage,
                   t.cChildren, (t.state & TVIS_EXPANDED) ? " [exp]" : "");
            ++count;
            HTREEITEM child = reinterpret_cast<HTREEITEM>(
                SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
            if (child) stack.push_back({child, f.depth + 1});
        }
    }
    if (count >= 90) printf("      ...（截断）\n");
}

// 解析命名空间 CLSID：优先 "shell:::{GUID}"，失败再退 "::{GUID}"
static IShellItem* ItemFromClsid(REFGUID clsid, const wchar_t* label) {
    wchar_t g[64];
    StringFromGUID2(clsid, g, 64);
    const wchar_t* kPrefixes[] = {L"shell:::", L"::"};
    for (const wchar_t* prefix : kPrefixes) {
        wchar_t spec[160];
        // %ls 是必须的：%s 在宽字符格式串里按窄串读，会在 '{' 后遇 \0 截断
        swprintf(spec, 160, L"%ls%ls", prefix, g);
        IShellItem* it = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(spec, nullptr, IID_IShellItem,
                                                 reinterpret_cast<void**>(&it));
        if (SUCCEEDED(hr) && it) {
            PWSTR d = nullptr;
            it->GetDisplayName(SIGDN_NORMALDISPLAY, &d);
            printf("    [解析] %s -> \"%s\"  用前缀 \"%ls\"\n", U8(label).c_str(),
                   U8(d ? d : L"?").c_str(), prefix);
            if (d) CoTaskMemFree(d);
            return it;
        }
        if (it) it->Release();
    }
    printf("    [解析] %s 失败（两种前缀都不行）\n", U8(label).c_str());
    return nullptr;
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

struct RootSpec {
    IShellItem* item;
    const wchar_t* label;
};

static void RunRoots(HINSTANCE inst, const wchar_t* tag, const std::vector<RootSpec>& roots) {
    printf("\n---------- %s ----------\n", U8(tag).c_str());

    static int idx = 0;
    wchar_t cls[64];
    swprintf(cls, 64, L"Probe35Host_%d", idx++);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, cls, tag, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                80 + idx * 45, 50 + idx * 35, 360, 720, nullptr, nullptr, inst,
                                nullptr);

    INameSpaceTreeControl* tree = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                IID_INameSpaceTreeControl,
                                reinterpret_cast<void**>(&tree))) ||
        !tree) {
        printf("    创建失败\n");
        DestroyWindow(host);
        return;
    }
    RECT rc{0, 0, 360, 720};
    NSTCSTYLE style = static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                                            NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL |
                                            NSTCS_NOINFOTIP | NSTCS_TABSTOP | NSTCS_BORDER);
    tree->Initialize(host, &rc, style);

    for (const auto& r : roots) {
        if (!r.item) {
            printf("    [跳过] %s\n", U8(r.label).c_str());
            continue;
        }
        HRESULT a = tree->AppendRoot(r.item, SHCONTF_FOLDERS,
                                     static_cast<NSTCROOTSTYLE>(NSTCRS_VISIBLE | NSTCRS_EXPANDED),
                                     nullptr);
        printf("    AppendRoot(%s) hr=0x%08lX\n", U8(r.label).c_str(),
               static_cast<unsigned long>(a));
    }
    Pump(2600);

    HWND tv = FindByClass(host, L"SysTreeView32");
    if (tv) {
        DumpTree(tv, 2);
    } else {
        printf("    没有 SysTreeView32\n");
    }

    tree->Release();
    DestroyWindow(host);
    Pump(350);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HINSTANCE inst = GetModuleHandleW(nullptr);

    printf("=== 解析三个命名空间根 ===\n");
    IShellItem* home = ItemFromClsid(kCLSID_Home, L"主页/快速访问");
    IShellItem* gallery = ItemFromClsid(kCLSID_Gallery, L"图库");
    IShellItem* pc = Known(FOLDERID_ComputerFolder);
    IShellItem* net = Known(FOLDERID_NetworkFolder);

    RunRoots(inst, L"A. 主页单根", {{home, L"主页"}});
    RunRoots(inst, L"B. 图库单根", {{gallery, L"图库"}});
    RunRoots(inst, L"C. 主页+图库+此电脑+网络（explorer 左栏整体）",
             {{home, L"主页"}, {gallery, L"图库"}, {pc, L"此电脑"}, {net, L"网络"}});

    for (IShellItem* p : {home, gallery, pc, net}) {
        if (p) p->Release();
    }

    printf("\n=== 速览 ===\n");
    printf("  A 组若出现「桌面/Downloads/文档/图片/音乐/视频」= 主页根可用\n");
    printf("  C 组四项并列 = 可以按配置拼出 explorer 左栏\n");
    fflush(stdout);
    return 0;
}