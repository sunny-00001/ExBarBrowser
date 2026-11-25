// 窗口类注册与自绘子窗口外壳（唯一实现）。见 platform_win.h。
#include "platform_win.h"

#include <string>
#include <unordered_set>
#include <uxtheme.h>

namespace win {
namespace {

// 已注册类名集合：以前每个文件一个 bool 门闩，11 份；这里收成一张表，
// 新增类不需要再加静态变量，也顺手挡住"同一个类名注册两次"。
std::unordered_set<std::wstring>& Registered() {
    static std::unordered_set<std::wstring> s;
    return s;
}

}  // namespace

bool RegisterClassOnce(const wchar_t* name, WNDPROC proc, UINT style, HICON icon) {
    if (!name || Registered().count(name)) return true;
    WNDCLASSW wc{};
    wc.style = style;
    wc.lpfnWndProc = proc ? proc : DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon;
    wc.lpszClassName = name;
    if (!RegisterClassW(&wc)) return false;
    Registered().insert(name);
    return true;
}

HWND CreateChild(const wchar_t* cls, HWND parent, void* self, int heightPx, HINSTANCE inst,
                 DWORD style) {
    // 与 CreatePopup 同一约定：self 走 lpCreateParams，创建后再补设一次。
    // 本工程的栏都用 GetThis<T>(h) 在过程开头读 GWLP_USERDATA，不处理 WM_NCCREATE，
    // 所以事后设置对它们是够的；但**约定必须一致** —— 否则下一个给栏加 WM_NCCREATE
    // 的人会拿到空指针，而 CreatePopup 那边却正常，这种不一致最难查。
    HWND h = CreateWindowExW(0, cls, L"", style, 0, 0, 100, heightPx, parent, nullptr,
                             inst ? inst : GetModuleHandleW(nullptr), self);
    if (h && self) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return h;
}

HWND CreatePopup(const wchar_t* cls, HWND owner, void* self, DWORD exStyle, DWORD style, int x,
                 int y, int w, int height, const wchar_t* title) {
    // 【必须用 lpCreateParams 传 self，不能创建后再 SetWindowLongPtrW】
    // 本工程的弹层/对话框窗口过程都实现了 WM_NCCREATE，里面会读
    // CREATESTRUCTW::lpCreateParams 写进 GWLP_USERDATA：
    //     SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    // WM_NCCREATE 发生在 CreateWindowExW 返回**之前**。若这里传 nullptr 再事后补设，
    // WM_NCCREATE 会先用 nullptr 覆盖一次（无害），但更糟的是：任何在 WM_NCCREATE
    // 里就要用 this 的窗口过程会直接拿到空指针。统一走 lpCreateParams 才是唯一安全解。
    HWND h = CreateWindowExW(exStyle, cls, title ? title : L"", style, x, y, w, height, owner,
                             nullptr, GetModuleHandleW(nullptr), self);
    if (h && self) {
        // 兜底：窗口过程**没有**处理 WM_NCCREATE 的情形（如纯 DefWindowProcW 的窗口类）
        if (!GetWindowLongPtrW(h, GWLP_USERDATA))
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return h;
}

void ThemeControl(HWND h, bool dark, const wchar_t* subApp) {
    if (!h) return;
    if (!subApp || !*subApp) subApp = L"Explorer";
    // 深色统一加 DarkMode_ 前缀；浅色就是后缀本身（Explorer / CFD 都是系统已知的 SubAppName）
    std::wstring name = dark ? (std::wstring(L"DarkMode_") + subApp) : subApp;
    SetWindowTheme(h, name.c_str(), nullptr);
}

}  // namespace win
