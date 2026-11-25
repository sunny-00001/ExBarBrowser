// 探针 41：图标全空的矩阵定位。
//
// 现象（探针 40 复刻产品调用序列）：树有 image list（51 张），但每个节点 iImage=-1
// -> 一个图标都画不出来（和用户截图一致）。
// 探针 39（顶层宿主 + 只挂"此电脑"一个根）却是 iImage=15/53/55 —— 正常。
//
// 变量矩阵：
//   宿主：顶层窗口 / 子窗口（产品用子窗口）
//   根：只挂"此电脑" / 挂主页+图库+此电脑+网络
//   事件：不实现 OnGetDefaultIconIndex(E_NOTIMPL) / 写 -1
// 判据：第一个根节点的 iImage（-1 = 无图标）。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <knownfolders.h>

#include <cstdio>
#include <string>
#include <vector>

static const wchar_t* kClsidHome = L"679F85CB-0220-4080-B29B-5540CC05AAB6";
static const wchar_t* kClsidGallery = L"E88865EA-0E1C-4E20-9AA6-EDCD0212C87C";

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

static int IlCount(HWND tv) {
    if (!tv) return -2;
    HIMAGELIST il =
        reinterpret_cast<HIMAGELIST>(SendMessageW(tv, TVM_GETIMAGELIST, TVSIL_NORMAL, 0));
    return il ? ImageList_GetImageCount(il) : -1;
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

static IShellItem* Clsid(const wchar_t* guid) {
    IShellItem* it = nullptr;
    std::wstring spec = L"shell:::{";
    spec += guid;
    spec += L"}";
    SHCreateItemFromParsingName(spec.c_str(), nullptr, IID_IShellItem,
                                reinterpret_cast<void**>(&it));
    return it;
}

class Events final : public INameSpaceTreeControlEvents {
public:
    LONG ref = 1;
    bool minusOne = false;
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
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE OnGetDefaultIconIndex(IShellItem*, int* d, int* o) override {
        if (!minusOne) return E_NOTIMPL;
        if (d) *d = -1;
        if (o) *o = -1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChanged(IShellItemArray*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemClick(IShellItem*, NSTCEHITTEST,
                                          NSTCECLICKTYPE) override {
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
};

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

static int FirstImage(HWND tv) {
    HTREEITEM r = reinterpret_cast<HTREEITEM>(SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    if (!r) return -999;
    TVITEMW t{};
    t.mask = TVIF_IMAGE;
    t.hItem = r;
    SendMessageW(tv, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&t));
    return t.iImage;
}

// 递归统计：多少节点有 iImage>0
static void CountImages(HWND tv, HTREEITEM item, int* total, int* withIcon) {
    for (HTREEITEM cur = item; cur;
         cur = reinterpret_cast<HTREEITEM>(SendMessageW(tv, TVM_GETNEXTITEM, TVGN_NEXT,
                                                         reinterpret_cast<LPARAM>(cur)))) {
        TVITEMW t{};
        t.mask = TVIF_IMAGE;
        t.hItem = cur;
        SendMessageW(tv, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&t));
        ++*total;
        if (t.iImage > 0) ++*withIcon;
        HTREEITEM ch = reinterpret_cast<HTREEITEM>(
            SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
        if (ch) CountImages(tv, ch, total, withIcon);
    }
}

static void Run(bool childHost, bool fourRoots, bool minusOne, HWND parent) {
    HWND host = childHost
                    ? CreateWindowExW(0, L"Probe41Host", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                      0, 0, 300, 600, parent, nullptr, GetModuleHandleW(nullptr),
                                      nullptr)
                    : CreateWindowExW(0, L"Probe41Host", L"p41", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                      100, 100, 360, 700, nullptr, nullptr, GetModuleHandleW(nullptr),
                                      nullptr);
    INameSpaceTreeControl* tree = nullptr;
    CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                     IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree));
    RECT rc{0, 0, 300, 600};
    tree->Initialize(host, &rc,
                     static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                                           NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL |
                                           NSTCS_NOINFOTIP | NSTCS_TABSTOP | NSTCS_BORDER));
    Events* ev = new Events();
    ev->minusOne = minusOne;
    DWORD cookie = 0;
    tree->TreeAdvise(ev, &cookie);

    if (fourRoots) {
        IShellItem* roots[4] = {Clsid(kClsidHome), Clsid(kClsidGallery),
                                Known(FOLDERID_ComputerFolder), Known(FOLDERID_NetworkFolder)};
        for (IShellItem* r : roots) {
            if (!r) continue;
            tree->AppendRoot(r, SHCONTF_FOLDERS,
                             static_cast<NSTCROOTSTYLE>(NSTCRS_VISIBLE | NSTCRS_EXPANDED),
                             nullptr);
            r->Release();
        }
    } else {
        IShellItem* pc = Known(FOLDERID_ComputerFolder);
        tree->AppendRoot(pc, SHCONTF_FOLDERS, NSTCRS_EXPANDED, nullptr);
        pc->Release();
    }
    Pump(1500);
    HWND tv = FindByClass(host, L"SysTreeView32");
    int total = 0, withIcon = 0;
    HTREEITEM root = reinterpret_cast<HTREEITEM>(SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    if (root) CountImages(tv, root, &total, &withIcon);
    printf("  宿主=%-6s 根=%-8s 事件=%-9s -> imagelist=%-3d 节点=%-3d 有图标=%-3d 首节点 iImage=%d\n",
           childHost ? "子窗口" : "顶层", fourRoots ? "四个" : "仅此电脑",
           minusOne ? "写-1" : "E_NOTIMPL", IlCount(tv), total, withIcon, FirstImage(tv));

    if (cookie) tree->TreeUnadvise(cookie);
    ev->Release();
    tree->Release();
    Pump(300);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"Probe41Host";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND parent = CreateWindowExW(0, L"Probe41Host", L"parent", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  100, 100, 900, 700, nullptr, nullptr, inst, nullptr);

    printf("=== 矩阵 ===\n");
    Run(false, false, false, parent);   // 探针 39 的环境（有图标）
    Run(false, false, true, parent);
    Run(false, true, false, parent);
    Run(false, true, true, parent);
    Run(true, false, false, parent);
    Run(true, false, true, parent);
    Run(true, true, false, parent);
    Run(true, true, true, parent);
    return 0;
}
