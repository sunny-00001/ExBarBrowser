#include "nav_bar.h"

#include "platform_win.h"

#include "ui_draw.h"
#include "icon_resolve.h"
#include "mouse_rules.h"
#include "nav_metrics.h"

namespace {

WNDPROC g_oldEditProc = nullptr;

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
    Layout();
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
    // 可用态与命令前置检查（Req::CanGoUp）读同一个函数：
    // 按钮灰显 == "点不动"，绝不允许"亮着但没反应"。
    for (auto& b : buttons_)
        if (b.id == kUp) b.enabled = nav::CanNavigateUp(model_.path());
    Layout();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
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
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
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
    case WM_LBUTTONUP:
        if (downBtn_) {
            downBtn_ = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        hoverBtn_ = 0;
        if (downBtn_) {
            downBtn_ = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        hotMore_ = hotDrop_ = false;
        hotSeg_ = hotChev_ = -1;
        InvalidateRect(h, nullptr, FALSE);
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
            downBtn_ = btn;  // 按下反馈；动作仍在 down 触发（与 mod 的即时响应一致）
            InvalidateRect(h, nullptr, FALSE);
            const Btn* b = nullptr;
            for (const auto& it : buttons_)
                if (it.id == btn) b = &it;
            if (b && b->enabled) {
                if (btn == kBack && onBack_) onBack_();
                else if (btn == kForward && onForward_) onForward_();
                else if (btn == kUp && onUp_) onUp_();
                else if (btn == kRefresh && onRefresh_) onRefresh_();
                else if (btn == kView) ShowViewMenu(pt);
            }
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
            if (onNavigate_) onNavigate_(model_.parts()[seg].path);
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
        if (btn == kBack || btn == kForward) ShowHistoryMenu(btn == kBack, pt);
        return 0;
    }
    case WM_MBUTTONDOWN: {
        const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        if (sink_ && sink_(NavTargetAt(x, y), m, MBTN_MIDDLE, CurrentMods(), pt)) return 0;
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
    fieldRc_ = {fieldLeft, fy, fieldRight, fy + fieldH};
    dropRc_ = {fieldRc_.right - theme_->S(kPad96) - theme_->S(kDrop96), fieldRc_.top,
               fieldRc_.right - theme_->S(kPad96), fieldRc_.bottom};

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
        total -= widths[firstShown_];
        ++firstShown_;
    }

    int cx = startX;
    const int clipR = dropRc_.left;
    for (size_t i = firstShown_; i < parts.size(); ++i) {
        if (cx >= clipR) break;
        int labelW = widths[i] - sepW;
        if (cx + labelW > clipR) labelW = clipR - cx;
        segRects_.push_back({cx, fieldRc_.top, cx + labelW, fieldRc_.bottom});
        chevRects_.push_back({cx + labelW, fieldRc_.top, cx + widths[i], fieldRc_.bottom});
        cx += widths[i];
    }

    ReleaseDC(hwnd_, dc);
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
