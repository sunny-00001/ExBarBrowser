// 导航栏输入状态机：窗口过程、地址框子类、鼠标捕获与键盘导航。
//
// 【为什么单独成文件】这里保存的是"按下→抬起→提交"的时序状态：downBtn_、
// tracking_、edit_ 子类回调、搜索框两条带的热区。它们与生命周期/几何无关，
// 混在生命周期文件里，改 Create 时极易顺手改到捕获顺序。
//
// 【捕获顺序不可改】WM_LBUTTONDOWN 只记 downBtn_ 并 SetCapture；动作在
// WM_LBUTTONUP 里执行，且抬起点必须仍落在同一颗按钮上。先清 downBtn_ 再
// ReleaseCapture，因为 ReleaseCapture 会同步派发 WM_CAPTURECHANGED。
// 这条时序错了就是"关弹层顺带导航"或"拖出栏外松手仍然导航"。
#include "nav_bar.h"

#include "mouse_rules.h"
#include "platform_win.h"

namespace {

WNDPROC g_oldEditProc = nullptr;

}  // namespace

void NavBar::InstallEditSubclass(HWND edit) {
    g_oldEditProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
    // 系统自动补全（目录优先）：与 mod 的 SHACF_FILESYS_DIRS 同口径。
    // 只补全、不自动追加，避免把用户没确认的路径吞进去。
    SHAutoComplete(edit, SHACF_FILESYSTEM | SHACF_FILESYS_DIRS | SHACF_AUTOAPPEND_FORCE_OFF);
}

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
        // 搜索框的两条带（左侧=范围、右侧=历史）。它们画在搜索框里但**不是**
        // EDIT 的一部分，点击落在编辑控件上时根本到不了这里 ——
        // 所以命中判定只对这两条带有效。
        bool sm = HitSearchMenu(x, y);
        bool sh = HitSearchHist(x, y);
        if (btn != hoverBtn_ || more != hotMore_ || drop != hotDrop_ || seg != hotSeg_ ||
            chev != hotChev_ || sm != hotSearchMenu_ || sh != hotSearchHist_) {
            hoverBtn_ = btn;
            hotMore_ = more;
            hotDrop_ = drop;
            hotSeg_ = seg;
            hotChev_ = chev;
            hotSearchMenu_ = sm;
            hotSearchHist_ = sh;
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
        hotSearchMenu_ = hotSearchHist_ = false;
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
        // 搜索框的两条带。必须排在 `x >= fieldRc_.left` 之前 ——
        // 搜索框在地址框右边，不先判掉就会被当成"点地址框"（去编辑地址）。
        if (HitSearchMenu(x, y)) {
            ShowSearchScopeMenu();
            return 0;
        }
        if (HitSearchHist(x, y)) {
            ShowSearchHistoryMenu();
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
