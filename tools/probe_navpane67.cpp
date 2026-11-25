// 探针 67：**同源**测量 —— 树的像素与 TVM_GETITEMRECT 用同一套客户区坐标。
//
// 【为什么必须重做】探针 64 已经证明消息层面的差异是确定的（indent=19 时
//   根行 textLeft=19、子行 textLeft=38），但它的 BMP 抓的是**宿主窗口**
//   （含标题栏与边框），像素 x 与消息 x 差一个常量偏移，两边对不上，
//   于是"根行到底有没有展开按钮列"一直无法从像素上确认。
//
// 【本探针做法】像素只抓内部 SysTreeView32 的客户区（PrintWindow + PW_CLIENTONLY），
//   于是 BMP 的 x 就是 TVM_GETITEMRECT 的 x。再逐行（按行矩形切）扫：
//     · band0 = [0, indent)                  <- 有像素 = 存在独立按钮列
//     · 该行首个非背景像素 x                   <- 图标左缘
//     · 该行文字左缘（TVM_GETITEMRECT TRUE）    <- 消息口径
//   两个口径互相校验。
//
// 【三种方案】样式与产品同源，只改一样东西：
//   A 现状：无 ROOTHASEXPANDO（indent 用系统默认）
//   B 加 NSTCS_ROOTHASEXPANDO
//   C 加 NSTCS_ROOTHASEXPANDO + 显式 indent（看列宽能不能拉开）
//
// 合规：自建窗口 + PrintWindow 只读，不发合成输入、不动光标、不抢焦点（R21）。
#include <windows.h>
#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClsidHome[] = L"679F85CB-0220-4080-B29B-5540CC05AAB6";
constexpr wchar_t kClsidGallery[] = L"E88865EA-0E1C-4E20-9AA6-EDCD0212C87C";

// 与 src/nav_pane.cpp 的 kNstcStyle 同源。
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

// TVM_GETITEMRECT 要求 hItem 覆盖 RECT 的前 sizeof(HANDLE) 字节（不能用 (LONG)item）。
bool ItemRect(HWND tv, HTREEITEM item, bool textOnly, RECT* out) {
    if (!out) return false;
    RECT rc{};
    *reinterpret_cast<HTREEITEM*>(&rc) = item;
    if (!SendMessageW(tv, TVM_GETITEMRECT, textOnly ? TRUE : FALSE,
                      reinterpret_cast<LPARAM>(&rc))) {
        return false;
    }
    *out = rc;
    return true;
}

struct Row {
    std::wstring text;
    int depth = 0;
    RECT textRc{};
    RECT rowRc{};
};

void Collect(HWND tv, HTREEITEM start, int depth, std::vector<Row>* out) {
    for (HTREEITEM cur = start; cur;
         cur = reinterpret_cast<HTREEITEM>(SendMessageW(
             tv, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(cur)))) {
        wchar_t text[256] = L"";
        TVITEMW item{};
        item.mask = TVIF_TEXT;
        item.hItem = cur;
        item.pszText = text;
        item.cchTextMax = 256;
        SendMessageW(tv, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));

        Row row;
        row.text = text;
        row.depth = depth;
        ItemRect(tv, cur, true, &row.textRc);
        ItemRect(tv, cur, false, &row.rowRc);
        out->push_back(row);

        HTREEITEM child = reinterpret_cast<HTREEITEM>(
            SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
        if (child) Collect(tv, child, depth + 1, out);
    }
}

// 抓树控件**客户区**位图：这样 BMP 的 x 与 TVM_GETITEMRECT 的 x 同源。
bool GrabClient(HWND hwnd, std::vector<unsigned char>* out, int* pw, int* ph) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(mem, old);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    out->assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    GetDIBits(mem, bmp, 0, h, out->data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    printf("   PrintWindow(客户区) ok=%d  %dx%d\n", ok ? 1 : 0, w, h);
    *pw = w;
    *ph = h;
    return true;
}

void Run(DWORD extraStyle, int indent, const char* label, const wchar_t* bmpPath) {
    printf("\n===== %s (extraStyle=0x%lX indent=%d) =====\n", label,
           static_cast<unsigned long>(extraStyle), indent);

    HWND host = CreateWindowExW(0, L"Probe67Host", L"p67", WS_OVERLAPPEDWINDOW, 40, 40, 400, 760,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    UpdateWindow(host);

    INameSpaceTreeControl* tree = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree))) ||
        !tree) {
        printf("   <CoCreateInstance 失败>\n");
        DestroyWindow(host);
        return;
    }
    RECT rc{0, 0, 380, 700};
    tree->Initialize(host, &rc, static_cast<NSTCSTYLE>(kBaseStyle | extraStyle));

    IShellItem* roots[4] = {Clsid(kClsidHome), Clsid(kClsidGallery),
                            Known(FOLDERID_ComputerFolder), Known(FOLDERID_NetworkFolder)};
    for (IShellItem* r : roots) {
        if (!r) continue;
        tree->AppendRoot(r, SHCONTF_FOLDERS,
                         static_cast<NSTCROOTSTYLE>(NSTCRS_VISIBLE | NSTCRS_EXPANDED), nullptr);
        r->Release();
    }
    Pump(1200);

    HWND tv = FindByClass(host, L"SysTreeView32");
    if (!tv) {
        printf("   <找不到 SysTreeView32>\n");
        tree->Release();
        DestroyWindow(host);
        return;
    }
    if (indent > 0) {
        SendMessageW(tv, TVM_SETINDENT, static_cast<WPARAM>(indent), 0);
        Pump(200);
    }
    const int realIndent = static_cast<int>(SendMessageW(tv, TVM_GETINDENT, 0, 0));
    const int itemH = static_cast<int>(SendMessageW(tv, TVM_GETITEMHEIGHT, 0, 0));
    printf("   实际 indent=%d itemHeight=%d\n", realIndent, itemH);

    std::vector<Row> rows;
    HTREEITEM root = reinterpret_cast<HTREEITEM>(
        SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    if (root) Collect(tv, root, 0, &rows);

    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (!GrabClient(tv, &px, &w, &h)) {
        printf("   抓图失败\n");
        tree->Release();
        DestroyWindow(host);
        return;
    }
    auto at = [&](int x, int y) -> const unsigned char* {
        return &px[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
    };
    // 背景 = 该行最后 8 列的众数色（行尾部必然是空白）
    auto bgOf = [&](int y, unsigned char* bg) {
        int best = 0;
        for (int x = w - 8; x < w; ++x) {
            const unsigned char* c = at(x, y);
            int same = 0;
            for (int x2 = w - 8; x2 < w; ++x2) {
                const unsigned char* d = at(x2, y);
                if (abs(c[0] - d[0]) + abs(c[1] - d[1]) + abs(c[2] - d[2]) < 6) ++same;
            }
            if (same > best) {
                best = same;
                bg[0] = c[0];
                bg[1] = c[1];
                bg[2] = c[2];
            }
        }
    };

    printf("   %-4s %-6s %-5s %-9s %-9s %-9s\n", "行", "层级", "文字x", "图标x", "按钮列内?", "整行x");
    for (const Row& r : rows) {
        const int y = (r.rowRc.top + r.rowRc.bottom) / 2;
        unsigned char bg[3] = {255, 255, 255};
        if (y >= 0 && y < h) bgOf(y, bg);

        int first = -1;   // 该行首个非背景像素
        for (int x = 0; x < w && first < 0; ++x) {
            for (int yy = r.rowRc.top; yy <= r.rowRc.bottom && yy < h; ++yy) {
                if (yy < 0) continue;
                const unsigned char* c = at(x, yy);
                if (abs(c[0] - bg[0]) + abs(c[1] - bg[1]) + abs(c[2] - bg[2]) > 30) {
                    first = x;
                    break;
                }
            }
        }
        // band0 = [0, indent)：这里有像素 = 前导列里画了东西（按钮/图标）
        bool band0 = false;
        for (int x = 0; x < realIndent && x < w && !band0; ++x) {
            for (int yy = r.rowRc.top; yy <= r.rowRc.bottom && yy < h; ++yy) {
                if (yy < 0) continue;
                const unsigned char* c = at(x, yy);
                if (abs(c[0] - bg[0]) + abs(c[1] - bg[1]) + abs(c[2] - bg[2]) > 30) {
                    band0 = true;
                    break;
                }
            }
        }
        printf("   %-4s d=%-4d %-5ld %-9d %-9s %-9ld  %s\n", "item", r.depth,
               static_cast<long>(r.textRc.left), first, band0 ? "有像素" : "空",
               static_cast<long>(r.rowRc.left), U8(r.text.c_str()).c_str());
    }

    // 存图（客户区，x 与消息同源）
    FILE* f = nullptr;
    if (_wfopen_s(&f, bmpPath, L"wb") == 0 && f) {
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = static_cast<DWORD>(fh.bfOffBits + px.size());
        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(bi);
        bi.biWidth = w;
        bi.biHeight = -h;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
        printf("   已存 %ls\n", bmpPath);
    }

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
    wc.lpszClassName = L"Probe67Host";
    RegisterClassExW(&wc);

    Run(0, 0, "A 现状（无 ROOTHASEXPANDO、默认 indent）", L"tools/nav67_a.bmp");
    Run(NSTCS_ROOTHASEXPANDO, 0, "B 加 NSTCS_ROOTHASEXPANDO", L"tools/nav67_b.bmp");
    Run(NSTCS_ROOTHASEXPANDO, 26, "C ROOTHASEXPANDO + indent=26", L"tools/nav67_c.bmp");

    CoUninitialize();
    return 0;
}