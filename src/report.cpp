// 只读文本报告窗口实现（说明见 report.h）。
//
// 窗口骨架（模态循环 / 后台刷深色 / 恢复焦点）与 prompt.cpp 是同一套做法：
// 独立线程之外只有 UI 线程，模态期间必须自己泵消息，且结束后要把焦点还给视图，
// 否则 type-ahead 与 Delete 全失效。
#include "report.h"

#include "clipboard.h"
#include "identity.h"
#include "modal.h"
#include "platform_win.h"

namespace {

using id::kReportClass;

constexpr int kIdCopy = 101;

struct ReportState {
    HWND edit = nullptr;
    HWND copy = nullptr;
    HWND close = nullptr;
    const Theme* theme = nullptr;
    bool done = false;
    std::wstring text;
    bool copied = false;
};

void RegisterClassOnce();

LRESULT CALLBACK ReportProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = GetThis<ReportState>(h);
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(w);
        if (id == kIdCopy) {
            if (SetClipboardText(h, st->text)) {
                st->copied = true;
                SetWindowTextW(st->copy, L"已复制");
            } else {
                // 剪贴板被别的进程占着：说出来，不要假装成功。
                SetWindowTextW(st->copy, L"复制失败");
            }
            return 0;
        }
        if (id == IDCANCEL) {
            st->done = true;
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (st) st->done = true;
        return 0;
    case WM_ERASEBKGND: {
        if (!st) break;
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(reinterpret_cast<HDC>(w), &rc, st->theme->brBar);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if (st) {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, st->theme->text);
            SetBkColor(dc, st->theme->editBg);
            return reinterpret_cast<LRESULT>(st->theme->brEdit);
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (st) {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(dc, &rc, st->theme->brBar);
        }
        EndPaint(h, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void RegisterClassOnce() { win::RegisterClassOnce(kReportClass, ReportProc); }

}  // namespace

void ShowReport(HWND owner, const Theme& theme, const std::wstring& title,
                const std::wstring& text) {
    RegisterClassOnce();
    ReportState st;
    st.theme = &theme;
    st.text = text;

    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    const int w = theme.S(620);
    const int h = theme.S(460);
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = win::CreatePopup(kReportClass, owner, &st, WS_EX_DLGMODALFRAME,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, title.c_str());
    if (!hwnd) return;

    // ES_MULTILINE | ES_READONLY：能滚、能选、能复制，但改不动。
    // 右侧留出滚动条宽度（WS_VSCROLL），否则长行末尾会被按钮边缘压住。
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                  ES_AUTOVSCROLL,
                              theme.S(16), theme.S(16), w - theme.S(32), h - theme.S(72),
                              hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (st.edit) SendMessageW(st.edit, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);

    st.copy = CreateWindowExW(0, L"BUTTON", L"复制全部", WS_CHILD | WS_VISIBLE,
                              w - theme.S(196), h - theme.S(44), theme.S(84), theme.S(28), hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCopy)),
                              GetModuleHandleW(nullptr), nullptr);
    st.close = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                               w - theme.S(104), h - theme.S(44), theme.S(84), theme.S(28), hwnd,
                               reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleW(nullptr),
                               nullptr);
    for (HWND b : {st.copy, st.close})
        if (b) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);

    ApplyDarkTitleBar(hwnd, theme.dark);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(st.edit);
    SendMessageW(st.edit, EM_SETSEL, 0, 0);  // 从开头看，不要全选（误按键就整篇删掉观感差）

    modal::Run([&st] { return st.done; }, [&st](MSG& msg) {
        if (msg.message != WM_KEYDOWN) return false;
        // ESC 关窗（多行 EDIT 自己吃掉回车/方向键，这里只拦 ESC）。
        if (msg.wParam == VK_ESCAPE) {
            st.done = true;
            return true;
        }
        // Ctrl+C 在只读 EDIT 里本来就能用；这里额外提供 Ctrl+A 全选，
        // 因为只读框默认不支持（用户想复制整篇时唯一可行的手势）。
        if (msg.wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) && msg.hwnd == st.edit) {
            SendMessageW(st.edit, EM_SETSEL, 0, -1);
            return true;
        }
        return false;
    });
    if (owner) {
        EnableWindow(owner, TRUE);
        PostMessageW(owner, WM_APP_REFOCUS, 0, 0);
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
}
