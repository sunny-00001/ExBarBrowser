#include "prompt.h"

#include "modal.h"
#include "platform_win.h"
#include "ui_draw.h"

namespace {

// 类名由 identity.h 统一给出（规则 B1）
using id::kPromptClass;

// 两个对话框共用一套 96 DPI 逻辑尺寸：按钮组整体居中，标签/输入框按行排布。
// 以前单字段/双字段各写一份坐标，改一处就会错位；这里让绘制与控件创建都读同一份。
struct PromptLayout {
    int margin = 0;
    int labelH = 0;
    int editH = 0;
    int labelEditGap = 0;
    int rowGap = 0;
    int buttonW = 0;
    int buttonH = 0;
    int buttonGap = 0;
    int label1Y = 0;
    int edit1Y = 0;
    int label2Y = 0;
    int edit2Y = 0;
    int buttonY = 0;
    int height = 0;
};

PromptLayout MakePromptLayout(const Theme& theme, bool twoFields) {
    PromptLayout L;
    L.margin = theme.S(16);
    L.labelH = theme.S(18);
    L.editH = theme.S(26);
    L.labelEditGap = theme.S(4);
    L.rowGap = theme.S(14);
    L.buttonW = theme.S(76);
    L.buttonH = theme.S(28);
    L.buttonGap = theme.S(10);

    L.label1Y = L.margin;
    L.edit1Y = L.label1Y + L.labelH + L.labelEditGap;
    if (twoFields) {
        L.label2Y = L.edit1Y + L.editH + L.rowGap;
        L.edit2Y = L.label2Y + L.labelH + L.labelEditGap;
        L.buttonY = L.edit2Y + L.editH + L.rowGap;
    } else {
        L.label2Y = -1;
        L.edit2Y = -1;
        L.buttonY = L.edit1Y + L.editH + L.rowGap;
    }
    L.height = L.buttonY + L.buttonH + L.margin;
    return L;
}

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
    PromptLayout layout;
};

void RegisterClassOnce();

void StyleControl(HWND h, const Theme& theme, const wchar_t* subApp) {
    if (!h) return;
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);
    win::ThemeControl(h, theme.dark, subApp);
}

void CreateButtons(HWND owner, const Theme& theme, PromptState* st, int w) {
    const PromptLayout& L = st->layout;
    const int groupW = L.buttonW * 2 + L.buttonGap;
    const int x = (w - groupW) / 2;
    st->ok = CreateWindowExW(0, L"BUTTON", L"确定",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, x, L.buttonY,
                             L.buttonW, L.buttonH, owner, reinterpret_cast<HMENU>(IDOK),
                             GetModuleHandleW(nullptr), nullptr);
    st->cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 x + L.buttonW + L.buttonGap, L.buttonY, L.buttonW, L.buttonH,
                                 owner, reinterpret_cast<HMENU>(IDCANCEL),
                                 GetModuleHandleW(nullptr), nullptr);
    StyleControl(st->ok, theme, L"Explorer");
    StyleControl(st->cancel, theme, L"Explorer");
}

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
                const PromptLayout& L = st->layout;
                SetBkMode(dc, TRANSPARENT);
                // 两个 label 各调一次原语（它自己选字体并还原），不再共用一个 of ——
                // 共用时任何一条分支提前 return 都会把字体留在 DC 上。
                const RECT tr{L.margin, L.label1Y, rc.right - L.margin, L.label1Y + L.labelH};
                ui::DrawTextRow(dc, st->theme->font, st->label, tr, st->theme->sub,
                                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                if (!st->label2.empty() && L.label2Y >= 0) {
                    const RECT tr2{L.margin, L.label2Y, rc.right - L.margin,
                                   L.label2Y + L.labelH};
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
    st.layout = MakePromptLayout(theme, false);

    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    const int w = theme.S(360);
    const int h = st.layout.height;
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hwnd = win::CreatePopup(kPromptClass, owner, &st, WS_EX_DLGMODALFRAME,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, title.c_str());
    if (!hwnd) return false;

    const PromptLayout& L = st.layout;
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, L.margin,
                              L.edit1Y, w - L.margin * 2, L.editH, hwnd, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    StyleControl(st.edit, theme, L"CFD");

    CreateButtons(hwnd, theme, &st, w);

    ApplyDarkTitleBar(hwnd, theme.dark);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(st.edit);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);

    modal::Run([&st] { return st.done; }, [&](MSG& msg) {
        if (msg.message != WM_KEYDOWN || msg.hwnd != st.edit) return false;
        if (msg.wParam == VK_RETURN) {
            SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
            return true;
        }
        if (msg.wParam == VK_ESCAPE) {
            SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
            return true;
        }
        return false;
    });

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
    st.layout = MakePromptLayout(theme, true);

    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    const int w = theme.S(420);
    const int h = st.layout.height;
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hwnd = win::CreatePopup(kPromptClass, owner, &st, WS_EX_DLGMODALFRAME,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, title.c_str());
    if (!hwnd) return false;

    const PromptLayout& L = st.layout;
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial1.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, L.margin,
                              L.edit1Y, w - L.margin * 2, L.editH, hwnd, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    st.edit2 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial2.c_str(),
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, L.margin,
                               L.edit2Y, w - L.margin * 2, L.editH, hwnd, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    StyleControl(st.edit, theme, L"CFD");
    StyleControl(st.edit2, theme, L"CFD");

    CreateButtons(hwnd, theme, &st, w);

    ApplyDarkTitleBar(hwnd, theme.dark);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(st.edit);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);

    modal::Run([&st] { return st.done; }, [&](MSG& msg) {
        if (msg.message != WM_KEYDOWN || (msg.hwnd != st.edit && msg.hwnd != st.edit2))
            return false;
        if (msg.wParam == VK_RETURN) {
            SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
            return true;
        }
        if (msg.wParam == VK_ESCAPE) {
            SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
            return true;
        }
        if (msg.wParam == VK_TAB) {
            // 两输入框间切换焦点（IsDialogMessage 不在循环里，Tab 得自己处理）
            HWND to = (msg.hwnd == st.edit) ? st.edit2 : st.edit;
            SetFocus(to);
            SendMessageW(to, EM_SETSEL, 0, -1);
            return true;
        }
        return false;
    });

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
