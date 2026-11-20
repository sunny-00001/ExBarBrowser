// 窗口类的注册与"自绘子窗口"的统一外壳（唯一实现）。
//
// 收编理由：RegisterClassW 在 11 个文件各写一遍，每份都是同样的 8 行
//   WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
//   wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = ...; RegisterClassW(&wc);
// 再配一个 `bool g_classReady` 门闩。而且**其中 7 处**注册时把 lpfnWndProc 填成
// DefWindowProcW，创建窗口后再用 SetWindowLongPtrW(GWLP_WNDPROC) 换掉 —— 这不是
// 必要手段，只是照着上一份抄的结果：窗口还没创建，直接注册真实 WndProc 即可。
//
// 规则 R12 目标：窗口类 ≤3（Lister / Popup / Dialog）。现实仍多，但至少
// "注册"这件事现在只有一份实现，新增一个类是一行调用而不是 8 行样板 + 一个静态变量。
#pragma once
#include <windows.h>

namespace win {

// 注册一次（幂等）。style 可加 CS_DBLCLKS / CS_DROPSHADOW；
// hIcon 只在顶层窗口类需要（子窗口传 nullptr）。
// 返回 false = 注册失败（类名冲突或系统资源不足），调用方应据此放弃创建。
bool RegisterClassOnce(const wchar_t* name, WNDPROC proc, UINT style = 0, HICON icon = nullptr);

// 创建自绘子窗口 + 挂 this 指针（GWLP_USERDATA）+ 统一 WndProc 转发。
// 省掉"SetThis + SetWindowLongPtrW(GWLP_WNDPROC)"这套事后补丁。
// 已注册的类必须用注册时的真实 WndProc；本函数不再改写窗口过程。
//
// heightPx 是**已缩放的像素**（调用方传 theme.S(h96)）；初始宽度统一 100，
// 随后由 Layout/Resize 决定真实几何。style 默认可见子窗口。
HWND CreateChild(const wchar_t* cls, HWND parent, void* self, int heightPx,
                 HINSTANCE inst = nullptr,
                 DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN);

// 创建顶层/弹出窗口（带 this 指针）。exStyle 用于 WS_EX_TOOLWINDOW 等。
HWND CreatePopup(const wchar_t* cls, HWND owner, void* self, DWORD exStyle, DWORD style, int x,
                 int y, int w, int h, const wchar_t* title = L"");

// 给原生控件套主题（EDIT / LISTBOX / TREEVIEW…）。
// 以前 nav_bar / popup_list / shell_view 各写一遍同一个三元表达式
// `dark ? L"DarkMode_<X>" : L"Explorer"`，且 X 取值散落（CFD / Explorer）。
//
// subApp 是深浅两套 SubAppName 的**共同后缀**：
//   "Explorer" → 深色用 DarkMode_Explorer（列表/树/滚动条）
//   "CFD"      → 深色用 DarkMode_CFD（编辑框，即 Common File Dialog 那套）
void ThemeControl(HWND h, bool dark, const wchar_t* subApp = L"Explorer");

}  // namespace win
