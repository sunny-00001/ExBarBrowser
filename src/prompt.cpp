#include "prompt.h"

#include "platform_win.h"
#include "ui_draw.h"

namespace {

// 类名由 identity.h 统一给出（规则 B1）
using id::kPromptClass;

struct PromptState {
    HWND edit = nullptr;
    HWND edit2 = nullptr;  // 双字段版第二输入框（单字段版为 null）
    HWND ok = nullptr;
    HWND cancel = nullptr;
    const Theme* theme = nullptr;
    std::wstring result;
    std::wstring result2;
    bool ok_ = false;
    bool done = false;
    std::wstring label;
    std::wstring label2;
};

void RegisterClassOnce();

// 这个窗口过程之前是缺失的：类注册成 DefWindowProc，WM_COMMAND(IDOK/IDCANCEL)
// 没人处理 → st.done 永远为 false → 模态循环不退出、owner 被禁用 → 整个程序卡死
// （书签重命名/新建分类/删除确认都会踩到）。现在由它负责"提交/取消/关闭"。
LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = GetThis<PromptState>(h);
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(w);
        if (id == IDOK) {
            auto read = [](HWND e) {
                if (!e) return std::wstring();
                int n = GetWindowTextLengthW(e);
                std::wstring t(static_cast<size_t>(n) + 1, L'\0');
                if (n > 0) GetWindowTextW(e, &t[0], n + 1);
                t.resize(static_cast<size_t>(n));
                return t;
            };
            st->result = read(st->edit);
            st->result2 = read(st->edit2);
            st->ok_ = true;
            st->done = true;
            return 0;
        }
        if (id == IDCANCEL) {
            st->ok_ = false;
            st->done = true;
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (st) {
            st->ok_ = false;
            st->done = true;
        }
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
            if (!st->label.empty()) {
                SetBkMode(dc, TRANSPARENT);
                // 两个 label 各调一次原语（它自己选字体并还原），不再共用一个 of ——
                // 共用时任何一条分支提前 return 都会把字体留在 DC 上。
                const RECT tr{st->theme->S(16), st->theme->S(18), rc.right - st->theme->S(16),
                              st->theme->S(42)};
                ui::DrawTextRow(dc, st->theme->font, st->label, tr, st->theme->sub,
                                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                if (!st->label2.empty()) {
                    const RECT tr2{st->theme->S(16), st->theme->S(76),
                                   rc.right - st->theme->S(16), st->theme->S(100)};
                    ui::DrawTextRow(dc, st->theme->font, st->label2, tr2, st->theme->sub,
                                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void RegisterClassOnce() { win::RegisterClassOnce(kPromptClass, PromptProc); }

}  // namespace

bool PromptText(HWND owner, const Theme& theme, const std::wstring& title,
                const std::wstring& label, const std::wstring& initial, std::wstring* out) {
    RegisterClassOnce();
    PromptState st;
    st.theme = &theme;
    st.label = label;

    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    const int w = theme.S(420);
    const int h = theme.S(150);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = win::CreatePopup(kPromptClass, owner, &st, WS_EX_DLGMODALFRAME,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, title.c_str());
    if (!hwnd) return false;

    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, theme.S(16), theme.S(44),
                              w - theme.S(32), theme.S(26), hwnd, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (st.edit) SendMessageW(st.edit, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);

    st.ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                            w - theme.S(196), theme.S(84), theme.S(84), theme.S(28), hwnd,
                            reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr), nullptr);
    st.cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, w - theme.S(104),
                                theme.S(84), theme.S(84), theme.S(28), hwnd,
                                reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleW(nullptr),
                                nullptr);
    for (HWND b : {st.ok, st.cancel})
        if (b) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);

    ApplyDarkTitleBar(hwnd, theme.dark);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(st.edit);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);

    MSG msg;
    int gm;
    // GetMessage 返回 -1 是错误（不能继续循环），返回 0 是 WM_QUIT（要原样交还宿主）。
    while (!st.done && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == st.edit) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gm == 0) PostQuitMessage(static_cast<int>(msg.wParam));

    if (owner) {
        EnableWindow(owner, TRUE);
        // 把焦点还给视图（异步：此时本窗口还没销毁，直接 SetFocus 会被抢回去）。
        // 否则 ESC 取消后焦点停在不可见的 OWNER 上，type-ahead / Delete 全失效。
        PostMessageW(owner, WM_APP_REFOCUS, 0, 0);
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (st.ok_ && out) *out = st.result;
    return st.ok_;
}

bool PromptText2(HWND owner, const Theme& theme, const std::wstring& title,
                 const std::wstring& label1, const std::wstring& initial1, std::wstring* out1,
                 const std::wstring& label2, const std::wstring& initial2, std::wstring* out2) {
    RegisterClassOnce();
    PromptState st;
    st.theme = &theme;
    st.label = label1;
    st.label2 = label2;

    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    const int w = theme.S(460);
    const int h = theme.S(200);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = win::CreatePopup(kPromptClass, owner, &st, WS_EX_DLGMODALFRAME,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, title.c_str());
    if (!hwnd) return false;

    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial1.c_str(),
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, theme.S(16), theme.S(44),
                              w - theme.S(32), theme.S(26), hwnd, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (st.edit) SendMessageW(st.edit, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);
    st.edit2 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial2.c_str(),
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, theme.S(16),
                               theme.S(102), w - theme.S(32), theme.S(26), hwnd, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (st.edit2) SendMessageW(st.edit2, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);

    st.ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                            w - theme.S(196), theme.S(148), theme.S(84), theme.S(28), hwnd,
                            reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr), nullptr);
    st.cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, w - theme.S(104),
                                theme.S(148), theme.S(84), theme.S(28), hwnd,
                                reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleW(nullptr),
                                nullptr);
    for (HWND b : {st.ok, st.cancel})
        if (b) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);

    ApplyDarkTitleBar(hwnd, theme.dark);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(st.edit);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);

    MSG msg;
    int gm;
    while (!st.done && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == WM_KEYDOWN && (msg.hwnd == st.edit || msg.hwnd == st.edit2)) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
                continue;
            }
            if (msg.wParam == VK_TAB) {
                // 两输入框间切换焦点（IsDialogMessage 不在循环里，Tab 得自己处理）
                HWND to = (msg.hwnd == st.edit) ? st.edit2 : st.edit;
                SetFocus(to);
                SendMessageW(to, EM_SETSEL, 0, -1);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gm == 0) PostQuitMessage(static_cast<int>(msg.wParam));

    if (owner) {
        EnableWindow(owner, TRUE);
        PostMessageW(owner, WM_APP_REFOCUS, 0, 0);
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (st.ok_) {
        if (out1) *out1 = st.result;
        if (out2) *out2 = st.result2;
    }
    return st.ok_;
}
