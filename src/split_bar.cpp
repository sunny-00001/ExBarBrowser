// 可拖动布局边界的实现（设计取舍见 split_bar.h）。
//
// 本文件只做三件事：把自己画成一条线、把鼠标位置换算成落点、在按下/松开之间
// 维护拖动状态。真正的"怎么排两栏/两条侧栏"在 app_layout.cpp，
// 百分比 ↔ 像素的换算是 pane_split.cpp，侧栏宽度求解是 side_layout.cpp ——
// 这里不重复任何一边的口径。
#include "split_bar.h"

#include "pane_split.h"
#include "platform_win.h"
#include "ui_draw.h"

#include <algorithm>

// 类名由调用方从 identity.h 给出（规则 B1：品牌串只允许出现在那里）。

bool SplitBar::Create(HWND parent, const Theme& theme, const wchar_t* cls) {
    if (!cls) return false;
    win::RegisterClassOnce(cls, SplitBar::WndProc, CS_DBLCLKS);
    parent_ = parent;
    theme_ = &theme;
    // WS_CLIPSIBLINGS 与父窗口 viewArea 的同名样式成对使用：内容窗口重绘时
    // 必须把这条边界裁掉，否则"悬浮一下线就没了"；同时本窗口重绘也不会
    // 误盖相邻内容。这条边界本身不抢焦点、不做 Tab 停靠。
    hwnd_ = win::CreateChild(cls, parent, this, theme.edgeHit(), nullptr,
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    return hwnd_ != nullptr;
}

void SplitBar::Destroy() {
    if (!hwnd_) return;
    // 先撤捕获再销毁：带着捕获销毁窗口会让系统把捕获交给别的窗口，
    // 而那时我们还在拖 —— 松开那一刻就落到别人身上了。
    if (dragging_) {
        ReleaseCapture();
        dragging_ = false;
    }
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void SplitBar::ReloadTheme() {
    if (!hwnd_) return;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int SplitBar::GapWidth() const { return theme_ ? theme_->splitter() : 2; }

HCURSOR SplitBar::ResizeCursor() const {
    // 上下堆叠（分割线横向）= 拖它改高度 -> 上下箭头；左右并排 = 左右箭头。
    return LoadCursorW(nullptr, vertical_ ? IDC_SIZENS : IDC_SIZEWE);
}

void SplitBar::SetGeometry(const RECT& rc, bool vertical, int avail, int origin, int center) {
    metric_ = Metric::Percent;
    rc_ = rc;
    vertical_ = vertical;
    avail_ = avail;
    origin_ = origin;
    center_ = center;
    SyncWindow();
}

void SplitBar::SetPixelGeometry(const RECT& rc, bool vertical, int center, int loCenter,
                                int hiCenter) {
    metric_ = Metric::Pixels;
    rc_ = rc;
    vertical_ = vertical;
    center_ = center;
    loCenter_ = std::min(loCenter, hiCenter);
    hiCenter_ = std::max(loCenter, hiCenter);
    SyncWindow();
}

void SplitBar::SyncWindow() {
    if (!hwnd_) return;
    RECT cur{};
    GetWindowRect(hwnd_, &cur);
    MapWindowPoints(nullptr, parent_, reinterpret_cast<POINT*>(&cur), 2);
    const bool same = cur.left == rc_.left && cur.top == rc_.top && cur.right == rc_.right &&
                      cur.bottom == rc_.bottom;
    if (same) {
        // 【仍要压回最上层】两侧内容是按需创建的（切标签、首帧补建、面板刚展开），
        // 新窗口会排在边界条之上把它整条盖住 —— 那时几何没变，只抬层级。
        // 只抬层级（NOMOVE/NOSIZE）不产生 WM_PAINT。
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    // 【SWP_NOCOPYBITS 是"拖动不残留"的关键】缺了它，SetWindowPos 会把窗口原来的
    // 像素**拷贝**到新位置 —— 拖动中每移动一步，旧位置那条线就被拷到新位置，
    // 而旧位置还没重绘完，两块叠在一起 → 用户看到一路拖影。
    SetWindowPos(hwnd_, HWND_TOP, rc_.left, rc_.top, rc_.right - rc_.left,
                 rc_.bottom - rc_.top, SWP_NOACTIVATE | SWP_NOCOPYBITS);
    if (parent_) {
        // 【腾出来的那块必须重绘，而且不能只 InvalidateRect】
        // 父窗口是 WS_CLIPCHILDREN 的：InvalidateRect 标脏的区域在绘制时**会避开
        // 子窗口**，于是"已经被腾空、现在没有子窗口覆盖"的那一带反而没人画 ——
        // 屏幕上留着上一步的位图（就是用户说的"线条残留"）。
        // 必须 RedrawWindow + RDW_ERASE + RDW_ALLCHILDREN。
        RECT vacated = cur;
        RedrawWindow(parent_, &vacated, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    // 自己整块重画（顺序在父窗口之后：保证最终结果显示的是本窗口的正确内容）。
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

void SplitBar::SetVisible(bool visible) {
    if (!hwnd_) return;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void SplitBar::SetHover(bool on) {
    if (hover_ == on) return;
    hover_ = on;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK SplitBar::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<SplitBar>(h);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT SplitBar::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;   // 全部走 Paint 的双缓冲，擦底会把那一帧露出来

    case WM_PAINT:
        Paint(h);
        return 0;

    case WM_SETCURSOR:
        // 光标由本窗口自己定：DefWindowProc 会用窗口类的 IDC_ARROW，
        // 而这里是唯一需要"看起来能拖"的地方。
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(ResizeCursor());
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        if (dragging_) {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(h, &pt);
            NoteDrag(pt, false);
            return 0;
        }
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        SetHover(true);
        SetCursor(ResizeCursor());
        return 0;
    }

    case WM_MOUSELEAVE:
        tracking_ = false;
        if (!dragging_) SetHover(false);
        return 0;

    case WM_LBUTTONDOWN: {
        // 【不抢焦点】分割条不是可聚焦控件：SetFocus 会打断"视图里正在用键盘
        // 选文件"的用户，也会让双栏的活动窗格跳到一个不是视图的窗口上。
        //
        // 抓取偏移：鼠标相对边界中心的位置。之后整场拖动都用"鼠标 - 偏移"
        // 作为线位置，线不会因为"抓在缝边缘"而跳一下。
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(h, &pt);
        lastScreen_ = pt;
        POINT parentPt = pt;
        MapWindowPoints(nullptr, parent_, &parentPt, 1);
        const int mainPos = vertical_ ? parentPt.y : parentPt.x;
        dragOffset_ = mainPos - center_;
        lastValue_ = INT_MIN;   // 新一轮拖动：允许第一次移动就提交
        SetCapture(h);
        dragging_ = true;
        SetHover(true);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!dragging_) return 0;
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(h, &pt);
        // 先提交、再释放捕获：ReleaseCapture 会同步派发 WM_CAPTURECHANGED
        // （那里会把 dragging_ 复位），顺序反了松手这一步就丢了。
        NoteDrag(pt, true);
        dragging_ = false;
        ReleaseCapture();
        SetHover(false);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED:
        // 捕获被系统/别的控件抢走（弹层、切窗口）：拖动必须在这里收尾，
        // 否则 dragging_ 残留，下一次移动就把位置当成"还在拖"直接改落点。
        // 还要补一次 final —— 否则拖动中改的内存宽度一直没有落盘，
        // 用户松手后关掉窗口，下次启动又回到上一次的宽度。
        if (dragging_) {
            dragging_ = false;
            NoteDrag(lastScreen_, true);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void SplitBar::NoteDrag(const POINT& screenPt, bool final) {
    if (!dragFn_) return;
    lastScreen_ = screenPt;
    POINT pt = screenPt;
    ScreenToClient(parent_, &pt);
    // 沿主轴的位置；竖直分割条（上下堆叠）时主轴是 y。减掉抓取偏移，
    // 让线的相对位置全程跟手（见 dragOffset_ 的说明）。
    const int mainPos = vertical_ ? pt.y : pt.x;

    int value = 0;
    if (metric_ == Metric::Percent) {
        if (avail_ <= 0) return;
        const int local = panesplit::ClampCenter(avail_, GapWidth(), mainPos - dragOffset_ - origin_);
        value = panesplit::PercentFromCenter(avail_, GapWidth(), local);
    } else {
        // 像素口径：区间由布局求解给出（side_layout），这里只夹、不推导。
        value = std::clamp(mainPos - dragOffset_, loCenter_, hiCenter_);
    }

    // 拖动中去重：落点没变就不必重排。final=true 一定提交 —— 松开是
    // 落盘的时机，与"位置有没有变"无关（用户可能只是点了一下边界）。
    if (!final && value == lastValue_) return;
    lastValue_ = value;
    dragFn_(value, final);
}

void SplitBar::Paint(HWND h) {
    // 【底色 + 中线】两种边界、同一套观感：
    //
    //   内容 | bg | line | bg | 内容
    //
    // 打底色用 bg（= 文件列表/导航树的底，见 ui_palette.h 的 viewBg 说明），
    // 于是两侧内容与这条带子同色，视觉上只有中间那一根线。
    //
    // 双栏的窗口比缝两侧各宽 1px（theme.h 的 splitterHit），所以两栏 Shell 视图
    // 自带的 1px 边框被压在**本窗口底下**、根本不显示 —— 这就是"物理上不可能
    // 出现第二条线"。侧栏边界同理压在两块内容的交界上（见 split_bar.h）。
    //
    // 【悬停/拖动 = 中线变粗变强调色，而不是整块变色】整块 6px 换成 accent 太吵，
    // 而且一闪一闪的；把 1px 换成 3px 的 accent 既够醒目，也不改变任何宽度，
    // 所以不存在"悬浮后离开、宽度残留"的问题（每次 WM_PAINT 都是全量重画）。
    //
    // 【活动侧条（只有双栏那条用）】activeSide_ 指出"命令与导航当前打到哪一栏"，
    // 在那一侧沿缝画一条 2px 的 accent 纵深条 —— 与 DOpus"双栏必须显示 border
    // 区分源/目标"同一件事。这是**常驻状态提示**，与悬停/拖动的临时提示并存：
    // 条贴着缝的边、不占任何布局宽度，切栏只重绘本窗口（SetGeometry 的几何不变）。
    const COLORREF bg = theme_ ? theme_->bg : GetSysColor(COLOR_WINDOW);
    const bool live = dragging_ || hover_;
    const COLORREF lineColor =
        theme_ ? (live ? theme_->accent : theme_->line) : GetSysColor(COLOR_3DSHADOW);
    const COLORREF accent = theme_ ? theme_->accent : GetSysColor(COLOR_HIGHLIGHT);
    const int liveThick = theme_ ? theme_->S(2) : 2;
    const int sideThick = theme_ ? theme_->S(2) : 2;   // 活动侧条粗细，与 liveThick 同档
    const bool vertical = vertical_;

    ui::PaintBuffered(h, bg, [&](HDC mem, const RECT& rc) {
        // 中线：横分隔条画横线，竖分隔条画竖线。静止 1px，悬停/拖动时两侧各扩一点。
        const int grow = live ? liveThick : 0;
        RECT line;
        if (vertical) {
            const int y = (rc.top + rc.bottom) / 2;
            line = {rc.left, y, rc.right, y + 1 + grow};
        } else {
            const int x = (rc.left + rc.right) / 2;
            line = {x, rc.top, x + 1 + grow, rc.bottom};
        }
        // 夹回客户区：grow 可能把线画到窗口外，FillRect 会画进别处。
        line.left = std::max(line.left, rc.left);
        line.top = std::max(line.top, rc.top);
        line.right = std::min(line.right, rc.right);
        line.bottom = std::min(line.bottom, rc.bottom);
        if (line.right > line.left && line.bottom > line.top)
            ui::FillRectColor(mem, line, lineColor);

        // 活动侧条：贴着中线、属于活动窗格的那一侧。竖分割条（左右并排）时
        // side=0 在左、side=1 在右；横分割条（上下堆叠）时 side=0 在上、side=1 在下。
        if (activeSide_ == 0 || activeSide_ == 1) {
            RECT side;
            if (vertical) {
                const int y = (rc.top + rc.bottom) / 2;
                side = activeSide_ == 0 ? RECT{rc.left, y - sideThick, rc.right, y}
                                        : RECT{rc.left, y + 1, rc.right, y + 1 + sideThick};
            } else {
                const int x = (rc.left + rc.right) / 2;
                side = activeSide_ == 0 ? RECT{x - sideThick, rc.top, x, rc.bottom}
                                        : RECT{x + 1, rc.top, x + 1 + sideThick, rc.bottom};
            }
            side.left = std::max(side.left, rc.left);
            side.top = std::max(side.top, rc.top);
            side.right = std::min(side.right, rc.right);
            side.bottom = std::min(side.bottom, rc.bottom);
            if (side.right > side.left && side.bottom > side.top)
                ui::FillRectColor(mem, side, accent);
        }
    });
}