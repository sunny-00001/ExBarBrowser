// 探针 64：导航树"根节点与子节点不对齐"的成因定位。
//
// 【现象（产品截图实测像素，tmp_nav_shot.png）】
//   根行（主页/图库/此电脑/网络）  图标 x=10..29，文字 x=35 起，左侧**无展开按钮列**
//   子行（桌面/Downloads/盘符…）   图标 x=33..52，文字 x=58 起
//   盘符行在 x=16..21 有展开按钮（+/-）
//   -> 根行比子行左移 23px（正好一个缩进宽度），看起来"根自己另起一列"。
//
// 【假设】NSTC 的根默认不占展开按钮列；NSTCS_ROOTHASEXPANDO (0x40) 让根也占这一列。
//   开了之后根行图标应与子行同 x，展开按钮独占一列（缩进 0 的那一列）。
//
// 【判据】TVM_GETITEMRECT(textOnly=TRUE) 的 left。注意 TVM_GETITEMRECT 的传法：
//   hItem 必须塞在 RECT.left 里（comctl32 的历史约定），不能传 lParam=item。
//   期望 B 组：四个根行的 textLeft == 子行 textLeft。
// 另外各存一张 BMP 供肉眼比对（PrintWindow 只读，被遮挡也能抓）。
//
// 合规：只读 + 自建窗口，不发任何合成输入、不动光标、不抢焦点（R21）。
#include <windows.h>
#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>

namespace {

constexpr wchar_t kClsidHome[] = L"679F85CB-0220-4080-B29B-5540CC05AAB6";
constexpr wchar_t kClsidGallery[] = L"E88865EA-0E1C-4E20-9AA6-EDCD0212C87C";

// 与 src/nav_pane.cpp 的 kNstcStyle 完全一致 —— 比对必须同源，否则结论不可移植。
constexpr DWORD kBaseStyle = NSTCS_HASEXPANDOS | NSTCS_FULLROWSELECT | NSTCS_SINGLECLICKEXPAND |
                             NSTCS_AUTOHSCROLL | NSTCS_NOINFOTIP | NSTCS_TABSTOP | NSTCS_BORDER;

std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

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

HWND FindByClass(HWND root, const wchar_t* want) {
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

IShellItem* Clsid(const wchar_t* guid) {
    std::wstring spec = L"shell:::{" + std::wstring(guid) + L"}";
    IShellItem* it = nullptr;
    SHCreateItemFromParsingName(spec.c_str(), nullptr, IID_IShellItem,
                                reinterpret_cast<void**>(&it));
    return it;
}

IShellItem* Known(REFKNOWNFOLDERID id) {
    IShellItem* it = nullptr;
    SHGetKnownFolderItem(id, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                         reinterpret_cast<void**>(&it));
    return it;
}

// TVM_GETITEMRECT 的唯一正确传法：hItem 覆盖 RECT 的**前 sizeof(HANDLE) 字节**。
// 不能写成 rc.left = (LONG)item —— 64 位句柄会被截成 32 位，消息直接失败
// （这正是本探针第一版所有 textLeft 都读到 0 的原因）。
bool ItemRect(HWND tv, HTREEITEM item, bool textOnly, RECT* out) {
    if (!out) return false;
    RECT rc{};
    *reinterpret_cast<HTREEITEM*>(&rc) = item;
    LRESULT ok = SendMessageW(tv, TVM_GETITEMRECT, textOnly ? TRUE : FALSE,
                              reinterpret_cast<LPARAM>(&rc));
    if (!ok) return false;
    *out = rc;
    return true;
}

// 递归 dump：文本 / 文字矩形 left（= 文字真实起点）/ 整行 left / iImage / 是否带子项
void DumpItems(HWND tv, HTREEITEM start, int depth) {
    for (HTREEITEM cur = start; cur;
         cur = reinterpret_cast<HTREEITEM>(SendMessageW(
             tv, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(cur)))) {
        wchar_t text[256] = L"";
        TVITEMW item{};
        item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_CHILDREN;
        item.hItem = cur;
        item.pszText = text;
        item.cchTextMax = 256;
        SendMessageW(tv, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));

        RECT textRc{}, rowRc{};
        ItemRect(tv, cur, true, &textRc);
        ItemRect(tv, cur, false, &rowRc);
        printf("%*s[%-16s] d=%d textLeft=%4ld rowLeft=%4ld img=%-3d kids=%d\n", depth * 2, "",
               U8(text).c_str(), depth, static_cast<long>(textRc.left),
               static_cast<long>(rowRc.left), item.iImage, item.cChildren);

        HTREEITEM child = reinterpret_cast<HTREEITEM>(
            SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
        if (child) DumpItems(tv, child, depth + 1);
    }
}

bool SaveBmp(HWND hwnd, const wchar_t* path) {
    RECT wrc{};
    if (!GetWindowRect(hwnd, &wrc)) return false;
    const int w = wrc.right - wrc.left;
    const int h = wrc.bottom - wrc.top;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!PrintWindow(hwnd, mem, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */)) {
        BitBlt(mem, 0, 0, w, h, screen, wrc.left, wrc.top, SRCCOPY);
    }
    SelectObject(mem, old);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    const size_t stride = static_cast<size_t>(w) * 4;
    const size_t pixels = stride * static_cast<size_t>(h);
    unsigned char* buf = static_cast<unsigned char*>(malloc(pixels));
    if (!buf) {
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    GetDIBits(mem, bmp, 0, h, buf, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = static_cast<DWORD>(fh.bfOffBits + pixels);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
        free(buf);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(buf, 1, pixels, f);
    fclose(f);

    free(buf);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    printf("   saved %ls (%dx%d)\n", path, w, h);
    return true;
}

void Run(DWORD extraStyle, int indent, const char* label, const wchar_t* bmpPath) {
    printf("\n===== %s (extraStyle=0x%lX indent=%d) =====\n", label,
           static_cast<unsigned long>(extraStyle), indent);

    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, L"Probe64Host", L"p64", WS_OVERLAPPEDWINDOW,
                                60, 60, 340, 700, nullptr, nullptr, GetModuleHandleW(nullptr),
                                nullptr);
    if (!host) {
        printf("   <host 创建失败 %lu>\n", GetLastError());
        return;
    }
    ShowWindow(host, SW_SHOWNOACTIVATE);
    UpdateWindow(host);

    INameSpaceTreeControl* tree = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree));
    if (FAILED(hr) || !tree) {
        printf("   <CoCreateInstance 0x%08lX>\n", static_cast<unsigned long>(hr));
        DestroyWindow(host);
        return;
    }
    RECT rc{0, 0, 320, 660};
    hr = tree->Initialize(host, &rc, static_cast<NSTCSTYLE>(kBaseStyle | extraStyle));
    printf("   Initialize hr=0x%08lX\n", static_cast<unsigned long>(hr));

    IShellItem* roots[4] = {Clsid(kClsidHome), Clsid(kClsidGallery),
                            Known(FOLDERID_ComputerFolder), Known(FOLDERID_NetworkFolder)};
    for (IShellItem* r : roots) {
        if (!r) continue;
        tree->AppendRoot(r, SHCONTF_FOLDERS,
                         static_cast<NSTCROOTSTYLE>(NSTCRS_VISIBLE | NSTCRS_EXPANDED), nullptr);
        r->Release();
    }

    Pump(1500);

    HWND tv = FindByClass(host, L"SysTreeView32");
    if (!tv) {
        printf("   <找不到内部 SysTreeView32>\n");
    } else {
        // 缩进宽度也只能在本进程内改（跨进程 TVM_SETITEM 不封送 tree 句柄）。
        SendMessageW(tv, TVM_SETINDENT, static_cast<WPARAM>(indent), 0);
        Pump(200);
        HTREEITEM root = reinterpret_cast<HTREEITEM>(
            SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
        if (root) DumpItems(tv, root, 0);
    }
    SaveBmp(host, bmpPath);

    tree->Release();
    DestroyWindow(host);
    Pump(150);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Probe64Host";
    RegisterClassExW(&wc);

    Run(0, 19, "A 现状：无 ROOTHASEXPANDO / indent 默认 19", L"tools/nav64_a.bmp");
    Run(NSTCS_ROOTHASEXPANDO, 19, "B 只加 NSTCS_ROOTHASEXPANDO",
        L"tools/nav64_b.bmp");
    Run(NSTCS_ROOTHASEXPANDO, 0, "C ROOTHASEXPANDO + indent=0（按钮列压成 0）",
        L"tools/nav64_c.bmp");
    Run(0, 0, "D 无 ROOTHASEXPANDO + indent=0", L"tools/nav64_d.bmp");

    CoUninitialize();
    return 0;
}