// 探针 53：把运行中 Explorer 的导航窗格**截图**存成 BMP。
//
// 为什么需要截图：MSAA 的 accLocation 对 treeview 项返回的是**整行矩形**
// （27 项 x 全 = 846 = 树控件左边缘），无法判断缩进层级；而 Explorer 真实布局
// （哪些项是顶层、哪些缩进一级、分隔线在哪）必须看像素才能确定。
//
// 只读：PrintWindow 到内存 DC，不发合成输入，不改用户界面（R21 合规）。
#include <windows.h>
#include <stdio.h>

namespace {

HWND FindExplorerTree() {
    HWND explorer = FindWindowW(L"CabinetWClass", nullptr);
    if (!explorer) return nullptr;
    struct Ctx {
        HWND found = nullptr;
    } ctx;
    EnumChildWindows(explorer, [](HWND h, LPARAM lp) -> BOOL {
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

bool SaveBmp(HWND hwnd, const wchar_t* path) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    // PW_RENDERFULLCONTENT = 0x00000002：对 DWM 合成窗口也能拿到内容
    BOOL ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    if (!ok) {
        BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
    }
    SelectObject(mem, old);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;  // 负 = 自上而下
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
    printf("saved %ls  (%dx%d)\n", path, w, h);
    return true;
}

}  // namespace

int main() {
    HWND tree = FindExplorerTree();
    if (!tree) {
        printf("NO_NAV_TREE\n");
        return 1;
    }
    wchar_t title[256] = L"";
    GetWindowTextW(tree, title, 256);
    printf("tree hwnd=0x%llX title=[%ls]\n", (unsigned long long)tree, title);
    if (!SaveBmp(tree, L"tools/explorer_navpane.bmp")) {
        printf("SAVE_FAILED\n");
        return 2;
    }
    return 0;
}
