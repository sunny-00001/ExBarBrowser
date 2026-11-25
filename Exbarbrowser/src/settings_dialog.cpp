// 设置对话框：窗口生命周期与窗口过程。
//
// 职责切分（六个文件，按"做什么"分，不按行数分）：
//   settings_dialog.cpp            窗口生命周期 / 消息路由
//   settings_dialog_model.cpp      模型（哪些行可见 / 当前值是什么）
//   settings_dialog_layout.cpp     排布（画在哪）/ 命中（点到哪儿）
//   settings_dialog_actions.cpp    交互（点了做什么 / 写成什么）
//   settings_dialog_draw.cpp       绘制（左栏 / 中栏）
//   settings_dialog_draw_help.cpp  绘制（右栏说明 + 底部状态栏）
//
// 【为什么模型与绘制分开】"当前显示哪些行"和"这些行画在哪"是两件事，
// 混在一起就会出现"点到的地方和画出来的地方不是同一处" —— 那种错在界面上
// 表现为"点在复选框上却切换了别的项"，极难查。
// 排布与命中共用同一份矩形，所以它们住在同一个文件里。
#include "settings_dialog.h"

#include "clipboard.h"
#include "commands.h"
#include "config.h"
#include "platform_win.h"
#include "str.h"
#include "ui_draw.h"


// ---------------------------------------------------------------- 生命周期

bool SettingsDialog::Create(HWND owner, const Theme& theme) {
    if (hwnd_) return true;
    owner_ = owner;
    theme_ = &theme;
    if (!win::RegisterClassOnce(id::kSettingsClass, WndProc)) return false;

    // 尺寸：与原型一致（tmp_settings_mockup/app.css 的 .win 是 1080×720）。
    // 宽度不是随手定的 —— 三栏 200 + 说明区 + 204 的比例下，宽度决定说明列
    // 能显示多少字；比原型窄 20px，说明列就少一截，全是省略号。
    const int w = theme.S(1080);
    const int h = theme.S(720);
    // 位置：落在主窗口中间；主窗口不可见时退回屏幕工作区中间。
    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    if (orc.right - orc.left <= 0) {
        RECT wr{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wr, 0);
        orc = wr;
    }
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    hwnd_ = win::CreatePopup(id::kSettingsClass, owner, this, 0,
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, L"设置");
    if (!hwnd_) return false;

    ApplyDarkTitleBar(hwnd_, theme.dark);
    AllowDarkForWindow(hwnd_, theme.dark);

    // 搜索框：真的 EDIT 控件（不是自绘的假输入框）——
    // 真控件才有输入法、撤销、Ctrl+A、全选复制。
    searchEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (searchEdit_) {
        SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);
        // 占位文字（规则见 settings_dialog.h 的 kSearchPlaceholder）。
        SetWindowTextW(searchEdit_, kSearchPlaceholder);
        SetWindowSubclass(searchEdit_, SearchSub, 1, 0);
    }

    // 数字项的行内编辑框：平时隐藏，开始编辑时才露出来。
    valueEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_RIGHT, 0, 0, 10,
                                 10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (valueEdit_) {
        SendMessageW(valueEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);
        SetWindowSubclass(valueEdit_, ValueSub, 1, 0);
    }

    RebuildModel();
    Layout();
    return true;
}

void SettingsDialog::Show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    RefreshValues();
    // 焦点落在搜索框：这个窗口最常见的第一个动作是"找一项设置"。
    // "打开后还要先点一下输入框才能打字"是白白多出来的一步。
    // 占位文字只显示、不自动全选（见 SearchSub）—— 全选底色只包字形高度，
    // 会让选中态与未选中态看起来上下错位。
    if (searchEdit_) SetFocus(searchEdit_);
    // 配置文件解析失败 = 本次所有修改都不会写盘。必须在用户动手**之前**说 ——
    // 否则他改完一批设置、重启才发现一项都没留下，而那时已分不清是哪一步丢的。
    if (Config::Get().LoadFailed())
        // 配置只在启动时读一次（Config::Load 幂等），所以"修好文件"之后必须
        // 重启才会生效 —— 不说这一句，用户会以为改完文件就能继续用，
        // 而本次会话里每一个改动仍然不会写盘。
        SetStatus(L"配置文件解析失败：本次修改不会写盘；修好 settings.json 后需重启", true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDialog::ReloadTheme() {
    if (!hwnd_ || !theme_) return;
    ApplyDarkTitleBar(hwnd_, theme_->dark);
    AllowDarkForWindow(hwnd_, theme_->dark);
    for (HWND c : {searchEdit_, valueEdit_})
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(theme_->font), TRUE);
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SettingsDialog::OnConfigChangedExternally() {
    // 没开就不做：Create 之前 rows_ 是空的，RefreshValues 会静默什么都不做，
    // 但 Layout() 会去算一个不存在的窗口 —— 早退省掉这一串无意义工作。
    if (!hwnd_) return;
    // 只重读值。理由见声明处：外部改动不会改变项集合，重建会丢选中与滚动位置。
    RefreshValues();
    // 值的文本宽度可能变了（"30" → "200"），右栏的取值域说明也可能变了，
    // 所以重排一次；不重排会出现"数字溢出控件、压到单位上"。
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK SettingsDialog::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    auto* self = GetThis<SettingsDialog>(h);
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->Handle(h, m, w, l);
}

LRESULT SettingsDialog::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;   // 全部自绘（走 ui::PaintBuffered）
    case WM_CTLCOLOREDIT: {
        // 输入框的底与框一致；占位文字用浅色 —— 一眼看出"这不是内容"。
        // 不处理 WM_CTLCOLOREDIT 的话，控件用系统默认的白底，
        // 浅色主题下那块白会从灰底搜索框里凸出来。
        if (!theme_) break;
        HDC dc = reinterpret_cast<HDC>(w);
        HWND ctl = reinterpret_cast<HWND>(l);
        if (ctl == searchEdit_) {
            SetBkColor(dc, theme_->barAlt);
            SetTextColor(dc, SearchShowsPlaceholder() ? theme_->sub : theme_->text);
            return theme_->brBarAlt ? reinterpret_cast<LRESULT>(theme_->brBarAlt) : 0;
        }
        if (ctl == valueEdit_) {
            SetBkColor(dc, theme_->editBg);
            SetTextColor(dc, theme_->text);
            return theme_->brEdit ? reinterpret_cast<LRESULT>(theme_->brEdit) : 0;
        }
        break;
    }
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_SIZE:
        Layout();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        // 最小尺寸由 MinClientSize 反推（三栏常量 + 右栏固定块 + 状态栏），
        // 再换算出**窗框**下限 —— 拖拽约束用的是整窗尺寸，不是客户区。
        //
        // 【为什么不能写死一个"手感值"】写死的那个数一旦与布局常量脱钩，
        // 后果不是"难看"，而是静默的功能缺失：比 720 窄一点，右栏就被压成
        // 一条缝（说明看不见）；比 420 矮一点，"何时生效"整节消失。
        // 这两种都没有任何检查会报 —— 界面照常画，只是少了东西。
        if (!theme_) return 0;
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        int cw = 0;
        int chh = 0;
        MinClientSize(*theme_, &cw, &chh);
        RECT fr{0, 0, cw, chh};
        // 与 Create 里用的样式一致；AdjustWindowRectEx 不接受 WS_CLIPCHILDREN
        // 之外的"会影响边框"的标志，传当前窗口的样式是唯一不会脱钩的写法。
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE));
        AdjustWindowRectEx(&fr, style, FALSE, exStyle);
        const int minW = fr.right - fr.left;
        const int minH = fr.bottom - fr.top;
        if (mmi->ptMinTrackSize.x < minW) mmi->ptMinTrackSize.x = minW;
        if (mmi->ptMinTrackSize.y < minH) mmi->ptMinTrackSize.y = minH;
        return 0;
    }
    case WM_COMMAND: {
        if (searchEdit_ && l == reinterpret_cast<LPARAM>(searchEdit_) && HIWORD(w) == EN_CHANGE) {
            // 读一次控件文本，存进缓存（后面各处都读缓存，不再碰控件）。
            UpdateSearchFromControl();
            RebuildModel();
            // 搜索改了可见行 → 行数变了 → **每一行的矩形都得重算**。
            // 漏掉这一步的后果不是"排版难看"，而是"控件画在上一组的位置上"：
            // 搜索前是复选框的行，搜索后仍是复选框的位置，但那一格其实是下拉框，
            // 于是点下去毫无反应。这种错没有任何检查会报，只能靠这一行注释记住。
            Layout();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        OnWheel(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(l);
        const int y = GET_Y_LPARAM(l);
        // hoverRow_ 记的是**行下标**（rows_ 里的），不是中栏的内容下标 ——
        // 两者在搜索态下不是一回事（中间隔着段标题条）。
        const int item = HitItem(x, y);
        const int newRow = (item >= 0 && !items_[static_cast<size_t>(item)].header)
                               ? items_[static_cast<size_t>(item)].rowIndex
                               : -1;
        const int grp = HitGroup(x, y);
        if (newRow != hoverRow_ || grp != hoverGroup_) {
            hoverRow_ = newRow;
            hoverGroup_ = grp;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (!tracking_) {
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
        if (hoverRow_ != -1 || hoverGroup_ != -1) {
            hoverRow_ = -1;
            hoverGroup_ = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        // 先同步结算正在进行的行内编辑，再处理点击。OnClick 可能触发
        // RebuildModel（切组 / 搜索），那会把 rows_ 整体换掉；若把提交留给
        // 失焦的异步消息，它会拿"新模型的同一行号"去写另一个设置项。
        if (!editKey_.empty()) CommitEdit(true);
        SetFocus(h);   // 点空白处时把焦点从输入框收回窗口
        OnClick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    }
    case WM_KEYDOWN: {
        switch (w) {
        case VK_ESCAPE:
            // 与"点别处 = 提交"同一契约：关闭前先把在编辑的值落下去，
            // 否则用户敲了一半的数字会随窗口一起消失。
            if (!editKey_.empty()) CommitEdit(true);
            DestroyWindow(h);
            return 0;
        case VK_TAB:
            // 这个窗口不是对话框（没有对话框管理器，消息循环里也没有
            // IsDialogMessage），Tab **没有**任何默认处理 —— 不自己接就永远
            // 停在原地。落点只有两个：搜索框与表单，所以不搞焦点环，
            // 直接在两处之间往返。
            if (searchEdit_) SetFocus(searchEdit_);
            return 0;
        case VK_DOWN: MoveSelection(1); return 0;
        case VK_UP: MoveSelection(-1); return 0;
        case VK_HOME:
            if (!rows_.empty()) SelectRowAt(0);
            return 0;
        case VK_END:
            if (!rows_.empty()) SelectRowAt(static_cast<int>(rows_.size()) - 1);
            return 0;
        case VK_PRIOR: ScrollBy(-static_cast<int>(form_.bottom - form_.top)); return 0;
        case VK_NEXT: ScrollBy(static_cast<int>(form_.bottom - form_.top)); return 0;
        case VK_RETURN:
        case VK_SPACE:
            // 与"点控件"同一个动作（ApplyRowControl）：键盘与鼠标不能各有一套。
            ActivateSelected();
            return 0;
        default:
            break;
        }
        break;
    }
    case kMsgCommitEdit: {
        CommitEdit(w != 0);
        return 0;
    }
    case WM_CLOSE:
        if (!editKey_.empty()) CommitEdit(true);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        // 窗口没了，所有"指向某一行"的瞬态都要复位，否则重开时它们会
        // 落在新模型里错误的行上（对象是 App 成员，跨窗口存活）。
        hwnd_ = nullptr;
        searchEdit_ = nullptr;
        valueEdit_ = nullptr;
        editKey_.clear();
        badKey_.clear();
        hoverRow_ = -1;
        hoverGroup_ = -1;
        tracking_ = false;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
