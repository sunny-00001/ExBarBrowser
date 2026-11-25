// 探针 75：独立 NSTC，专门测三件事（一次问完）：
//
//   A) INameSpaceTreeControl::GetItemRect 给的是**屏幕坐标**还是**树客户坐标**？
//      -> 决定分隔线覆盖层怎么换算。
//   B) 内部 SysTreeView32 的顶层结构：是不是"1 个隐藏项 + N 个子项"？
//      -> 决定"第 i 个根"怎么定位到 HTREEITEM。
//   C) 【关键】能不能造出 explorer 那种**组间 30px 空行**？
//      依次试：TVM_SETITEMHEIGHT(30) / TVM_SETINDENT(30) / TVM_SETITEMRECT(加高某一项)
//      看后一行的 y 是否被推下去。
//
// 合规：自建窗口，不碰用户界面，不发合成输入（R21）。
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>
#include <vector>

#ifndef TVIR_BOUNDS
#define TVIR_BOUNDS 0
#endif
#ifndef TVIR_LABELBOUNDS
#define TVIR_LABELBOUNDS 2
#endif
#ifndef TVM_GETFIRSTVISIBLEITEM
#define TVM_GETFIRSTVISIBLEITEM (TV_FIRST + 20)
#endif
#ifndef TVM_SETITEMRECT
#define TVM_SETITEMRECT (TV_FIRST + 102)
#endif

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

struct FindCtx {
    const wchar_t* want;
    HWND found;
};

BOOL CALLBACK EnumChildProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    wchar_t cls[128] = L"";
    GetClassNameW(h, cls, 128);
    if (wcscmp(cls, c->want) == 0) {
        c->found = h;
        return FALSE;
    }
    return TRUE;
}

HWND FindByClass(HWND root, const wchar_t* want) {
    FindCtx ctx{want, nullptr};
    EnumChildWindows(root, EnumChildProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// 打印内部树的层级结构（深度 2，够看"隐藏根 + 子项"）
void DumpTree(HWND tv, const char* tag) {
    printf("  --- %s ---\n", tag);
    HTREEITEM top = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0);
    int ti = 0;
    while (top && ti < 4) {
        wchar_t txt[256] = L"";
        TVITEMW tvi{};
        tvi.mask = TVIF_TEXT;
        tvi.hItem = top;
        tvi.pszText = txt;
        tvi.cchTextMax = 256;
        SendMessageW(tv, TVM_GETITEMW, 0, (LPARAM)&tvi);
        RECT r{};
        r.left = TVIR_BOUNDS;
        SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)top, (LPARAM)&r);
        printf("    top[%d] text=[%s] bounds=(%ld,%ld,%ld,%ld)\n", ti, U8(txt).c_str(), r.left,
               r.top, r.right, r.bottom);
        HTREEITEM child = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)top);
        int ci = 0;
        while (child && ci < 6) {
            wchar_t t2[256] = L"";
            TVITEMW tvi2{};
            tvi2.mask = TVIF_TEXT;
            tvi2.hItem = child;
            tvi2.pszText = t2;
            tvi2.cchTextMax = 256;
            SendMessageW(tv, TVM_GETITEMW, 0, (LPARAM)&tvi2);
            RECT r2{};
            r2.left = TVIR_BOUNDS;
            SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)child, (LPARAM)&r2);
            RECT rl{};
            rl.left = TVIR_LABELBOUNDS;
            SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)child, (LPARAM)&rl);
            printf("      child[%d] text=[%s] bounds=(%ld,%ld,%ld,%ld) label=(%ld,%ld,%ld,%ld)\n",
                   ci, U8(t2).c_str(), r2.left, r2.top, r2.right, r2.bottom, rl.left, rl.top,
                   rl.right, rl.bottom);
            child = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)child);
            ++ci;
        }
        top = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)top);
        ++ti;
    }
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Probe75Host";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"Probe75Host", L"probe75", WS_OVERLAPPEDWINDOW, 0, 0, 400, 400,
                                nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(host, SW_SHOW);
    UpdateWindow(host);

    INameSpaceTreeControl* tree = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree));
    printf("CoCreateInstance hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr) || !tree) return 1;

    RECT rc{0, 0, 300, 400};
    hr = tree->Initialize(host, &rc,
                          static_cast<NSTCSTYLE>(NSTCS_HASEXPANDOS | NSTCS_ROOTHASEXPANDO |
                                                NSTCS_FULLROWSELECT | NSTCS_SINGLECLICKEXPAND |
                                                NSTCS_AUTOHSCROLL | NSTCS_NOINFOTIP |
                                                NSTCS_TABSTOP | NSTCS_BORDER));
    printf("Initialize hr=0x%08lX\n", (unsigned long)hr);

    HWND nstcWnd = nullptr;
    if (IOleWindow* ow = nullptr;
        SUCCEEDED(tree->QueryInterface(IID_IOleWindow, reinterpret_cast<void**>(&ow))) && ow) {
        ow->GetWindow(&nstcWnd);
        ow->Release();
    }
    HWND tv = FindByClass(host, L"SysTreeView32");
    printf("NSTC hwnd=0x%llX  tree hwnd=0x%llX\n", (unsigned long long)nstcWnd,
           (unsigned long long)tv);

    // 三个根
    const wchar_t* guids[] = {
        L"679F85CB-0220-4080-B29B-5540CC05AAB6",   // 主文件夹
        L"B4BFCC3A-BDB2-4C30-99C8-6C8C2E374D6A",   // 桌面
        L"2087C2F4-2CEF-4953-A81A-D26AB8C73ECB",   // 网络
    };
    std::vector<IShellItem*> items;
    for (const wchar_t* g : guids) {
        wchar_t buf[128];
        swprintf(buf, 128, L"shell:::{%ls}", g);
        IShellItem* it = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(buf, nullptr, IID_IShellItem,
                                                   reinterpret_cast<void**>(&it))) &&
            it) {
            tree->AppendRoot(it, SHCONTF_FOLDERS, static_cast<NSTCROOTSTYLE>(NSTCRS_VISIBLE),
                             nullptr);
            items.push_back(it);
        }
    }
    printf("appended %d roots\n", (int)items.size());

    RECT twr{}, tcr{}, nwr{}, ncr{}, hwr{}, hcr{};
    GetWindowRect(tv, &twr);
    GetClientRect(tv, &tcr);
    GetWindowRect(nstcWnd, &nwr);
    GetClientRect(nstcWnd, &ncr);
    GetWindowRect(host, &hwr);
    GetClientRect(host, &hcr);
    printf("\n[几何] tree win=(%ld,%ld,%ld,%ld) client=%ldx%d\n", twr.left, twr.top, twr.right,
           twr.bottom, tcr.right - tcr.left, tcr.bottom - tcr.top);
    printf("       nstc win=(%ld,%ld,%ld,%ld) client=%ldx%d\n", nwr.left, nwr.top, nwr.right,
           nwr.bottom, ncr.right - ncr.left, ncr.bottom - ncr.top);
    printf("       host win=(%ld,%ld,%ld,%ld) client=%ldx%d\n", hwr.left, hwr.top, hwr.right,
           hwr.bottom, hcr.right - hcr.left, hcr.bottom - hcr.top);

    printf("\n[A] NSTC GetItemRect vs 树客户坐标\n");
    for (size_t i = 0; i < items.size(); ++i) {
        RECT ir{};
        HRESULT h = tree->GetItemRect(items[i], &ir);
        printf("  root[%zu] hr=0x%08lX rect=(%ld,%ld,%ld,%ld)\n", i, (unsigned long)h, ir.left,
               ir.top, ir.right, ir.bottom);
    }

    printf("\n[B] 默认层级\n");
    printf("  indent=%d itemHeight=%d count=%d\n", (int)SendMessageW(tv, TVM_GETINDENT, 0, 0),
           (int)SendMessageW(tv, TVM_GETITEMHEIGHT, 0, 0), (int)SendMessageW(tv, TVM_GETCOUNT, 0, 0));
    DumpTree(tv, "default");

    printf("\n[C1] SETINDENT(30) + SETITEMHEIGHT(30)\n");
    SendMessageW(tv, TVM_SETINDENT, 30, 0);
    SendMessageW(tv, TVM_SETITEMHEIGHT, 30, 0);
    printf("  indent=%d itemHeight=%d\n", (int)SendMessageW(tv, TVM_GETINDENT, 0, 0),
           (int)SendMessageW(tv, TVM_GETITEMHEIGHT, 0, 0));
    DumpTree(tv, "after indent/height");

    printf("\n[C2] TVM_SETITEMRECT：把第 2 个 child 的 rect 加高 30px，看第 3 个是否被推下去\n");
    {
        HTREEITEM top = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0);
        HTREEITEM c0 = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)top);
        HTREEITEM c1 = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)c0);
        HTREEITEM c2 = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)c1);
        RECT r1{};
        r1.left = TVIR_BOUNDS;
        SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)c1, (LPARAM)&r1);
        RECT before2{};
        before2.left = TVIR_BOUNDS;
        SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)c2, (LPARAM)&before2);
        printf("  before: c1=(%ld,%ld,%ld,%ld) c2=(%ld,%ld,%ld,%ld)\n", r1.left, r1.top, r1.right,
               r1.bottom, before2.left, before2.top, before2.right, before2.bottom);
        RECT nr = r1;
        nr.bottom = nr.bottom + 30;
        BOOL ok = (BOOL)SendMessageW(tv, TVM_SETITEMRECT, (WPARAM)c1, (LPARAM)&nr);
        RECT after1{}, after2{};
        after1.left = TVIR_BOUNDS;
        SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)c1, (LPARAM)&after1);
        after2.left = TVIR_BOUNDS;
        SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)c2, (LPARAM)&after2);
        printf("  SETITEMRECT ok=%d -> c1=(%ld,%ld,%ld,%ld) c2=(%ld,%ld,%ld,%ld)\n", ok,
               after1.left, after1.top, after1.right, after1.bottom, after2.left, after2.top,
               after2.right, after2.bottom);
        printf("  【判读】c2.top 若从 %ld 变成 %ld，说明加高会推动后续行（可用来造空行）\n",
               before2.top, after2.top);
    }

    printf("\n[C3] 滚动后再取 rect（验证行矩形随滚动变化）\n");
    {
        HTREEITEM top = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0);
        HTREEITEM c0 = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)top);
        RECT r0{};
        r0.left = TVIR_BOUNDS;
        SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)c0, (LPARAM)&r0);
        printf("  before scroll c0.top=%ld\n", r0.top);
        SendMessageW(tv, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 1), 0);
        RECT r1{};
        r1.left = TVIR_BOUNDS;
        SendMessageW(tv, TVM_GETITEMRECT, (WPARAM)c0, (LPARAM)&r1);
        printf("  after  scroll c0.top=%ld\n", r1.top);
    }

    for (IShellItem* it : items) it->Release();
    tree->Release();
    DestroyWindow(host);
    CoUninitialize();
    return 0;
}
