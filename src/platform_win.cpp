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

HWND CreateChild(const wchar_t* cls, HWND parent, void* self, int h96, HINSTANCE inst) {
    HWND h = CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, h96,
                             parent, nullptr, inst ? inst : GetModuleHandleW(nullptr), nullptr);
    if (h && self) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return h;
}

HWND CreatePopup(const wchar_t* cls, HWND owner, void* self, DWORD exStyle, DWORD style, int x,
                 int y, int w, int height, const wchar_t* title) {
    HWND h = CreateWindowExW(exStyle, cls, title ? title : L"", style, x, y, w, height, owner,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h && self) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return h;
}

void ThemeControl(HWND h, bool dark, const wchar_t* lightSubApp) {
    if (!h) return;
    const wchar_t* sub = dark ? L"DarkMode_Explorer" : (lightSubApp ? lightSubApp : L"Explorer");
    SetWindowTheme(h, sub, nullptr);
}

}  // namespace win
