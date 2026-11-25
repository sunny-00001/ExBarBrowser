// 探针 29：INameSpaceTreeControl 独立树的正确用法
//
// 【要回答的问题】用户否决了"切割法"（导航列被 shell 定死 150px，不能独立缩放），
// 要求导航树用 shell 自己的控件、与文件列表**彻底独立**。这条路的两个已知病史：
//   1. "图标全是空白" —— 到底是真没图标，还是别的原因？
//   2. "根节点只有桌面 / 与 explorer 相差太远" —— AppendRoot 的根该怎么传？
//
// 【官方依据】MS Learn INameSpaceTreeControl::AppendRoot：
//   NSTCRS_VISIBLE  = 0x0  "The root is visible as well as the items."
//   NSTCRS_HIDDEN   = 0x1  "The root is hidden so that the children only are visible."
//   NSTCRS_EXPANDED = 0x2  "The root is expanded upon initialization."
//
// 探针 2 只传了 NSTCRS_EXPANDED，于是根「桌面」自己也被画出来 -> "桌面 -> 桌面"双层。
// 猜：加 NSTCRS_HIDDEN 就与系统 explorer 左栏一致（无桌面根，直接一堆命名空间孩子）。
//
// 本探针逐项验证：
//   A. 图标是否真的有（tree imagelist 计数、节点 iImage、SHIL 计数、导出 BMP 供肉眼比对）
//   B. 三种根样式（EXPANDED / HIDDEN|EXPANDED / 物理桌面 vs 桌面命名空间）的结构差异
//   C. 独立缩放：SetWindowPos 改宿主宽度后，树是否真的跟着变（对比切割法的 150px 死宽）
//   D. 选中事件的通道：TreeAdvise 能否拿到被点中的 IShellItem（= 树->视图联动）
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <uxtheme.h>

#include <cstdio>
#include <string>
#include <vector>

// mingw 的 shlguid.h 缺 FOLDERID_DesktopNamespace
static const GUID kFOLDERID_DesktopNamespace = {0xb4bfcc3a, 0xdb2c, 0x424c,
                                                {0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41}};
static const GUID kIID_IImageList = {0x46eb5926, 0x582e, 0x4017,
                                      {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}};

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

// 把树控件逐节点 dump 出来：文本 / iImage / 选中位 / 是否有孩子
struct DumpCtx {
    HWND tree;
    int maxDepth;
};

static std::wstring ItemText(HWND tree, HTREEITEM item) {
    wchar_t buf[512] = L"";
    TVITEMW t{};
    t.hItem = item;
    t.mask = TVIF_TEXT;
    t.pszText = buf;
    t.cchTextMax = 512;
    SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&t));
    return buf;
}

static void DumpBranch(HWND tree, HTREEITEM item, int depth, int maxDepth) {
    for (HTREEITEM cur = item; cur && depth <= maxDepth;
         cur = reinterpret_cast<HTREEITEM>(SendMessageW(
             tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(cur)))) {
        TVITEMW t{};
        t.hItem = cur;
        t.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN | TVIF_STATE;
        t.stateMask = TVIS_SELECTED | TVIS_EXPANDED;
        SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&t));
        std::wstring text = ItemText(tree, cur);
        printf("      %*s%s img=%-3d selImg=%-3d kids=%d %s%s\n", depth * 2, "",
               U8(text.c_str()).c_str(), t.iImage, t.iSelectedImage, t.cChildren,
               (t.state & TVIS_SELECTED) ? "[selected]" : "",
               (t.state & TVIS_EXPANDED) ? "[expanded]" : "");
        HTREEITEM child = reinterpret_cast<HTREEITEM>(
            SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
        if (child) DumpBranch(tree, child, depth + 1, maxDepth);
    }
}

// NSTC 事件：验证"点中节点 -> 拿到 IShellItem"这条通道（= 树->视图联动的基础）
class TreeEvents : public INameSpaceTreeControlEvents {
public:
    LONG ref = 1;
    int hits = 0;
    std::wstring lastParsing;

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

    HRESULT STDMETHODCALLTYPE OnItemClick(IShellItem* psi, NSTCEHITTEST,
                                          NSTCECLICKTYPE) override {
        ++hits;
        Capture(psi);
        printf("      [event] OnItemClick: %s\n", U8(lastParsing.c_str()).c_str());
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
    // 这条是"用户选中了某个节点"的主通道 —— 树->视图联动就靠它。
    HRESULT STDMETHODCALLTYPE OnSelectionChanged(IShellItemArray* psia) override {
        ++hits;
        if (!psia) return S_OK;
        DWORD count = 0;
        psia->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(psia->GetItemAt(i, &item)) && item) {
                Capture(item);
                item->Release();
            }
        }
        printf("      [event] OnSelectionChanged: count=%lu -> %s\n",
               static_cast<unsigned long>(count), U8(lastParsing.c_str()).c_str());
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
    HRESULT STDMETHODCALLTYPE OnGetDefaultIconIndex(IShellItem*, int*, int*) override {
        return S_OK;
    }

private:
    void Capture(IShellItem* psi) {
        if (!psi) return;
        PWSTR p = nullptr;
        if (SUCCEEDED(psi->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p)) && p) {
            lastParsing = p;
            CoTaskMemFree(p);
        }
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

static INameSpaceTreeControl* MakeTree(HWND host, int w, int h) {
    INameSpaceTreeControl* tree = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree));
    printf("    CoCreateInstance hr=0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr) || !tree) return nullptr;

    RECT rc{0, 0, w, h};
    // 与 explorer 左栏对齐的样式组合：展开箭头、整行选中、单击展开、自动横向滚动、
    // 不要 info tip、可 Tab 进入、始终显示选中态。
    NSTCSTYLE style = static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT |
                                            NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL |
                                            NSTCS_NOINFOTIP | NSTCS_TABSTOP |
                                            NSTCS_SHOWSELECTIONALWAYS | NSTCS_BORDER);
    hr = tree->Initialize(host, &rc, style);
    printf("    Initialize hr=0x%08lX\n", static_cast<unsigned long>(hr));
    return tree;
}

// 建一棵指定根样式的树，dump 结构 + 图标，可选导出 BMP
static void RunCase(HWND host, const wchar_t* tag, IShellItem* root, DWORD rootStyle,
                    int hostW, bool saveBmp) {
    printf("\n---------- %s ----------\n", U8(tag).c_str());
    SetWindowPos(host, nullptr, 0, 0, hostW, 640, SWP_NOZORDER | SWP_NOACTIVATE);

    INameSpaceTreeControl* tree = MakeTree(host, hostW, 640);
    if (!tree) return;

    TreeEvents* ev = new TreeEvents();
    DWORD cookie = 0;
    HRESULT adv = tree->TreeAdvise(ev, &cookie);
    printf("    TreeAdvise hr=0x%08lX cookie=%lu\n", static_cast<unsigned long>(adv),
           static_cast<unsigned long>(cookie));

    tree->AppendRoot(root, SHCONTF_FOLDERS, rootStyle, nullptr);
    Pump(2200);

    HWND tv = FindByClass(host, L"SysTreeView32");
    if (!tv) {
        printf("    【没有 SysTreeView32】\n");
        tree->TreeUnadvise(cookie);
        ev->Release();
        tree->Release();
        return;
    }

    // -- 图标真相 --
    HIMAGELIST il =
        reinterpret_cast<HIMAGELIST>(SendMessageW(tv, TVM_GETIMAGELIST, TVSIL_NORMAL, 0));
    printf("    tree imagelist=%p count=%d\n", static_cast<void*>(il),
           il ? ImageList_GetImageCount(il) : -1);

    // -- 结构 + 图标索引 --
    HTREEITEM r = reinterpret_cast<HTREEITEM>(SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    printf("    首层节点数（根的直接孩子）:\n");
    DumpBranch(tv, r, 0, 2);

    // -- 独立缩放：改宿主宽度，树宽是否跟随（切割法做不到这件事）--
    RECT before{};
    GetWindowRect(tv, &before);
    MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&before), 2);
    for (int w : {200, 320, 420}) {
        SetWindowPos(host, nullptr, 0, 0, w, 640, SWP_NOZORDER | SWP_NOACTIVATE);
        // NSTC 自己跟着宿主走，不归我们管 —— 这里只是验证"它确实会跟随"
        Pump(400);
        RECT now{};
        GetWindowRect(tv, &now);
        MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&now), 2);
        printf("    宿主宽=%-4d -> 树 x=%d w=%d\n", w, static_cast<int>(now.left),
               static_cast<int>(now.right - now.left));
    }
    SetWindowPos(host, nullptr, 0, 0, hostW, 640, SWP_NOZORDER | SWP_NOACTIVATE);
    Pump(300);

    if (saveBmp) {
        wchar_t path[260];
        swprintf(path, 260, L"C:\\Users\\NoteBook\\probe29_%s.bmp", tag);
        // 简单的位图导出：打印到 DIB 再存文件
        RECT wr{};
        GetWindowRect(host, &wr);
        const int w = wr.right - wr.left, h = wr.bottom - wr.top;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        PrintWindow(host, mem, PW_CLIENTONLY);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> raw(static_cast<size_t>(w) * h * 4);
        GetDIBits(mem, bmp, 0, h, raw.data(), &bi, DIB_RGB_COLORS);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        if (FILE* f = _wfopen(path, L"wb")) {
            BITMAPFILEHEADER fh{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
            fh.bfSize = static_cast<DWORD>(fh.bfOffBits + raw.size());
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
            fwrite(raw.data(), 1, raw.size(), f);
            fclose(f);
            printf("    截图: %s\n", U8(path).c_str());
        }
    }

    tree->TreeUnadvise(cookie);
    ev->Release();
    tree->Release();
    Pump(400);
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
    wc.lpszClassName = L"Probe29Host";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"Probe29Host", L"probe29", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 300, 700, nullptr, nullptr, inst, nullptr);

    IShellItem* desktop = Known(FOLDERID_Desktop);
    IShellItem* ns = Known(kFOLDERID_DesktopNamespace);
    if (desktop) {
        PWSTR p = nullptr;
        desktop->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p);
        printf("物理桌面 FOLDERID_Desktop        = %s\n", U8(p ? p : L"?").c_str());
        if (p) CoTaskMemFree(p);
    }
    if (ns) {
        PWSTR p = nullptr;
        ns->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p);
        printf("桌面命名空间 DesktopNamespace    = %s\n", U8(p ? p : L"?").c_str());
        if (p) CoTaskMemFree(p);
    }

    printf("\n=== A. 基线复现：只有 EXPANDED（探针 2 的老写法）===\n");
    if (desktop)
        RunCase(host, L"base_only_expanded", desktop, NSTCRS_EXPANDED, 300, false);

    printf("\n=== B. 加 HIDDEN：根隐藏、只显示孩子（官方说= explorer 左栏）===\n");
    if (desktop)
        RunCase(host, L"hidden_expanded", desktop,
                static_cast<DWORD>(NSTCRS_HIDDEN | NSTCRS_EXPANDED), 300, true);

    printf("\n=== C. 桌面命名空间 + HIDDEN ===\n");
    if (ns)
        RunCase(host, L"ns_hidden", ns, static_cast<DWORD>(NSTCRS_HIDDEN | NSTCRS_EXPANDED), 320,
                true);

    if (desktop) desktop->Release();
    if (ns) ns->Release();

    printf("\n=== 结论速览 ===\n");
    printf("  · 图标有没有 = 上面 tree imagelist count 与 img 索引是否非负\n");
    printf("  · 根结构对不对 = HIDDEN 组是否只有一层命名空间孩子（无'桌面'根）\n");
    printf("  · 能否独立缩放 = 宿主宽 200/320/420 时树宽是否跟着变\n");
    fflush(stdout);
    return 0;
}