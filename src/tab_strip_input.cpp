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
        RebuildTooltip(h);   // 宿主宽度随窗口变，换行宽度跟着变
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
                const int target = DropTargetAt(x, y);
                if (target != dropIdx_) {
                    dropIdx_ = target;
                    InvalidateRect(h, nullptr, FALSE);
                }
                // 边缘自动滚动：仅在溢出时有意义（不溢出时没有可滚内容）。
                // 进入左/右边缘带就按对应方向滚动，离开就停。带宽与步长都过 S()。
                if (overflow_) {
                    const int edge = theme_->S(28);
                    const int leftEdge = theme_->S(6) + edge;
                    const int rightEdge = tabViewRight_ - edge;
                    UpdateAutoScroll(x, x < leftEdge ? -theme_->S(24)
                                        : (x > rightEdge ? theme_->S(24) : 0));
                } else {
                    StopAutoScroll();
                }
            }
        }
        UpdateHover(h, x, y);
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        // 鼠标离开标签栏 → 快速关闭模式结束，恢复正常宽度分配。
        CancelRapidClose();
        if (hoverKind_ != kNone) {
            hoverKind_ = kNone;
            hoverIndex_ = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        // 上一次双击的尾随 UP 可能落到了窗口外（点空白时没有 capture），
        // 新的一次按下必须先清掉旧标记，否则会把新单击误吞。
        suppressDblUp_ = false;
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        int tab = HitTab(x, y);
        int close = HitClose(x, y);
        // DOWN/UP/DBLCLK 必须报同一个 target。以前这里漏了 new/overflow：
        // 点在 + 上 DOWN 报 empty，命中"双击空白 -> 上一级"的规则并被 claim；
        // UP 报 new_tab 却被 InputRouter 因为 claim 吞掉，于是单击毫无反应。
        int target = MouseTargetAt(x, y);
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
    case WM_CAPTURECHANGED:
        // 捕获被系统抢走（弹层弹出、切窗口、别的控件 SetCapture）：
        // 拖拽状态必须在这里复位，否则残留下次点击。
        //
        // 真实的坑：按住标签拖到一半，右键弹菜单（或 Alt+Tab 切走），
        // 捕获被夺 → 我们收不到 LBUTTONUP → dragIdx_/dragging_ 一直是"正在拖"。
        // 下一次鼠标移进标签栏（哪怕没按键）就会走进拖拽分支，标签跟着光标乱跳。
        // BookmarkBar 早就有这个分支，标签栏一直漏着。
        // 捕获已失，不再 ReleaseCapture（GetCapture() 已不是本窗口）。
        CancelDrag(h, false);
        return 0;
    case WM_TIMER:
        if (w == kDragScrollTimer && dragging_ && dragScrollStep_ != 0 && overflow_) {
            // 缓存鼠标 x 没有配对的 y：边缘滚动只改横向位置，y 用拖拽起点即可
            // （DropTargetAt 的 y 只用于 HitTab 判断落在哪一行，标签栏是单行）。
            ScrollBy(dragScrollStep_);
            const int target = DropTargetAt(dragMouseX_, dragStart_.y);
            if (target != dropIdx_) {
                dropIdx_ = target;
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_KEYDOWN:
        // Esc 取消拖拽：拖到一半改主意，松手就会真的移位 —— 给一条反悔路径。
        // 只有真在拖（dragIdx_>=0）时才吞掉 Esc；否则让它继续传给对话框等。
        if (w == VK_ESCAPE && (dragging_ || dragIdx_ >= 0)) {
            CancelDrag(h, true);   // ReleaseCapture 会再派一次 CAPTURECHANGED，幂等
            return 0;
        }
        break;
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        // 双击的第二次抬起只是尾巴，不是新的单击。必须先吞掉再分发给 sink_，
        // 否则用户给标签配的鼠标规则会在尾巴 UP 上再触发一次动作。
        if (suppressDblUp_) {
            suppressDblUp_ = false;
            return 0;
        }
        const bool claimed = sink_ && sink_(MouseTargetAt(x, y), m, MBTN_LEFT, CurrentMods(), pt);
        if (claimed) return 0;

        int close = HitClose(x, y);
        int tab = HitTab(x, y);
        if (dragIdx_ >= 0) {
            // 顺序不能反：ReleaseCapture 会**同步**派发 WM_CAPTURECHANGED，
            // 而那个分支（见上）会把 dragIdx_/dropIdx_ 清成 -1。先释放再读，
            // 读到的永远是 -1 —— 拖拽排序从此静默失效，且没有任何报错。
            // （BookmarkBar 早就踩过同一个坑，注释就写在那里。）
            const int from = dragIdx_;
            const int to = dropIdx_;
            const bool moved = dragging_ && to >= 0 && to != from;
            StopAutoScroll();   // 拖到边缘滚着松手：先停表，再提交移动
            dragIdx_ = -1;
            dragging_ = false;
            dropIdx_ = -1;
            ReleaseCapture();
            if (moved) PostMessageW(parent_, WM_APP_TAB_MOVED, static_cast<WPARAM>(from),
                                    static_cast<LPARAM>(to));
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
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        // 侧键前进/后退：标签栏自己没有默认行为，全部交给鼠标规则。
        // target/button/mods/坐标四元组必须与文件视图、其它栏同口径
        // （shell_input::ButtonFrom 与 XButtonFromWParam 解析一致），
        // 否则规则引擎的 target 精确匹配会落空，侧键在标签栏静默失效。
        const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        const int button = XButtonFromWParam(w);
        if (button < 0) return 0;
        if (sink_ && sink_(MouseTargetAt(x, y), m, button, CurrentMods(), pt)) return 0;
        return 0;
    }
    case WM_CONTEXTMENU: {
        // Shift+F10 / VK_APPS 走这个消息（lParam = -1,-1），右键走 WM_RBUTTONUP。
        // Mozilla 的无障碍验收要求：每个图标功能都要有键盘可达路径。
        const int sx = GET_X_LPARAM(l), sy = GET_Y_LPARAM(l);
        POINT at{sx, sy};
        int tab = -1;
        if (sx != -1 && sy != -1) {
            POINT client = at;
            ScreenToClient(h, &client);
            tab = HitTab(client.x, client.y);
        }
        if (tab < 0) tab = active_;   // 键盘触发时落在激活标签上
        if (tab < 0 || tab >= static_cast<int>(tabRects_.size())) return 0;
        if (sx == -1 && sy == -1) {
            const RECT& r = tabRects_[static_cast<size_t>(tab)];
            at = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(h, &at);
        }
        ShowTabMenu(tab, at);
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
        // 双击必带一次尾随 UP（DOWN → UP → DBLCLK → UP）。那次 UP 不能当新单击：
        // 它会在 CLOSE 消息之后发出 SELECT(旧下标)，把"顶上来"的标签误选中。
        suppressDblUp_ = true;
        // 落在关闭钮上：**什么都不做**。
        //
        // 这里曾写 NotifyClose(close)，是"双击关闭钮关掉两个标签"的成因：
        // 一次双击的消息序列是 DOWN → UP → DBLCLK → UP。UP 那一下已经关掉了标签 A，
        // 标签随之左移；DBLCLK 到达时 HitClose 按**新布局**命中 —— 而新标签的
        // 关闭钮恰好落在鼠标下（快速关闭模式就是刻意保住这个位置的），
        // 于是第二下又把标签 B 关掉。用户只双击了一次，却少了两个标签。
        //
        // Chrome / VS Code 对关闭钮的双击都是"就一次"：关闭钮不是可双击的控件。
        if (HitClose(x, y) >= 0) return 0;
        int tab = HitTab(x, y);
        if (tab >= 0) {
            // 双击标签正文 = 关闭该标签（Chrome / Edge / VS Code 的惯例）。
            // 与关闭钮不同：正文是可双击的控件，这里才真正执行关闭。
            //
            // 顺序要点：
            //   · sink 优先 —— 鼠标规则可以把某个标签的双击绑成别的动作，
            //     吞掉了就不再走默认关闭（与其它输入的契约一致）。
            //   · 锁定标签不关 —— 与中键关闭（见 WM_MBUTTONDOWN）同一口径。
            //     双击是"好心"的快捷方式，不该绕开"锁定=关不掉"的硬约束。
            //   · 不碰 dragIdx_ —— DBLCLK 后还有一次 UP，它由 suppressDblUp_
            //     吞掉；在这里清反而会让误触发的默认行为漏过去。
            POINT pt{x, y};
            ClientToScreen(h, &pt);
            if (sink_ && sink_(MT_TAB, m, MBTN_LEFT, CurrentMods(), pt)) return 0;
            if (!items_[static_cast<size_t>(tab)].locked) NotifyClose(tab);
            return 0;
        }
        // 空白/新增/溢出区：只把命中交给规则引擎，不再内置"双击空白 = 新建标签"。
        // 该默认行为没有快捷键提示、与"+"按钮语义重复，还会让人误以为双击标签能开新页。
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        if (sink_ && sink_(MouseTargetAt(x, y), m, MBTN_LEFT, CurrentMods(), pt)) return 0;
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
    // 裁切线用 tabViewRight_：+ 已移到标签右侧，»/+ 都在它右边，不属于任何标签。
    const int clipRight = overflow_ ? tabViewRight_ : 32767;
    // 横坐标落在裁切线右侧时一律不属于任何标签（判据与理由见 TabHitInView）。
    if (!TabHitInView(x, clipRight)) return -1;
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

int TabStrip::MouseTargetAt(int x, int y) const {
    // 纯函数定优先级，这里只负责把四个命中结果喂进去。
    // 三处消息共用同一口径，避免"按下报 empty、抬起报 new_tab"导致
    // InputRouter 的 claim 吞掉默认行为。
    return TabMouseTarget(HitClose(x, y) >= 0, HitTab(x, y) >= 0, HitNew(x, y),
                          HitOverflow(x, y));
}
