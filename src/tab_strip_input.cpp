// 标签栏的输入端：消息分发、悬停态、命中测试。
//
// 只回答"点到了谁"，不改标签状态：选中/关闭/移动一律 PostMessage 给 App。
#include "tab_strip.h"

#include "tab_uia.h"

#include "config.h"      // tabTearOffDy / tabAnimMs（当帧读，改完即生效）
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
                // 拖动期间禁 tooltip：鼠标停在标签上时提示框会盖住插入线/浮层。
                // 松手（HandleDragCommit）与取消（CancelDrag）都会重建恢复。
                tooltip_.Clear();
            }
            if (dragging_) {
                // 浮层跟手：坐标每帧更新（自动滚动时也照样跟，视觉上才对）。
                dragMouse_ = {x, y};
                // tear-off 判定（Chromium kVerticalDetachMagnetism）：垂直方向
                // **上或下**离开栏体超过 tabTearOffDy 才算脱离。脱离期间不做
                // 插入位置计算 —— 松手是"移到新窗口"，此时算 dropIdx_ 只会让
                // 插入线闪一下。
                const int dy = y;
                const bool tear =
                    TabIsTearOff(dy, clientH_, theme_->S(Config::Get().s().tabTearOffDy));
                if (tear != tearOff_) {
                    tearOff_ = tear;
                    // 脱离瞬间收掉边缘滚动：继续滚会让浮层与目标窗口位置脱节。
                    if (tear) StopAutoScroll();
                    InvalidateRect(h, nullptr, FALSE);
                }
                if (tearOff_) {
                    UpdateHover(h, x, y);
                    return 0;
                }
                const int target = DropTargetAt(x, y);
                if (target != dropIdx_) {
                    dropIdx_ = target;
                    InvalidateRect(h, nullptr, FALSE);
                }
                // 边缘自动滚动：仅在溢出时有意义（不溢出时没有可滚内容）。
                // 边缘带与起停判定在 UpdateAutoScroll 内（edge::Update 共享实现）。
                if (overflow_)
                    UpdateAutoScroll(x, theme_->S(24));
                else
                    StopAutoScroll();
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
    case WM_SETCURSOR:
        // 拖动中给四向箭头反馈：系统默认箭头看不出"已进入拖拽"。只改视觉，
        // 不碰输入（R21 禁的是合成鼠标/键盘，光标外观不是输入）。
        if (dragging_) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
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
        if (sink_ && sink_(target, m, MBTN_LEFT, CurrentMods(), pt)) {
            // 规则接管了这次按下：显式复位拖拽瞬态。与 BookmarkBar::WM_LBUTTONDOWN
            // 里 `dragArm_ = false` 是同一件事 —— 若上一次拖拽经异常路径残留
            // （捕获被夺、Esc 没送达），下一次 MOUSEMOVE 会直接误入拖拽分支，
            // 标签跟着光标乱跳。规则优先的语义不能牺牲状态卫生。
            CancelDrag(h, false);
            return 0;
        }

        if (close >= 0) {
            closeDownIdx_ = close;  // 锁定按下时的关闭按钮，up 时只认这个索引
            return 0;               // 关闭在 up 时判定（避免按下即关）
        }
        if (tab >= 0) {
            // 固定标签不参与重排（Chrome pinned tab 同语义）：按下只选中，不预备拖拽。
            // locked 是"锁定子目录导航"，与位置无关，仍可正常重排 —— 两者别混。
            if (!items_[static_cast<size_t>(tab)].pinned) {
                dragIdx_ = tab;
                // 【来源 id 在按下时定格】消息是 PostMessage 入队的，到被处理之间
                // 集合可能已被别的消息改动，UP 时按下标反查会指错标签（见 common.h）。
                dragId_ = items_[static_cast<size_t>(tab)].id;
                dragStart_ = {x, y};
                dropIdx_ = tab;
                tearOff_ = false;   // 每次新拖拽重新判：上一轮脱离状态不能残留
                // 浮层"粘手"偏移：按下时鼠标在被拖标签内的横向位置。
                // 必须在此刻定格 —— 拖到边缘自动滚动会改 tabRects_，
                // 绘制时现算会让浮层随滚动跳位。
                const RECT& grabRc = tabRects_[static_cast<size_t>(tab)];
                dragGrabDx_ = x - grabRc.left;
                dragMouse_ = {x, y};
                SetCapture(h);
            }
            // 焦点与选中对 pinned 同样成立，所以放在"可否拖动"判断之外。
            // 焦点留在标签栏：作用域为"标签栏"的裸键（Shift+←/→/Home/End）才收得到。
            // App::Activate 只在焦点原本位于视图区时才把焦点搬走。
            SetFocus(h);
            NotifySelect(tab);
        }
        return 0;
    }
    case WM_CANCELMODE:
        // 系统取消模式（弹出菜单、切窗口、Alt+Tab 等）：拖拽必须收手。
        // 与 WM_CAPTURECHANGED 互补 —— 后者只在捕获**真被夺走**时来，这条是
        // 系统明确要求取消当前交互，此刻捕获可能还在本窗口。只靠前者会漏：
        // 弹层抢焦点但没抢捕获时，dragIdx_ 会一直挂着，下次进栏就乱跳。
        CancelDrag(h, false);
        return 0;
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
        if (w == kAnimTimer) {
            // 让位动画每帧：时间到就停表收尾，否则持续重绘。
            // 结束判定放在这里而不是绘制里 —— Paint 可能因其它原因不跑
            // （窗口被遮挡、最小化），只靠绘制自检会让定时器永远活着。
            const int durMs = Config::Get().s().tabAnimMs;
            if (!animating_ || durMs <= 0 ||
                GetTickCount64() - animStart_ >= static_cast<ULONGLONG>(durMs)) {
                StopReorderAnim();
            } else {
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
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
        // 【先提交拖拽，再问鼠标规则】规则层没有理由在松手这一刻接管一个
        // 明确的拖拽意图。提交语义与"为什么必须先放捕获再读值"见
        // tab_strip_input_drag.cpp 的 HandleDragCommit。
        if (dragIdx_ >= 0) {
            HandleDragCommit(h);
            return 0;
        }
        // 非拖拽的 UP 才走鼠标规则：规则命中则吞掉组件默认行为（选中/新建/关闭）。
        // 拖拽已在上面提前 return，不会被这里的 claim 截断。
        const bool claimed = sink_ && sink_(MouseTargetAt(x, y), m, MBTN_LEFT, CurrentMods(), pt);
        if (claimed) return 0;

        int close = HitClose(x, y);
        int tab = HitTab(x, y);
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
        if (tab >= 0 && !items_[static_cast<size_t>(tab)].locked &&
            !items_[static_cast<size_t>(tab)].pinned)
            NotifyClose(tab);
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
            // 双击标签正文**不再默认关闭**。
            //
            // 这里曾按"Chrome / Edge / VS Code 的惯例"写成关闭，但那句话是错的：
            // 三家对标签正文的双击都不关闭（VS Code 的双击是把预览标签固定）。
            // 而 Windows 用户的肌肉记忆是"双击 = 打开/激活"，照旧实现的结果是
            // 想切换却误关标签 —— 关闭已有中键、关闭钮、右键菜单三条明确入口，
            // 不需要把双击也变成关闭。
            //
            // 仍然先问鼠标规则：用户若显式把"双击标签"绑成某个动作，规则优先
            // （与其它输入的契约一致）。规则没接管就什么都不做。
            POINT pt{x, y};
            ClientToScreen(h, &pt);
            if (sink_ && sink_(MT_TAB, m, MBTN_LEFT, CurrentMods(), pt)) return 0;
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
    case WM_GETOBJECT:
        // OBJID_CLIENT：UIA/读屏软件要这个窗口的可访问性树。
        if (static_cast<LONG>(l) == OBJID_CLIENT)
            return uia::HandleGetObject(uia::TabTraitsFor(this), h, w, l);
        break;
    case uia::kMsgCount:
        return static_cast<LRESULT>(items_.size());
    case uia::kMsgItem: {
        auto* info = reinterpret_cast<uia::ItemInfo*>(l);
        const int i = static_cast<int>(w);
        // items_ 与 tabRects_ 是两个独立容器（Layout 可能还没跑，规模不同步）。
        // 只查 items_ 就取 tabRects_[i] 是越界读 —— 崩溃窗口在
        // 「读屏软件查询 vs 标签集合刚变、重排未完成」。两个都查。
        if (!info || i < 0 || i >= static_cast<int>(items_.size()) ||
            i >= static_cast<int>(tabRects_.size()))
            return FALSE;
        const RECT& r = tabRects_[static_cast<size_t>(i)];
        // 完全滚出可视区左侧的标签对读屏软件不可见：矩形在窗口外，点也点不到，
        // 列进可访问性树只会产生"幽灵项"（右侧由 maxScroll_ 保证不会滚出）。
        if (overflow_ && r.right <= 0) return FALSE;
        info->name = items_[static_cast<size_t>(i)].title;
        info->path = items_[static_cast<size_t>(i)].path;
        info->rect = r;
        info->selected = (i == active_);
        return TRUE;
    }
    case uia::kMsgSelect: {
        const int i = static_cast<int>(w);
        if (i >= 0 && i < static_cast<int>(items_.size())) NotifySelect(i);
        return 0;
    }
    case uia::kMsgInvoke: {
        // UIA 激活 = 执行默认动作。标签栏的默认动作就是"切到这个标签"，
        // 与 Select 的落点相同（但语义不同，见 tab_uia.h 的说明）——
        // 走同一条 NotifySelect 是因为在这个栏里两者恰好等价，
        // 而不是因为它们是同一个东西。
        const int i = static_cast<int>(w);
        if (i >= 0 && i < static_cast<int>(items_.size())) NotifySelect(i);
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
    // 固定标签不画关闭钮：误点率最高的一块区域不该放"关掉常用目录"。
    // 真要关，走右键菜单/命令（显式意图）。
    if (items_[static_cast<size_t>(index)].pinned) return false;
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
