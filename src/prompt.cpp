// 极简文本输入对话框（书签重命名 / 新建分类 / 标签组命名共用）。
// 用标准 EDIT + STATIC + BUTTON 控件；模态但不依赖资源文件。
//
// 【这里的返工要点】
//  1. CreateWindowEx 的 w/h 是**外窗尺寸**，不是客户区尺寸。旧实现把外窗尺寸
//     当成客户区来摆按钮，标题栏 + 边框吃掉高度后，“确定/取消”就被截在下边缘。
//     现在先用 AdjustWindowRectEx 反推外窗尺寸，再按真实客户区摆控件。
//  2. 标签由自绘文字改成原生 STATIC，输入框/按钮也统一走 Theme.S/theme.font/
//     win::ThemeControl，不再在提示框里另写一套“看起来像输入框/图标”的绘制。
//  3. 深浅色只调用已有的 ApplyDarkTitleBar/AllowDarkForWindow/ThemeControl，
//     颜色仍取 Theme 令牌，避免再出现一套独立的提示框配色。
#include "prompt.h"

#include "modal.h"
#include "platform_win.h"

namespace {

// 类名由 identity.h 统一给出（规则 B1）
using id::kPromptClass;

const DWORD kPromptStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
const DWORD kPromptExStyle = WS_EX_DLGMODALFRAME;

// 两个对话框共用一套 96 DPI 逻辑尺寸：按钮组整体居中，标签/输入框按行排布。
// 绘制与控件创建都读同一份，不会再出现“画在一个位置、控件在另一个位置”。
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
    L.margin = theme.S(10);
    L.labelH = theme.S(16);
    L.editH = theme.S(22);
    L.labelEditGap = theme.S(2);
    L.rowGap = theme.S(8);
    L.buttonW = theme.S(72);
    L.buttonH = theme.S(24);
    L.buttonGap = theme.S(8);

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

// clientW/clientH -> 外窗 w/h。CreatePopup 与 CreateWindowEx 一样，收的是外窗尺寸。
void PromptOuterSize(const PromptLayout& L, int clientW, int* outW, int* outH) {
    RECT fr{0, 0, clientW, L.height};
    if (AdjustWindowRectEx(&fr, kPromptStyle, FALSE, kPromptExStyle)) {
        *outW = fr.right - fr.left;
        *outH = fr.bottom - fr.top;
    } else {
        // 极端环境下拿不到非客户区度量：退回客户区尺寸，至少不会比旧实现更差。
        *outW = clientW;
        *outH = L.height;
    }
}

struct PromptState {
    HWND labelHwnd = nullptr;
    HWND edit = nullptr;
    HWND label2Hwnd = nullptr;
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

void CreateLabel(HWND owner, const Theme& theme, const PromptLayout& L, HWND* out,
                 const std::wstring& text, int y) {
    *out = CreateWindowExW(0, L"STATIC", text.c_str(),
                           WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, L.margin, y,
                           theme.S(1), L.labelH, owner, nullptr, GetModuleHandleW(nullptr),
                           nullptr);
    StyleControl(*out, theme, L"Explorer");
}

void CreateEdit(HWND owner, const Theme& theme, const PromptLayout& L, HWND* out,
                const std::wstring& initial, int y) {
    // 与 settings_dialog / nav_bar 的真 EDIT 一致：不画“假输入框”。
    // WS_EX_CLIENTEDGE + ThemeControl(CFD) 是工程里现成的输入控件外观。
    *out = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, L.margin, y,
                           theme.S(1), L.editH, owner, nullptr, GetModuleHandleW(nullptr),
                           nullptr);
    StyleControl(*out, theme, L"CFD");
}

void CreateButtons(HWND owner, const Theme& theme, PromptState* st) {
    // 初始尺寸占位；真正的坐标由 LayoutControls() 按客户区统一算。
    st->ok = CreateWindowExW(0, L"BUTTON", L"确定",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0,
                             st->layout.buttonW, st->layout.buttonH, owner,
                             reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr), nullptr);
    st->cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, st->layout.buttonW,
                                 st->layout.buttonH, owner, reinterpret_cast<HMENU>(IDCANCEL),
                                 GetModuleHandleW(nullptr), nullptr);
    StyleControl(st->ok, theme, L"Explorer");
    StyleControl(st->cancel, theme, L"Explorer");
}

void LayoutControls(HWND hwnd, PromptState* st) {
    if (!st) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    const PromptLayout& L = st->layout;
    const int contentW = cw - L.margin * 2;

    if (st->labelHwnd) MoveWindow(st->labelHwnd, L.margin, L.label1Y, contentW, L.labelH, TRUE);
    if (st->edit) MoveWindow(st->edit, L.margin, L.edit1Y, contentW, L.editH, TRUE);
    if (st->label2Hwnd) MoveWindow(st->label2Hwnd, L.margin, L.label2Y, contentW, L.labelH, TRUE);
    if (st->edit2) MoveWindow(st->edit2, L.margin, L.edit2Y, contentW, L.editH, TRUE);

    // 再保守一层：即使非客户区度量与预期不一致，也把按钮钉在客户区内。
    int buttonY = L.buttonY;
    const int maxButtonY = ch - L.buttonH - L.margin;
    if (buttonY > maxButtonY) buttonY = maxButtonY;
    if (buttonY < 0) buttonY = 0;
    const int groupW = L.buttonW * 2 + L.buttonGap;
    const int buttonX = (cw - groupW) / 2;
    if (st->ok) MoveWindow(st->ok, buttonX, buttonY, L.buttonW, L.buttonH, TRUE);
    if (st->cancel)
        MoveWindow(st->cancel, buttonX + L.buttonW + L.buttonGap, buttonY, L.buttonW,
                   L.buttonH, TRUE);
}

// 这个窗口过程之前是缺失的：类注册成 DefWindowProc，WM_COMMAND(IDOK/IDCANCEL)
// 没人处理 → st.done 永远为 false → 模态循环不退出、owner 被禁用 → 整个程序卡死
// （书签重命名/新建分类/删除确认都会踩到）。现在由它负责“提交/取消/关闭”。
LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = GetThis<PromptState>(h);
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    case WM_SIZE:
        LayoutControls(h, st);
        return 0;
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
        if (st) {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, st->theme->sub);
            SetBkColor(dc, st->theme->bar);
            return reinterpret_cast<LRESULT>(st->theme->brBar);
        }
        break;
    case WM_CTLCOLOREDIT:
        if (st) {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, st->theme->text);
            SetBkColor(dc, st->theme->editBg);
            return reinterpret_cast<LRESULT>(st->theme->brEdit);
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void RegisterClassOnce() { win::RegisterClassOnce(kPromptClass, PromptProc); }

// 创建 + 居中 + 起模态。两个函数只差字段数，公共部分收在这里。
bool RunPrompt(HWND owner, const Theme& theme, const std::wstring& title,
               const std::wstring& label1, const std::wstring& initial1, std::wstring* out1,
               const std::wstring* label2, const std::wstring* initial2, std::wstring* out2) {
    RegisterClassOnce();
    const bool twoFields = label2 != nullptr;
    PromptState st;
    st.theme = &theme;
    st.label = label1;
    if (label2) st.label2 = *label2;
    st.layout = MakePromptLayout(theme, twoFields);

    const int clientW = theme.S(twoFields ? 420 : 360);
    int winW = clientW;
    int winH = st.layout.height;
    PromptOuterSize(st.layout, clientW, &winW, &winH);

    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    if (orc.right - orc.left <= 0) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    }
    int x = orc.left + ((orc.right - orc.left) - winW) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - winH) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hwnd = win::CreatePopup(kPromptClass, owner, &st, kPromptExStyle, kPromptStyle, x, y,
                                 winW, winH, title.c_str());
    if (!hwnd) return false;

    CreateLabel(hwnd, theme, st.layout, &st.labelHwnd, label1, st.layout.label1Y);
    CreateEdit(hwnd, theme, st.layout, &st.edit, initial1, st.layout.edit1Y);
    if (twoFields) {
        CreateLabel(hwnd, theme, st.layout, &st.label2Hwnd, st.label2, st.layout.label2Y);
        CreateEdit(hwnd, theme, st.layout, &st.edit2, *initial2, st.layout.edit2Y);
    }
    CreateButtons(hwnd, theme, &st);
    LayoutControls(hwnd, &st);

    ApplyDarkTitleBar(hwnd, theme.dark);
    AllowDarkForWindow(hwnd, theme.dark);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    if (st.edit) {
        SetFocus(st.edit);
        SendMessageW(st.edit, EM_SETSEL, 0, -1);
    }

    modal::Run([&st] { return st.done; }, [&](MSG& msg) {
        if (msg.message != WM_KEYDOWN || (msg.hwnd != st.edit && msg.hwnd != st.edit2)) return false;
        if (msg.wParam == VK_RETURN) {
            SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
            return true;
        }
        if (msg.wParam == VK_ESCAPE) {
            SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
            return true;
        }
        if (twoFields && msg.wParam == VK_TAB) {
            // 两输入框间切换焦点（IsDialogMessage 不在循环里，Tab 得自己处理）。
            HWND to = (msg.hwnd == st.edit) ? st.edit2 : st.edit;
            if (to) {
                SetFocus(to);
                SendMessageW(to, EM_SETSEL, 0, -1);
            }
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
    if (st.ok_) {
        if (out1) *out1 = st.result;
        if (twoFields && out2) *out2 = st.result2;
    }
    return st.ok_;
}

}  // namespace

bool PromptText(HWND owner, const Theme& theme, const std::wstring& title,
                const std::wstring& label, const std::wstring& initial, std::wstring* out) {
    return RunPrompt(owner, theme, title, label, initial, out, nullptr, nullptr, nullptr);
}

bool PromptText2(HWND owner, const Theme& theme, const std::wstring& title,
                 const std::wstring& label1, const std::wstring& initial1, std::wstring* out1,
                 const std::wstring& label2, const std::wstring& initial2, std::wstring* out2) {
    return RunPrompt(owner, theme, title, label1, initial1, out1, &label2, &initial2, out2);
}
