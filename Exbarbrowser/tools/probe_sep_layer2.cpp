// 最小复现 2：WS_EX_LAYERED + WS_CHILD 的 CreateWindowEx 会失败（err=0）。
// 本文件验证绕法：先不带 WS_EX_LAYERED 建，再用 SetWindowLongPtr 补上，
// 然后 UpdateLayeredWindow 是否可用、像素是否真的落到屏幕上。
#include <windows.h>
#include <cstdio>

namespace {

LRESULT CALLBACK ProcA(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = ProcA;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"TestSepLayerC";
    RegisterClassW(&wc);

    HWND parent = CreateWindowExW(0, L"STATIC", L"parent", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  0, 0, 300, 300, nullptr, nullptr, inst, nullptr);
    printf("parent=%p\n", (void*)parent);

    // 1) 不带 WS_EX_LAYERED 建
    int self = 42;
    HWND w = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"TestSepLayerC", L"", WS_CHILD,
                              0, 0, 200, 100, parent, nullptr, inst, &self);
    printf("create(without LAYERED) hwnd=%p err=0x%08lX\n", (void*)w, GetLastError());
    if (!w) return 1;

    // 2) 事后补 WS_EX_LAYERED
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    SetLastError(0);
    LONG_PTR ex2 = SetWindowLongPtrW(w, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    printf("SetWindowLongPtr(+LAYERED) old=0x%llX new=0x%llX err=0x%08lX\n",
           (unsigned long long)ex, (unsigned long long)ex2, GetLastError());
    printf("  readback exstyle=0x%llX (LAYERED=%d)\n",
           (unsigned long long)GetWindowLongPtrW(w, GWL_EXSTYLE),
           (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_LAYERED) ? 1 : 0);

    // 3) UpdateLayeredWindow 能不能用
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 200;
    bi.bmiHeader.biHeight = -100;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memset(bits, 0, 200 * 100 * 4);
    // 画一条 1px 灰线在 y=50
    auto* row = reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(bits) + 50 * 200 * 4);
    for (int x = 0; x < 200; ++x) row[x] = 0xFF606060u;
    HGDIOBJ old = SelectObject(mem, dib);

    RECT wr{};
    GetWindowRect(w, &wr);
    POINT dst{wr.left, wr.top};
    POINT src{0, 0};
    SIZE size{200, 100};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SetLastError(0);
    BOOL ok = UpdateLayeredWindow(w, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    printf("UpdateLayeredWindow ok=%d err=0x%08lX\n", ok, GetLastError());

    // 4) SetLayeredWindowAttributes 能不能用（每窗口 alpha 备选）
    SetLastError(0);
    BOOL ok2 = SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
    printf("SetLayeredWindowAttributes ok=%d err=0x%08lX\n", ok2, GetLastError());

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return 0;
}
