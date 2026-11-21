#include "nav_bar.h"

#include "platform_win.h"

#include "ui_draw.h"
#include "icon_resolve.h"
#include "mouse_rules.h"
#include "nav_metrics.h"

namespace {

WNDPROC g_oldEditProc = nullptr;
WNDPROC g_oldSearchProc = nullptr;

}  // namespace

NavBar::~NavBar() {
    for (auto& b : buttons_)
        if (b.icon) DestroyIcon(b.icon);
    if (leadIcon_) DestroyIcon(leadIcon_);
}

void NavBar::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kNavBarClass, NavBar::WndProc);
    parent_ = parent;
    theme_ = &theme;

    hwnd_ = win::CreateChild(kNavBarClass, parent, this, theme.S(30));
    if (!hwnd_) return;

    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10,
                            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (edit_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);
        win::ThemeControl(edit_, theme.dark, L"CFD");
        g_oldEditProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
        // 系统自动补全（目录优先）：与 mod 的 SHACF_FILESYS_DIRS 同口径。
        // 只补全、不自动追加，避免把用户没确认的路径吞进去。
        SHAutoComplete(edit_, SHACF_FILESYSTEM | SHACF_FILESYS_DIRS | SHACF_AUTOAPPEND_FORCE_OFF);
    }

    BuildButtons();
    SearchCreate();
    Layout();
    // Layout 在"路径为空/正处于编辑态"时会提前返回，那时按钮提示还没建起来。
    // 这里显式补一次：冷启动第一帧就应当有提示，而不是等第一次导航。
    RebuildTooltip();
}

void NavBar::BuildButtons() {
    // Win11 资源管理器同一套 MDL2 字形：系统 stock 图标是深色位图，深色主题下几乎
    // 不可见（实测），所以统一走字形并按主题色着色；back/forward 带分裂箭头。
    // 字形一律取 ico::Glyph(id)，不写码位
    buttons_.push_back({kBack, nullptr, ico::Glyph(ico::IconId::Back), {}, {}, true, true});
    buttons_.push_back({kForward, nullptr, ico::Glyph(ico::IconId::Forward), {}, {}, true, true});
    buttons_.push_back({kUp, nullptr, ico::Glyph(ico::IconId::Up), {}, {}, false, true});
    buttons_.push_back({kRefresh, nullptr, ico::Glyph(ico::IconId::Refresh), {}, {}, false, true});
    buttons_.push_back({kView, nullptr, ico::Glyph(ico::IconId::ViewMenu), {}, {}, false, true});
}

void NavBar::SetPath(const std::wstring& path, const std::wstring& display) {
    model_.SetPath(path, display);
    UpdateLeadIcon();
    // "向上"由路径唯一推导（nav::CanNavigateUp，盘根 -> 此电脑）。
    // 后退/前进不在这里碰：它们由 SetNavState 提供，详见该函数注释。
    state_.canUp = nav::CanNavigateUp(model_.path());
    for (auto& b : buttons_) {
        if (b.id == kUp) b.enabled = state_.canUp;
    }
    Layout();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void NavBar::SetNavState(const NavState& st) {
    // canUp 以路径推导为准：外部传错不会让按钮与实际去向分叉
    // （"能不能上"只有一个判据，见 nav::UpTarget）。
    state_.canBack = st.canBack;
    state_.canForward = st.canForward;
    state_.canUp = nav::CanNavigateUp(model_.path());
    bool changed = false;
    for (auto& b : buttons_) {
        bool want = b.enabled;
        if (b.id == kBack) want = state_.canBack;
        else if (b.id == kForward) want = state_.canForward;
        else if (b.id == kUp) want = state_.canUp;
        if (want != b.enabled) {
            b.enabled = want;
            changed = true;
        }
    }
    // 按下态如果对应按钮刚好被禁用（例如后退后历史用尽），保留会让它一直画成
    // pressed —— 抬起事件可能已经落在别的地方了。
    if (changed && downBtn_) {
        for (const auto& b : buttons_)
            if (b.id == downBtn_ && !b.enabled) { downBtn_ = 0; break; }
    }
    if (changed && hwnd_) {
        // 提示文案带可用态（"（当前不可用）"），状态一变就要重建。
        RebuildTooltip();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void NavBar::UpdateLeadIcon() {
    if (leadIconPath_ == model_.path()) return;
    if (leadIcon_) {
        DestroyIcon(leadIcon_);
        leadIcon_ = nullptr;
    }
    leadIconPath_ = model_.path();
    if (leadIconPath_.empty()) return;
    // 统一图标识别：特殊命名空间（此电脑/回收站/侧边栏/云存储）取准确图标。
    leadIcon_ = ico::Resolve(leadIconPath_, true);
}

void NavBar::ReloadTheme() {
    if (!hwnd_) return;
    if (edit_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_->font), TRUE);
        win::ThemeControl(edit_, theme_->dark, L"CFD");
    }
    // 提示窗宽度按 DPI 缩放 → 重建（TooltipHost 析构会销毁旧窗）
    tooltip_ = ui::TooltipHost();
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void NavBar::RebuildTooltip() {
    if (!hwnd_) return;
    // 走唯一实现（TooltipHost）；宽度按 DPI 缩放，翻页/换盘符的长提示才不会被截断。
    tooltip_.Attach(hwnd_, theme_->S(420));
    struct Tip {
        int id;
        const wchar_t* label;
    };
    static const Tip kTips[] = {
        {kBack, L"后退 (Alt+←)"},
        {kForward, L"前进 (Alt+→)"},
        {kUp, L"向上一级 (Alt+↑)"},
        {kRefresh, L"刷新 (F5)"},
        {kView, L"视图与排序"},
    };
    std::vector<ui::TooltipHost::Item> items;
    for (const auto& t : kTips) {
        for (const auto& b : buttons_) {
            if (b.id != t.id) continue;
            // 灰掉的按钮也要提示 —— 但要说清"为什么点不动"。
            // 只提示动作名会让用户以为程序卡了（按钮画着禁用色却写着"后退"）。
            std::wstring text = t.label;
            if (!b.enabled) text += L"（当前不可用）";
            items.push_back({b.rc, text});
            break;
        }
    }
    tooltip_.SetTools(items);
}

void NavBar::FocusAddress() { BeginEdit(); }

LRESULT CALLBACK NavBar::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<NavBar>(h);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK NavBar::EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<NavBar>(GetParent(h));
    if (m == WM_KEYDOWN && self) {
        if (w == VK_RETURN) {
            self->CommitEdit();
            return 0;
        }
        if (w == VK_ESCAPE) {
            self->EndEdit(true);
            return 0;
        }
        if (w == VK_TAB) {
            // 有候选才吃 Tab；没有候选就退出编辑、把焦点还给视图，
            // 否则 Tab 在地址框里永远出不去（也插不进 \t，等于死键）。
            if (self->DoComplete()) return 0;
            self->EndEdit(true);
            return 0;
        }
    }
    if (m == WM_CHAR && (w == L'\r' || w == L'\t')) return 0;
    if (m == WM_KILLFOCUS && self && self->editing_) self->EndEdit();
    return CallWindowProcW(g_oldEditProc, h, m, w, l);
}

LRESULT CALLBACK NavBar::SearchProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<NavBar>(GetParent(h));
    if (m == WM_KEYDOWN && self) {
        if (w == VK_RETURN) {
            self->CommitSearch();
            return 0;
        }
        if (w == VK_ESCAPE) {
            // Esc：清掉搜索词并收起搜索框，把目录还原。
            // 不这么做的话"搜过一次"就再也没有回到原目录的路径了
            // （地址框显示的还是原路径，但列表是筛选后的结果）。
            self->ShowSearch(false);
            return 0;
        }
        if (w == VK_TAB) {
            self->ShowSearch(false);   // Tab 离开搜索，别把它变成死键
            return 0;
        }
    }
    if (m == WM_CHAR && (w == L'\r' || w == L'\t')) return 0;
    return CallWindowProcW(g_oldSearchProc, h, m, w, l);
}

void NavBar::SearchCreate() {
    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10,
                              10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!search_) return;
    SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_->font), TRUE);
    win::ThemeControl(search_, theme_->dark, L"CFD");
    // 提示文本用 EM_SETCUEBANNER：不占内容，用户一打字就消失（不是真值）。
    SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"搜索当前目录（回车执行，Esc 取消）"));
    // 子类化以接 Enter/Esc —— 与地址框同一套做法（原生 EDIT 不会替我们处理）。
    g_oldSearchProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(search_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchProc)));
    ShowWindow(search_, SW_HIDE);   // 默认不占位，Ctrl+F 才出现
}

void NavBar::LayoutSearch(int fieldRight, int fy, int fieldH) {
    if (!search_ || !searchVisible_) {
        if (search_) ShowWindow(search_, SW_HIDE);
        return;
    }
    // 搜索框占用 Layout 里给它留出的那一段（宽度计算在 Layout 一处完成，
    // 这里只负责摆放 —— 两边各算一次宽度必然对不上）。
    searchRc_ = {fieldRc_.right + theme_->S(kGap96), fy, fieldRight, fy + fieldH};
    ShowWindow(search_, SW_SHOW);
    const int pad = theme_->S(kFieldVPad96);
    int eW = searchRc_.right - searchRc_.left - theme_->S(8);
    int lineH = searchRc_.bottom - searchRc_.top - pad * 2;
    MoveWindow(search_, searchRc_.left + theme_->S(4), searchRc_.top + pad, eW > 0 ? eW : 0,
               lineH > 0 ? lineH : 0, TRUE);
}

void NavBar::ShowSearch(bool show) {
    if (!search_) return;
    if (!show) {
        SetWindowTextW(search_, L"");
        searchText_.clear();
        // 收起时清空搜索 → 目录还原。这是 Esc 的语义，也是"右上角没框了
        // 但列表还是筛选结果"这种鬼状态的唯一出路。
        if (onSearch_) onSearch_(std::wstring());
    }
    searchVisible_ = show;
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (show && search_) {
        SetFocus(search_);
        SendMessageW(search_, EM_SETSEL, 0, -1);
    }
}

void NavBar::FocusSearch() {
    if (!hwnd_) return;
    // 已有搜索词时保留它（用户按 Ctrl+F 多半是想改一下条件，不是从头来）。
    ShowSearch(true);
    if (search_ && !searchText_.empty()) {
        SetWindowTextW(search_, searchText_.c_str());
        SendMessageW(search_, EM_SETSEL, 0, -1);
    }
}

bool NavBar::IsSearching() const { return searchVisible_ && !searchText_.empty(); }

void NavBar::CommitSearch() {
    if (!search_) return;
    const int n = GetWindowTextLengthW(search_);
    std::wstring q(static_cast<size_t>(n) + 1, L'\0');
    if (n > 0) GetWindowTextW(search_, q.data(), n + 1);
    q.resize(static_cast<size_t>(n));
    searchText_ = str::Trim(q);
    if (onSearch_) onSearch_(searchText_);
    // 回车后焦点还给视图：搜索结果是文件列表，下一步操作（方向键/Delete）
    // 应该在列表上，而不是继续留在输入框里。
    if (parent_) PostMessageW(GetAncestor(hwnd_, GA_ROOT), WM_APP_REFOCUS, 0, 0);
}

LRESULT NavBar::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        Layout();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, theme_->text);
        SetBkColor(dc, theme_->editBg);
        return reinterpret_cast<LRESULT>(theme_->brEdit);
    }
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int btn = HitButton(x, y);
        int arrowId = 0;
        if (HitArrow(x, y, &arrowId)) btn = arrowId;
        bool more = HitMore(x, y);
        bool drop = HitDrop(x, y);
        int seg = HitSeg(x, y);
        int chev = HitChev(x, y);
        if (btn != hoverBtn_ || more != hotMore_ || drop != hotDrop_ || seg != hotSeg_ ||
            chev != hotChev_) {
            hoverBtn_ = btn;
            hotMore_ = more;
            hotDrop_ = drop;
            hotSeg_ = seg;
            hotChev_ = chev;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        // 动作在这里执行（DOWN 只画按下态）。抬起点必须仍落在同一颗按钮上 ——
        // 按下后拖出去再松手 = 取消，这是所有按钮控件的标准语义。
        const int btn = downBtn_;
        downBtn_ = 0;
        // 先清状态再释放捕获：ReleaseCapture 会**同步**派发 WM_CAPTURECHANGED，
        // 那里的复位逻辑看到 downBtn_ 已为 0 就不会再动一次。
        if (GetCapture() == h) ReleaseCapture();
        if (!btn) return 0;
        InvalidateRect(h, nullptr, FALSE);

        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (HitButton(x, y) != btn) return 0;   // 拖出去了：取消

        const Btn* b = nullptr;
        for (const auto& it : buttons_)
            if (it.id == btn) b = &it;
        if (!b || !b->enabled) return 0;

        POINT pt{x, y};
        ClientToScreen(h, &pt);
        if (btn == kBack && onBack_) onBack_();
        else if (btn == kForward && onForward_) onForward_();
        else if (btn == kUp && onUp_) onUp_();
        else if (btn == kRefresh && onRefresh_) onRefresh_();
        else if (btn == kView) ShowViewMenu(pt);
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        hoverBtn_ = 0;
        // 按住按钮把光标拖出栏外时**不清 downBtn_**：抬起点是否还在按钮上由
        // WM_LBUTTONUP 判定（不在就是取消）。这里清掉会让"拖出去再拖回来"丢状态。
        hotMore_ = hotDrop_ = false;
        hotSeg_ = hotChev_ = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        // 捕获被抢走（弹层/切窗口）：按下态必须复位，否则残留的 downBtn_
        // 会让下一次无关的抬起触发一次导航。
        if (downBtn_) {
            downBtn_ = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);

        const int target = NavTargetAt(x, y);
        if (sink_ && sink_(target, m, MBTN_LEFT, CurrentMods(), pt)) return 0;

        int arrowId = 0;
        if (HitArrow(x, y, &arrowId)) {
            ShowHistoryMenu(arrowId == kBack, pt);
            return 0;
        }
        int btn = HitButton(x, y);
        if (btn) {
            // 只记"按下了哪一颗"，动作留到 WM_LBUTTONUP 再执行。
            //
            // 【为什么必须改成 UP 触发】本工程其余所有控件（标签、书签、状态栏段）
            // 都在 UP 里执行动作，弹层的"点别处只关菜单、不触发那个控件"也是
            // 靠这一点成立的。导航按钮原来是 DOWN 触发 —— 于是菜单开着时点它，
            // 会"关菜单 + 立刻导航"两件事一起发生，而用户的本意通常只是关菜单。
            // UP 触发还顺手拿到"按下后拖出去 = 取消"这个标准行为。
            downBtn_ = btn;
            // 捕获：按下后把光标拖出栏外再拖回来，抬起时仍能正确判定
            // （不捕获的话 WM_MOUSELEAVE 会清掉 downBtn_，拖回来时按钮已经"松开"了，
            // 而光标下看起来还是按着的 —— 典型的手感 bug）。
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (HitMore(x, y)) {
            ShowFoldMenu(pt);
            return 0;
        }
        if (HitDrop(x, y)) {
            ShowDropMenu(pt);
            return 0;
        }
        int chev = HitChev(x, y);
        if (chev >= 0) {
            ShowChildMenu(chev, pt);
            return 0;
        }
        int seg = HitSeg(x, y);
        if (seg >= 0) {
            if (onNavigate_) onNavigate_(model_.parts()[static_cast<size_t>(seg)].path);
            return 0;
        }
        if (x >= fieldRc_.left) BeginEdit();
        return 0;
    }
    case WM_RBUTTONUP: {
        const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        if (sink_ && sink_(NavTargetAt(x, y), m, MBTN_RIGHT, CurrentMods(), pt)) return 0;

        int btn = HitButton(x, y);
        if (btn == kBack || btn == kForward) {
            ShowHistoryMenu(btn == kBack, pt);
            return 0;
        }
        // 面包屑段右键：作用于"右键的那一段"，不是当前目录。
        // 以前这里什么都不做 —— 用户想「复制 D:\ 的路径」只能先导航过去。
        // Opus / TC / XYplorer 的面包屑右键都是段级菜单。
        const int seg = HitSeg(x, y);
        if (seg >= 0) {
            ShowSegmentMenu(seg, pt);
            return 0;
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {
        const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        if (sink_ && sink_(NavTargetAt(x, y), m, MBTN_MIDDLE, CurrentMods(), pt)) return 0;
        return 0;
    }
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        // 侧键在导航栏同样可达：前进/后退规则落在 MT_NAV（按钮区）与
        // MT_ADDRESS（地址/面包屑区）等 target 上，与文件视图同口径。
        const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        const int button = XButtonFromWParam(w);
        if (button < 0) return 0;
        if (sink_ && sink_(NavTargetAt(x, y), m, button, CurrentMods(), pt)) return 0;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void NavBar::Layout() {
    if (!hwnd_ || !theme_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);

    const int btn = theme_->S(kBtn96);
    const int gap = theme_->S(kGap96);
    int x = theme_->S(kX96);
    int by = (rc.bottom - btn) / 2;
    for (auto& b : buttons_) {
        b.rc = {x, by, x + btn, by + btn};
        if (b.hasArrow) b.arrow = {b.rc.right - theme_->S(kArrow96), b.rc.top, b.rc.right, b.rc.bottom};
        x = b.rc.right + gap;
    }

    // 文字行高按栏字体实测（单行 EDIT / 面包屑同源）。
    int lineH = theme_->S(18);
    if (HDC dc = GetDC(hwnd_)) {
        HGDIOBJ old = SelectObject(dc, theme_->font);
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0) lineH = tm.tmHeight + 2;
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
    }

    // 地址框边框高度贴合文字：行高 + 上下各 kFieldVPad。不再等于按钮边长,
    // 否则 24px 框装 15px 字 → 上下各留 ~4.5px,视觉上"边框比字大一圈"。
    const int fieldH = lineH + theme_->S(kFieldVPad96) * 2;
    const int cy = rc.bottom / 2;                 // 按钮与字段共用中心线
    const int fy = cy - fieldH / 2;
    const int fieldLeft = (buttons_.empty() ? theme_->S(kX96) : buttons_.back().rc.right) + gap +
                          theme_->S(2);
    const int fieldRight = rc.right - theme_->S(8);

    // 搜索框（显示中）从右侧切一段走，地址框让出宽度。
    // 顺序很关键：先算搜索框、再算地址框，否则地址框的右缘会盖在搜索框下面
    // （EDIT 子窗口的 z 序不保证，看起来像"地址框压住了搜索框一半"）。
    const int searchW = (search_ && searchVisible_)
                            ? std::max(theme_->S(160), (fieldRight - fieldLeft) / 3 +
                                                           theme_->S(kGap96))
                            : 0;
    const int addrRight = fieldRight - (searchW ? searchW + theme_->S(kGap96) : 0);
    fieldRc_ = {fieldLeft, fy, addrRight, fy + fieldH};
    dropRc_ = {fieldRc_.right - theme_->S(kPad96) - theme_->S(kDrop96), fieldRc_.top,
               fieldRc_.right - theme_->S(kPad96), fieldRc_.bottom};
    LayoutSearch(fieldRight, fy, fieldH);

    // EDIT 只占一行高并垂直居中（单行 EDIT 拉满整框会把文字贴顶）
    if (edit_) {
        int eW = fieldRc_.right - fieldRc_.left - theme_->S(kDrop96) - theme_->S(kPad96) - 1;
        int eTop = fieldRc_.top + (fieldH - lineH) / 2;
        MoveWindow(edit_, fieldRc_.left + 1, eTop, eW > 0 ? eW : 0, lineH, TRUE);
    }

    segRects_.clear();
    chevRects_.clear();
    // 段矩形重建后，旧的悬停段号可能指向不存在的段（或多段折叠后指错），先清掉。
    hotSeg_ = -1;
    hotChev_ = -1;
    firstShown_ = 0;
    moreRc_ = {fieldRc_.left + theme_->S(kLpad96) + theme_->S(kLead96), fieldRc_.top,
               fieldRc_.left + theme_->S(kLpad96) + theme_->S(kLead96) + theme_->S(kSep96),
               fieldRc_.bottom};
    if (editing_) return;

    const auto& parts = model_.parts();
    if (parts.empty()) return;

    // 量字走唯一原语（ui::MeasureText 内部选字体并还原）
    HDC dc = GetDC(hwnd_);
    const int sepW = theme_->S(kSep96);
    std::vector<int> widths(parts.size(), 0);
    int total = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        widths[i] = ui::MeasureText(dc, theme_->font, parts[i].label) + sepW;
        total += widths[i];
    }
    const int startX = moreRc_.right;
    const int avail = dropRc_.left - startX;
    while (total > avail && firstShown_ + 1 < static_cast<int>(parts.size())) {
        total -= widths[static_cast<size_t>(firstShown_)];
        ++firstShown_;
    }

    int cx = startX;
    const int clipR = dropRc_.left;
    // firstShown_ 恒 >= 0（只在上面的循环里 ++），转到 size_t 是明确的非负转换。
    for (size_t i = static_cast<size_t>(firstShown_); i < parts.size(); ++i) {
        if (cx >= clipR) break;
        int labelW = widths[i] - sepW;
        if (cx + labelW > clipR) labelW = clipR - cx;
        segRects_.push_back({cx, fieldRc_.top, cx + labelW, fieldRc_.bottom});
        chevRects_.push_back({cx + labelW, fieldRc_.top, cx + widths[i], fieldRc_.bottom});
        cx += widths[i];
    }

    ReleaseDC(hwnd_, dc);
    // 按钮矩形刚刚重算：提示热区必须跟着走，否则窗口缩放后气泡落在旧位置
    // （表现是"鼠标停在按钮上没提示，停在旁边的空白处却提示"）。
    RebuildTooltip();
}

int NavBar::HitButton(int x, int y) const {
    POINT pt{x, y};
    for (const auto& b : buttons_)
        if (PtInRect(&b.rc, pt)) return b.id;
    return 0;
}

bool NavBar::HitArrow(int x, int y, int* id) const {
    POINT pt{x, y};
    for (const auto& b : buttons_) {
        if (b.hasArrow && PtInRect(&b.arrow, pt)) {
            *id = b.id;
            return true;
        }
    }
    return false;
}

bool NavBar::HitMore(int x, int y) const {
    POINT pt{x, y};
    return moreRc_.right > moreRc_.left && PtInRect(&moreRc_, pt);
}

bool NavBar::HitDrop(int x, int y) const {
    POINT pt{x, y};
    return PtInRect(&dropRc_, pt) != FALSE;
}

int NavBar::HitSeg(int x, int y) const {
    POINT pt{x, y};
    for (size_t k = 0; k < segRects_.size(); ++k)
        if (PtInRect(&segRects_[k], pt)) return firstShown_ + static_cast<int>(k);
    return -1;
}

int NavBar::NavTargetAt(int x, int y) const {
    int arrowId = 0;
    if (HitArrow(x, y, &arrowId) || HitButton(x, y) != 0 || HitMore(x, y) || HitDrop(x, y))
        return MT_NAV;
    if (HitSeg(x, y) >= 0 || HitChev(x, y) >= 0) return MT_ADDRESS;
    if (x >= fieldRc_.left) return MT_ADDRESS;
    return MT_NAV;
}

int NavBar::HitChev(int x, int y) const {
    POINT pt{x, y};
    for (size_t k = 0; k < chevRects_.size(); ++k)
        if (PtInRect(&chevRects_[k], pt)) return firstShown_ + static_cast<int>(k);
    return -1;
}
