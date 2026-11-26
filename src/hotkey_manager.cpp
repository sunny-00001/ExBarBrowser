// 快捷键管理器：窗口生命周期与消息路由。
//
// 【这个文件只管"窗口活着"这件事】模型在 *_model、几何在 *_layout、像素在
// *_draw、动作在 *_actions。这里只做三件：建/销毁窗口、把消息分派给那四个、
// 以及**主题切换**（那是窗口级的事，不属于任何一栏）。
//
// 分层理由与设置对话框一致（见 settings_dialog.cpp 顶部）：模型与绘制混在一起，
// 就会出现"点到的地方和画出来的地方不是同一处"——这种错在界面上表现为
// "点键位芯片没反应、点下一行反而开始改键"，极难查。
#include "hotkey_manager.h"

#include "platform_win.h"
#include "str.h"
#include "ui_draw.h"

#include <commctrl.h>

bool HotkeyManager::Create(HWND owner, const Theme& theme, InputRouter& router) {
    if (hwnd_) return true;
    owner_ = owner;
    theme_ = &theme;
    router_ = &router;

    // CS_DBLCLKS：双击行 = 开始改键（鼠标用户的第二条路，与回车等价）。
    // 不给这个类风格，WM_LBUTTONDBLCLK 根本不会来 —— 系统会把第二次按下
    // 当普通按下再发一次，双击就成了"选中两次"。
    if (!win::RegisterClassOnce(id::kHotkeysClass, WndProc, CS_DBLCLKS)) return false;

    const int w = theme.S(1120);
    const int h = theme.S(720);
    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    if (orc.right - orc.left <= 0) {
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        orc = wa;
    }
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    hwnd_ = win::CreatePopup(id::kHotkeysClass, owner, this, 0,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, L"快捷键管理器");
    if (!hwnd_) return false;

    ApplyDarkTitleBar(hwnd_, theme.dark);
    AllowDarkForWindow(hwnd_, theme.dark);

    // 搜索框是真 EDIT（有输入法、撤销、Ctrl+A）—— 自绘的假输入框这些都没有。
    searchEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0,
                                  10, 10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (searchEdit_) {
        SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);
        // 原生 EDIT 自己不会跟深色主题走（它画的是系统白底），必须显式套 SubApp。
        win::ThemeControl(searchEdit_, theme.dark, L"CFD");
        SetWindowTextW(searchEdit_, kSearchPlaceholder);
        SetWindowSubclass(searchEdit_, SearchSub, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    return true;
}

void HotkeyManager::Show() {
    if (!hwnd_) return;
    // 每次打开都重读一次：面板可能关了又开，期间 AI / 命令面板改过键位。
    OnKeymapChangedExternally();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    // 焦点落在搜索框：这个窗口最常见的第一个动作是"找一条命令"。
    // 想直接按键盘筛选的话，搜索框右边那颗录制按钮就是干这个的。
    if (searchEdit_) SetFocus(searchEdit_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HotkeyManager::ReloadTheme() {
    if (!hwnd_ || !theme_) return;
    ApplyDarkTitleBar(hwnd_, theme_->dark);
    AllowDarkForWindow(hwnd_, theme_->dark);
    if (searchEdit_) {
        SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_->font), TRUE);
        win::ThemeControl(searchEdit_, theme_->dark, L"CFD");
    }
    // 字体换了 → 行高与键位芯片的宽度都可能变，必须重排，不能只重绘。
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void HotkeyManager::OnKeymapChangedExternally() {
    if (!hwnd_) return;
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool HotkeyManager::Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK HotkeyManager::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // 与 settings_dialog 同一约定：this 走 lpCreateParams，在 WM_NCCREATE 里落进
    // GWLP_USERDATA。CreatePopup 那边用同一个窗口消息传 —— 两处必须一致，
    // 否则"有的窗口过程在 WM_NCCREATE 里就要用 this" 的那个会拿到空指针。
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    auto* self = GetThis<HotkeyManager>(h);
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->Handle(h, m, w, l);
}

LRESULT HotkeyManager::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;   // 全部自绘（ui::PaintBuffered 已经铺过底）
    case WM_CTLCOLOREDIT: {
        // 不处理这条消息，搜索框就是系统白底 —— 浅色主题下从灰底框里凸出来一块白。
        if (!theme_) break;
        HDC dc = reinterpret_cast<HDC>(w);
        SetBkColor(dc, theme_->barAlt);
        SetTextColor(dc, searchPlaceholder_ ? theme_->sub : theme_->text);
        return theme_->brBarAlt ? reinterpret_cast<LRESULT>(theme_->brBarAlt) : 0;
    }
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_SIZE:
        Layout();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        // 最小尺寸由三栏常量反推，不写死"手感值"：写死的数与布局脱钩之后，
        // 后果不是难看，而是右栏被压成一条缝、说明看不见 —— 没有任何检查会报。
        if (!theme_) return 0;
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        int cw = 0;
        int ch = 0;
        MinClientSize(*theme_, &cw, &ch);
        RECT fr{0, 0, cw, ch};
        AdjustWindowRectEx(&fr, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE)));
        if (mmi->ptMinTrackSize.x < fr.right - fr.left) mmi->ptMinTrackSize.x = fr.right - fr.left;
        if (mmi->ptMinTrackSize.y < fr.bottom - fr.top) mmi->ptMinTrackSize.y = fr.bottom - fr.top;
        return 0;
    }
    case WM_COMMAND: {
        if (searchEdit_ && l == reinterpret_cast<LPARAM>(searchEdit_) && HIWORD(w) == EN_CHANGE) {
            // 只在这里读控件文本（缓存到 searchText_），别处一律读缓存：
            // 控件身上挂着子类回调，回调里再给控件发 WM_GETTEXT 就是递归。
            const std::wstring raw = SearchRawText();
            searchPlaceholder_ = str::IEquals(raw, kSearchPlaceholder) || raw.empty();
            searchText_ = raw;
            query_ = searchPlaceholder_ ? std::wstring() : str::Trim(raw);
            Refilter();
            // 可见行变了 → 每一行的矩形都得重算。漏这一步的表现不是"难看"，
            // 而是"命中判定按新行、绘制按旧行"。
            Layout();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL:
        ScrollBy(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(l);
        const int y = GET_Y_LPARAM(l);
        const int row = HitRow(x, y);
        const int grp = HitGroup(x, y);
        const int tool = HitTool(x, y);
        const int sortHd = HitSortHeader(x, y);
        const POINT pt{x, y};
        const bool rec = PtInRect(&recordBtn_, pt) != FALSE;
        if (row != hoverRow_ || grp != hoverGroup_ || rec != hoverRecord_ || tool != hoverTool_ ||
            sortHd != hoverSort_) {
            hoverRow_ = row;
            hoverGroup_ = grp;
            hoverRecord_ = rec;
            hoverTool_ = tool;
            hoverSort_ = sortHd;
            InvalidateRect(h, nullptr, FALSE);
        }
        // 工具栏按钮的悬停提示落到状态栏 —— 按钮上写不下的解释放这里，
        // 而不是做成 tooltip 弹窗（弹窗会挡住按钮本身）。
        if (tool != kToolNone) {
            int n = 0;
            const ToolButton* items = ToolbarItems(&n);
            for (int i = 0; i < n; ++i) {
                if (items[i].id == tool && items[i].tip && *items[i].tip) {
                    if (status_ != items[i].tip) SetStatus(items[i].tip);
                    break;
                }
            }
        }
        if (!tracking_) {
            // 自绘控件的 hover 必须自己登记 TME_LEAVE：窗口默认收不到 WM_MOUSELEAVE。
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hoverRow_ != -1 || hoverGroup_ != -1 || hoverRecord_ || hoverTool_ != kToolNone ||
            hoverSort_ != -1) {
            hoverRow_ = -1;
            hoverGroup_ = -1;
            hoverRecord_ = false;
            hoverTool_ = kToolNone;
            hoverSort_ = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        // 点窗口任何空处都把焦点从搜索框收回 —— 但捕获/录制期间不能收，
        // 那两件事靠"焦点在本窗口"来收键。
        if (!capturing_ && !recording_) SetFocus(h);
        OnClick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_RBUTTONUP:
        // 用 UP 而不是 DOWN：右键按下就弹菜单会让"按下-拖走"（取消手势）失效。
        OnRightClick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_CONTEXTMENU: {
        // 键盘 Shift+F10 / 菜单键也走这里。lParam = -1 表示来自键盘，
        // 此时用选中行的位置当锚点。
        int x = GET_X_LPARAM(l);
        int y = GET_Y_LPARAM(l);
        if (x == -1 && y == -1) {
            const int vi = SelectedViewIndex();
            if (vi < 0) return 0;
            const RowLayout& rl = rowLayout_[static_cast<size_t>(vi)];
            x = rl.row.left + theme_->S(40);
            y = (rl.row.top + rl.row.bottom) / 2;
        }
        OnRightClick(x, y);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        OnDoubleClick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        // Alt+字母走 WM_SYSKEYDOWN，捕获组合键时必须一并接。
        const UINT vk = static_cast<UINT>(w);
        // Alt+F4 放行给 DefWindowProc：它不是"要捕获的键"，是"关窗"。
        // 拦掉的话用户会发现这个窗口关不掉（只能用 Esc 或标题栏叉）。
        if (!capturing_ && !recording_ && m == WM_SYSKEYDOWN && vk == VK_F4)
            return DefWindowProcW(h, m, w, l);
        // 只按修饰键：更新实时回显，不算一个完整组合键。
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN ||
            vk == VK_RWIN) {
            if (capturing_ || recording_) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        OnKeyDown(vk);
        return 0;
    }
    case WM_KILLFOCUS:
        // 捕获/录制期间焦点跑掉（用户点了主窗口或别的程序）：安静取消。
        // 不取消的话，用户以为还在改键，之后按的每个键都会被他处消费 ——
        // 而他看到的是"我按了 Ctrl+W，程序没反应"。
        if (capturing_) {
            CancelCapture();
            SetStatus(L"已取消改键：焦点离开了快捷键管理器", true);
        }
        if (recording_) StopRecording();
        return 0;
    case WM_TIMER:
        OnTimer(w);
        return 0;
    case WM_CLOSE:
        if (capturing_) CancelCapture();
        StopRecording();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        // 窗口没了，所有"指向某一行 / 某个键"的瞬态都要复位：对象是 App 成员，
        // 会跨窗口存活，留着它们下次打开就落在错误的行上。
        hwnd_ = nullptr;
        searchEdit_ = nullptr;
        capturing_ = false;
        recording_ = false;
        conflictPending_ = false;
        resetArmed_ = false;
        captureSetting_.clear();
        selSetting_.clear();
        hoverRow_ = -1;
        hoverGroup_ = -1;
        hoverRecord_ = false;
        hoverTool_ = kToolNone;
        hoverSort_ = -1;
        tracking_ = false;
        scroll_ = 0;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// 搜索框子类：Esc 清空（空则关窗），回车把焦点交给列表。
//
// 【只处理键，不读控件文本】读文本 = 给控件发 WM_GETTEXT，而这条回调就挂在
// 那个控件身上 —— 回调里再读一次就是无限递归（爆栈）。文本一律走 EN_CHANGE
// 缓存在 searchText_ 里。
LRESULT CALLBACK HotkeyManager::SearchSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                                          DWORD_PTR ref) {
    auto* self = reinterpret_cast<HotkeyManager*>(ref);
    if (self && self->hwnd_) {
        if (m == WM_KEYDOWN) {
            if (w == VK_ESCAPE) {
                if (self->query_.empty()) {
                    PostMessageW(self->hwnd_, WM_CLOSE, 0, 0);
                } else {
                    SetWindowTextW(h, L"");
                }
                return 0;
            }
            if (w == VK_RETURN) {
                SetFocus(self->hwnd_);
                if (!self->view_.empty() && self->selSetting_.empty()) self->SelectViewIndex(0);
                InvalidateRect(self->hwnd_, nullptr, FALSE);
                return 0;
            }
            if (w == VK_DOWN || w == VK_UP) {
                // 直接上下移动列表，不用先 Tab 出去 —— 搜到了就想选。
                SetFocus(self->hwnd_);
                self->OnKeyDown(static_cast<UINT>(w));
                return 0;
            }
        }
        if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;   // 别响提示音
    }
    return DefSubclassProc(h, m, w, l);
}
