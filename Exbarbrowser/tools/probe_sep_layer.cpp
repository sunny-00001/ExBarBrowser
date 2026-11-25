// 最小复现：为什么 WS_EX_LAYERED|WS_CHILD 的子窗口 CreateWindowEx 会失败？
// 逐项对比：带/不带 WS_EX_LAYERED、注册类用/不用本模块句柄。
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

LRESULT CALLBACK ProcB(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    printf("inst=%p\n", (void*)inst);

    WNDCLASSW wc{};
    wc.lpfnWndProc = ProcA;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"TestSepLayerA";
    if (!RegisterClassW(&wc)) { printf("RegisterClassW A failed 0x%08lX\n", GetLastError()); }
    else printf("RegisterClassW A ok\n");

    WNDCLASSW wc2 = wc;
    wc2.lpfnWndProc = ProcB;
    wc2.lpszClassName = L"TestSepLayerB";
    RegisterClassW(&wc2);

    HWND parent = CreateWindowExW(0, L"STATIC", L"parent", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  0, 0, 300, 300, nullptr, nullptr, inst, nullptr);
    printf("parent=%p err=0x%08lX\n", (void*)parent, GetLastError());

    struct Case { const wchar_t* cls; DWORD ex; const wchar_t* tag; };
    const Case cases[] = {
        {L"TestSepLayerA", WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"LAYERED+NOACT+TOOL"},
        {L"TestSepLayerA", WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"NOACT+TOOL"},
        {L"TestSepLayerA", 0, L"PLAIN"},
        {L"TestSepLayerB", WS_EX_LAYERED, L"LAYERED(procB)"},
        {L"TestSepLayerB", 0, L"PLAIN(procB)"},
    };
    for (const auto& c : cases) {
        SetLastError(0);
        int self = 42;
        HWND w = CreateWindowExW(c.ex, c.cls, L"", WS_CHILD, 0, 0, 0, 0, parent, nullptr, inst, &self);
        printf("  %-18s ex=0x%08lX -> hwnd=%p err=0x%08lX\n", c.tag, (unsigned long)c.ex, (void*)w, GetLastError());
    }
    return 0;
}
