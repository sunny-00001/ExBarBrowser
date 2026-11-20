// 标签栏的输入端：消息分发、悬停态、命中测试。
//
// 只回答"点到了谁"，不改标签状态：选中/关闭/移动一律 PostMessage 给 App。
#include "tab_strip.h"

#include "ui_draw.h"
#include "mouse_rules.h"

LRESULT TabStrip::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
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
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (dragIdx_ >= 0 && (w & MK_LBUTTON)) {
            if (!dragging_ && (abs(x - dragStart_.x) > theme_->S(4) ||
                               abs(y - dragStart_.y) > theme_->S(4))) {
                dragging_ = true;
                SetCapture(h);
            }
            if (dragging_) {
                int target = HitTab(x, y);
                if (target < 0) {
                    // 落在标签之间：按 x 推断插入位置
                    target = static_cast<int>(tabRects_.size()) - 1;
                    for (size_t i = 0; i < tabRects_.size(); ++i) {
                        if (x < (tabRects_[i].left + tabRects_[i].right) / 2) {
                            target = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (target != dropIdx_) {
                    dropIdx_ = target;
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
        }
        UpdateHover(h, x, y);
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hoverKind_ != kNone) {
            hoverKind_ = kNone;
            hoverIndex_ = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        int tab = HitTab(x, y);
        int close = HitClose(x, y);
        int target = close >= 0 ? MT_TAB_CLOSE : (tab >= 0 ? MT_TAB : MT_EMPTY);
        closeDownIdx_ = -1;  // 每次按下先复位，避免上次残留索引影响本次判定
        if (sink_ && sink_(target, m, MBTN_LEFT, CurrentMods(), pt)) return 0;

        if (close >= 0) {
            closeDownIdx_ = close;  // 锁定按下时的关闭按钮，up 时只认这个索引
            return 0;               // 关闭在 up 时判定（避免按下即关）
        }
        if (tab >= 0) {
            dragIdx_ = tab;
            dragStart_ = {x, y};
            dropIdx_ = tab;
            SetCapture(h);
            // 焦点留在标签栏：作用域为"标签栏"的裸键（Shift+←/→/Home/End）才收得到。
            // App::Activate 只在焦点原本位于视图区时才把焦点搬走。
            SetFocus(h);
            NotifySelect(tab);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        int close = HitClose(x, y);
        int tab = HitTab(x, y);
        int target = close >= 0 ? MT_TAB_CLOSE
                                : (tab >= 0 ? MT_TAB : (HitNew(x, y) ? MT_NEW_TAB
                                                                    : (HitOverflow(x, y) ? MT_OVERFLOW
                                                                                         : MT_EMPTY)));
        if (sink_ && sink_(target, m, MBTN_LEFT, CurrentMods(), pt)) return 0;

        if (dragIdx_ >= 0) {
            ReleaseCapture();
            if (dragging_ && dropIdx_ >= 0 && dropIdx_ != dragIdx_)
                PostMessageW(parent_, WM_APP_TAB_MOVED, static_cast<WPARAM>(dragIdx_),
                             static_cast<LPARAM>(dropIdx_));
            dragIdx_ = -1;
            dragging_ = false;
            dropIdx_ = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        // 只认按下时锁定的那个关闭按钮：按 A 的关闭钮、松在 B 的关闭钮上不会误关 B。
        if (close >= 0 && close == closeDownIdx_) {
            closeDownIdx_ = -1;
            NotifyClose(close);
            return 0;
        }
        closeDownIdx_ = -1;
        if (tab >= 0) {
            NotifySelect(tab);
            return 0;
        }
        if (HitNew(x, y)) {
            PostMessageW(parent_, WM_APP_TAB_NEW, 0, 0);
            return 0;
        }
        if (HitOverflow(x, y)) {
            ShowOverflowMenu(pt);
            return 0;
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        int tab = HitTab(x, y);
        if (sink_ && sink_(tab >= 0 ? MT_TAB : MT_EMPTY, m, MBTN_MIDDLE, CurrentMods(), pt))
            return 0;
        if (tab >= 0 && !items_[static_cast<size_t>(tab)].locked) NotifyClose(tab);
        return 0;
    }
    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        int tab = HitTab(x, y);
        if (sink_ && sink_(tab >= 0 ? MT_TAB : MT_EMPTY, m, MBTN_RIGHT, CurrentMods(), pt)) return 0;
        if (tab >= 0) ShowTabMenu(tab, pt);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        // 落在关闭钮上就是"关闭"，不能顺便再开一个新标签（原来 dblclk 只判 HitTab）。
        int close = HitClose(x, y);
        if (close >= 0) {
            NotifyClose(close);
            return 0;
        }
        if (HitTab(x, y) < 0 && !HitNew(x, y) && !HitOverflow(x, y)) {
            POINT pt{x, y};
            ClientToScreen(h, &pt);
            if (sink_ && sink_(MT_EMPTY, m, MBTN_LEFT, CurrentMods(), pt)) return 0;
            PostMessageW(parent_, WM_APP_TAB_NEW, 0, 0);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        // 滚轮消息的坐标是屏幕坐标；sink 的契约也是屏幕坐标，别再 ScreenToClient 后传出去
        // （曾经传了客户区坐标，鼠标规则拿到的位置是错的）。
        POINT screen{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        POINT pt = screen;
        ScreenToClient(h, &pt);
        int target = HitTab(pt.x, pt.y) >= 0 ? MT_TAB : MT_EMPTY;
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (sink_ && sink_(target, m, delta, CurrentMods(), screen)) return 0;
        if (overflow_) {
            ScrollBy(delta > 0 ? -theme_->S(48) : theme_->S(48));
            return 0;
        }
        if (items_.size() > 1) {
            int next = active_ + (delta > 0 ? -1 : 1);
            if (next < 0) next = static_cast<int>(items_.size()) - 1;
            if (next >= static_cast<int>(items_.size())) next = 0;
            NotifySelect(next);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void TabStrip::UpdateHover(HWND h, int x, int y) {
    if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        tracking_ = true;
    }

    Hover kind = kNone;
    int index = -1;
    int close = HitClose(x, y);
    int tab = HitTab(x, y);
    if (close >= 0 && tab >= 0) {
        kind = kClose;
        index = close;
    } else if (tab >= 0) {
        kind = kTab;
        index = tab;
    } else if (HitNew(x, y)) {
        kind = kNew;
    } else if (HitOverflow(x, y)) {
        kind = kOverflow;
    }

    if (kind != hoverKind_ || index != hoverIndex_) {
        hoverKind_ = kind;
        hoverIndex_ = index;
        InvalidateRect(h, nullptr, FALSE);
    }
    (void)0;
}

int TabStrip::HitTab(int x, int y) const {
    POINT pt{x, y};
    int clipRight = overflow_ ? ovfRc_.left - theme_->S(2) : 32767;
    for (size_t i = 0; i < tabRects_.size(); ++i) {
        if (tabRects_[i].right <= 0 || tabRects_[i].left >= clipRight) continue;
        if (PtInRect(&tabRects_[i], pt)) return static_cast<int>(i);
    }
    return -1;
}

int TabStrip::HitClose(int x, int y) const {
    POINT pt{x, y};
    int tab = HitTab(x, y);
    if (tab < 0 || !CloseVisible(tab)) return -1;
    return PtInRect(&closeRects_[static_cast<size_t>(tab)], pt) ? tab : -1;
}

bool TabStrip::CloseVisible(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return false;
    if (items_[static_cast<size_t>(index)].locked) return false;
    return index == active_ || (hoverKind_ == kTab && hoverIndex_ == index) ||
           (hoverKind_ == kClose && hoverIndex_ == index);
}

bool TabStrip::HitNew(int x, int y) const {
    POINT pt{x, y};
    return PtInRect(&newRect_, pt) != FALSE;
}

bool TabStrip::HitOverflow(int x, int y) const {
    if (!overflow_) return false;
    POINT pt{x, y};
    return PtInRect(&ovfRc_, pt) != FALSE;
}
