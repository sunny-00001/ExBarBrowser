// 探针 30：INameSpaceTreeControl 三个未解问题的定向验证
//
// 探针 29 的结论：
//   · 节点 img 全是 0（imagelist 里明明有 51 张图）—— "图标空白"的真凶在事件实现
//   · 树宽恒 298，不随宿主变 —— Initialize 之后再无 SetRect，只能靠 IOleWindow 拿 hWnd
//   · 根的直接孩子包含一堆桌面杂物（Roaming/Local/VFS/OneSync…），原生窗格是过滤过的
//
// 本探针逐条验证：
//   A. 图标三路对照：
//        A1 完全不挂事件（TreeAdvise 都不调）
//        A2 挂事件但 OnGetDefaultIconIndex 返回 E_NOTIMPL
//        A3 挂事件且 OnGetDefaultIconIndex 写 *piDefaultIcon = -1 / *piOpenIcon = -1
//        A4 挂事件且返回 0（探针 29 的写法，作为负对照）
//   B. IOleWindow 缩放：QI 出 IOleWindow 拿 NSTC 自己的 hWnd，SetWindowPos 后树是否跟随
//   C. IShellItemFilter 过滤：只放行指定几个已知文件夹，模拟 explorer 左栏的精简根
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <commctrl.h>
#include <ole2.h>

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

// 图标策略（决定 A1~A4 走哪条）
enum class IconMode { NoAdvise, NotImplemented, MinusOne, Zero };

class TreeEvents : public INameSpaceTreeControlEvents {
public:
    LONG ref = 1;
    IconMode mode;

    explicit TreeEvents(IconMode m) : mode(m) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_INameSpaceTreeControlEvents) {
            *ppv = static_cast<INameSpaceTreeControlEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { return (ULONG)InterlockedDecrement(&ref); }

    HRESULT STDMETHODCALLTYPE OnItemClick(IShellItem*, NSTCEHITTEST, NSTCECLICKTYPE) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyItemCommit(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemStateChanging(IShellItem*, NSTCITEMSTATE,
                                                   NSTCITEMSTATE) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnItemStateChanged(IShellItem*, NSTCITEMSTATE,
                                                  NSTCITEMSTATE) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChanged(IShellItemArray*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnKeyboardInput(UINT, WPARAM, LPARAM) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnBeforeExpand(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnAfterExpand(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnBeginLabelEdit(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndLabelEdit(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGetToolTip(IShellItem*, LPWSTR, int) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnBeforeItemDelete(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemAdded(IShellItem*, WINBOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemDeleted(IShellItem*, WINBOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnBeforeContextMenu(IShellItem*, REFIID, void**) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnAfterContextMenu(IShellItem*, IContextMenu*, REFIID,
                                                  void**) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnBeforeStateImageChange(IShellItem*) override { return S_OK; }

    // ★ 本探针的主角
    HRESULT STDMETHODCALLTYPE OnGetDefaultIconIndex(IShellItem*, int* piDefaultIcon,
                                                     int* piOpenIcon) override {
        switch (mode) {
            case IconMode::NotImplemented:
                return E_NOTIMPL;
            case IconMode::MinusOne:
                if (piDefaultIcon) *piDefaultIcon = -1;
                if (piOpenIcon) *piOpenIcon = -1;
                return S_OK;
            case IconMode::Zero:
                if (piDefaultIcon) *piDefaultIcon = 0;
                if (piOpenIcon) *piOpenIcon = 0;
                return S_OK;
            default:
                return E_NOTIMPL;
        }
    }
};

// 过滤：只放行"名字在白名单里"的桌面孩子（模拟 explorer 左栏的精简根）
class NameFilter : public IShellItemFilter {
public:
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellItemFilter) {
            *ppv = static_cast<IShellItemFilter*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { return (ULONG)InterlockedDecrement(&ref); }

    HRESULT STDMETHODCALLTYPE IncludeItem(IShellItem* psi) override {
        if (!psi) return S_FALSE;
        PWSTR d = nullptr;
        bool keep = false;
        if (SUCCEEDED(psi->GetDisplayName(SIGDN_NORMALDISPLAY, &d)) && d) {
            static const wchar_t* const kWanted[] = {L"此电脑", L"网络", L"库", L"文档",
                                                     L"Downloads", L"桌面", L"图片",
                                                     L"音乐", L"视频"};
            for (const wchar_t* w : kWanted) {
                if (wcscmp(d, w) == 0) {
                    keep = true;
                    break;
                }
            }
            CoTaskMemFree(d);
        }
        return keep ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE GetEnumFlagsForItem(IShellItem*, SHCONTF* pflags) override {
        if (pflags) *pflags = SHCONTF_FOLDERS;
        return S_OK;
    }
};

static IShellItem* Known(REFKNOWNFOLDERID id) {
    IShellItem* it = nullptr;
    if (FAILED(SHGetKnownFolderItem(id, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                                    reinterpret_cast<void**>(&it))) ||
        !it) {
        return nullptr;
    }
    return it;
}

// 统计树里 iImage == 0 的节点数与总数 —— "图标空白"的量化判据
struct IconStat {
    int total = 0;
    int zero = 0;
    int distinctMax = -1;
};

static void CountIcons(HWND tree, HTREEITEM item, IconStat* st) {
    for (HTREEITEM cur = item; cur;
         cur = reinterpret_cast<HTREEITEM>(SendMessageW(
             tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(cur)))) {
        TVITEMW t{};
        t.hItem = cur;
        t.mask = TVIF_IMAGE;
        SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&t));
        ++st->total;
        if (t.iImage == 0) ++st->zero;
        if (t.iImage > st->distinctMax) st->distinctMax = t.iImage;
        HTREEITEM child = reinterpret_cast<HTREEITEM>(
            SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
        if (child) CountIcons(tree, child, st);
    }
}

static void RunIconCase(HWND host, const wchar_t* tag, IconMode mode, IShellItem* root) {
    printf("\n---------- A: %s ----------\n", U8(tag).c_str());
    SetWindowPos(host, nullptr, 0, 0, 300, 640, SWP_NOZORDER | SWP_NOACTIVATE);

    INameSpaceTreeControl* tree = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree));
    if (FAILED(hr) || !tree) {
        printf("    CoCreateInstance 失败 0x%08lX\n", static_cast<unsigned long>(hr));
        return;
    }
    RECT rc{0, 0, 300, 640};
    NSTCSTYLE style = static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                                            NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL |
                                            NSTCS_NOINFOTIP | NSTCS_TABSTOP);
    tree->Initialize(host, &rc, style);

    TreeEvents* ev = nullptr;
    DWORD cookie = 0;
    if (mode != IconMode::NoAdvise) {
        ev = new TreeEvents(mode);
        HRESULT a = tree->TreeAdvise(ev, &cookie);
        printf("    TreeAdvise hr=0x%08lX\n", static_cast<unsigned long>(a));
    } else {
        printf("    （不挂事件）\n");
    }

    tree->AppendRoot(root, SHCONTF_FOLDERS, NSTCRS_EXPANDED, nullptr);
    Pump(2000);

    HWND tv = FindByClass(host, L"SysTreeView32");
    if (!tv) {
        printf("    没有 SysTreeView32\n");
        if (ev) {
            if (cookie) tree->TreeUnadvise(cookie);
            ev->Release();
        }
        tree->Release();
        return;
    }
    HIMAGELIST il =
        reinterpret_cast<HIMAGELIST>(SendMessageW(tv, TVM_GETIMAGELIST, TVSIL_NORMAL, 0));
    IconStat st;
    HTREEITEM r = reinterpret_cast<HTREEITEM>(SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    CountIcons(tv, r, &st);
    printf("    imagelist=%p count=%d | 节点总数=%d 其中 img==0 的=%d | iImage 最大=%d\n",
           static_cast<void*>(il), il ? ImageList_GetImageCount(il) : -1, st.total, st.zero,
           st.distinctMax);
    printf("    -> 判定：%s\n",
           (st.total > 0 && st.zero == st.total) ? "【全部空白】" : "【有图标】");

    if (ev) {
        if (cookie) tree->TreeUnadvise(cookie);
        ev->Release();
    }
    tree->Release();
    Pump(400);
}

// B: IOleWindow 拿 NSTC 自己的窗口，验证能否被外部改尺寸
static void RunResizeCase(HWND host) {
    printf("\n---------- B: IOleWindow 缩放 ----------\n");
    SetWindowPos(host, nullptr, 0, 0, 150, 640, SWP_NOZORDER | SWP_NOACTIVATE);

    INameSpaceTreeControl* tree = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                IID_INameSpaceTreeControl,
                                reinterpret_cast<void**>(&tree))) ||
        !tree) {
        printf("    创建失败\n");
        return;
    }
    RECT rc{0, 0, 150, 640};
    NSTCSTYLE style = static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                                            NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL);
    tree->Initialize(host, &rc, style);

    // 官方途径：QI IOleWindow 拿控件自己的 hWnd（fafalone 的 ExplorerHost 就是这么做的）
    IOleWindow* ow = nullptr;
    HRESULT hr = tree->QueryInterface(IID_IOleWindow, reinterpret_cast<void**>(&ow));
    printf("    QI IOleWindow hr=0x%08lX ptr=%p\n", static_cast<unsigned long>(hr),
           static_cast<void*>(ow));
    HWND nstcWnd = nullptr;
    if (SUCCEEDED(hr) && ow) {
        ow->GetWindow(&nstcWnd);
        printf("    NSTC 自身窗口=%p\n", static_cast<void*>(nstcWnd));
        ow->Release();
    }

    IShellItem* pc = Known(FOLDERID_ComputerFolder);
    if (pc) {
        tree->AppendRoot(pc, SHCONTF_FOLDERS, NSTCRS_EXPANDED, nullptr);
        pc->Release();
    }
    Pump(1600);

    HWND tv = FindByClass(host, L"SysTreeView32");
    auto show = [&](const char* tag) {
        RECT tr{};
        if (tv) {
            GetWindowRect(tv, &tr);
            MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&tr), 2);
        }
        printf("    %-28s 树 x=%-3d w=%-4d\n", tag, static_cast<int>(tr.left),
               static_cast<int>(tr.right - tr.left));
    };
    show("初始（宿主 150）");

    // 关键：把 NSTC 自己的窗口拉宽到 320，树会不会跟着变？
    if (nstcWnd) {
        for (int w : {320, 420, 200}) {
            SetWindowPos(nstcWnd, nullptr, 0, 0, w, 640, SWP_NOZORDER | SWP_NOACTIVATE);
            Pump(450);
            char buf[64];
            snprintf(buf, sizeof(buf), "NSTC 窗口拉到 %d", w);
            show(buf);
        }
    } else {
        printf("    没拿到 NSTC 窗口，跳过缩放测试\n");
    }
    tree->Release();
    Pump(300);
}

// C: 过滤器能不能把根精简成 explorer 那样
static void RunFilterCase(HWND host) {
    printf("\n---------- C: IShellItemFilter 精简根 ----------\n");
    SetWindowPos(host, nullptr, 0, 0, 300, 640, SWP_NOZORDER | SWP_NOACTIVATE);

    INameSpaceTreeControl* tree = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                IID_INameSpaceTreeControl,
                                reinterpret_cast<void**>(&tree))) ||
        !tree) {
        printf("    创建失败\n");
        return;
    }
    RECT rc{0, 0, 300, 640};
    NSTCSTYLE style = static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                                            NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL);
    tree->Initialize(host, &rc, style);

    NameFilter* filter = new NameFilter();
    IShellItem* desktop = Known(FOLDERID_Desktop);
    if (desktop) {
        tree->AppendRoot(desktop, SHCONTF_FOLDERS, NSTCRS_EXPANDED, filter);
        desktop->Release();
    }
    Pump(2000);

    HWND tv = FindByClass(host, L"SysTreeView32");
    if (tv) {
        HTREEITEM r = reinterpret_cast<HTREEITEM>(SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
        printf("    过滤后的树：\n");
        std::vector<std::pair<HTREEITEM, int>> stack{{r, 0}};
        int count = 0;
        while (!stack.empty() && count < 60) {
            auto [item, depth] = stack.back();
            stack.pop_back();
            for (HTREEITEM cur = item; cur && count < 60;
                 cur = reinterpret_cast<HTREEITEM>(SendMessageW(
                     tv, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(cur)))) {
                wchar_t buf[256] = L"";
                TVITEMW t{};
                t.hItem = cur;
                t.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_CHILDREN;
                t.pszText = buf;
                t.cchTextMax = 256;
                SendMessageW(tv, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&t));
                printf("      %*s%s (img=%d kids=%d)\n", depth * 2, "", U8(buf).c_str(), t.iImage,
                       t.cChildren);
                ++count;
                HTREEITEM child = reinterpret_cast<HTREEITEM>(SendMessageW(
                    tv, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
                if (child) stack.push_back({child, depth + 1});
            }
        }
    }
    filter->Release();
    tree->Release();
    Pump(300);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"Probe30Host";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"Probe30Host", L"probe30", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 360, 700, nullptr, nullptr, inst, nullptr);

    IShellItem* pc = Known(FOLDERID_ComputerFolder);
    if (!pc) {
        printf("拿不到此电脑，退出\n");
        return 1;
    }

    RunIconCase(host, L"A1 不挂事件", IconMode::NoAdvise, pc);
    RunIconCase(host, L"A2 OnGetDefaultIconIndex=E_NOTIMPL", IconMode::NotImplemented, pc);
    RunIconCase(host, L"A3 OnGetDefaultIconIndex 写 -1", IconMode::MinusOne, pc);
    RunIconCase(host, L"A4 OnGetDefaultIconIndex 写 0（负对照）", IconMode::Zero, pc);
    pc->Release();

    RunResizeCase(host);
    RunFilterCase(host);

    printf("\n=== 速览 ===\n");
    printf("  A 组：哪一行显示【有图标】，就用那种 OnGetDefaultIconIndex 写法\n");
    printf("  B 组：NSTC 窗口拉宽后树宽是否跟随 = 能否独立缩放\n");
    printf("  C 组：过滤后根是否只剩指定几项\n");
    fflush(stdout);
    return 0;
}