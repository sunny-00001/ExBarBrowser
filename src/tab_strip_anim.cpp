// 标签栏的过渡与拖拽浮层：让位动画（StartReorderAnim / AnimDx）与被拖标签的抬升绘制。
//
// 从 tab_strip.cpp / tab_strip_paint.cpp 按**作用域**拆出来：本文件只回答
// "标签在过渡中画在哪、被拖的那一格怎么抬起来"，不碰命中、不碰模型、不改顺序。
#include "tab_strip.h"

#include "config.h"      // tabAnimMs / tabDragGhost / tabTearOffDy（当帧读，改完即生效）
#include "ui_draw.h"

void TabStrip::StopReorderAnim() {
    const bool was = animating_;
    animating_ = false;
    animFrom_.clear();
    if (!was || !hwnd_) return;
    KillTimer(hwnd_, kAnimTimer);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TabStrip::StartReorderAnim(const std::vector<int>& oldOrder,
                                const std::map<int, int>& oldLeft) {
    if (items_.empty() || oldOrder.size() != items_.size() ||
        tabRects_.size() != items_.size())
        return;
    // 只有"顺序真的变了"才动：切标签 / 改色 / 改名 / 改锁定都会走 SetTabs，
    // 那些情况下每个标签都还在原位，动画是纯噪声（还会让点击位置短暂错位）。
    bool reordered = false;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (oldOrder[i] != items_[i].id) {
            reordered = true;
            break;
        }
    }
    if (!reordered) return;
    // 0 = 用户明确关掉了让位动画：直接落到最终位置，不留 animating_ 状态。
    // 判据放在"确实重排了"之后 —— 顺序没变时本来就不该起表。
    if (Config::Get().s().tabAnimMs <= 0) return;

    animFrom_.assign(items_.size(), 0);
    bool any = false;
    for (size_t i = 0; i < items_.size(); ++i) {
        const int to = static_cast<int>(tabRects_[i].left);
        auto it = oldLeft.find(items_[i].id);
        const int from = it == oldLeft.end() ? to : it->second;
        animFrom_[i] = from;
        if (from != to) any = true;
    }
    if (!any) {
        animFrom_.clear();
        return;
    }
    animating_ = true;
    animStart_ = GetTickCount64();
    SetTimer(hwnd_, kAnimTimer, 15, nullptr);   // ~60fps；只在动画期间存在
}

int TabStrip::AnimDx(int index) const {
    if (!animating_ || index < 0 || index >= static_cast<int>(animFrom_.size()) ||
        index >= static_cast<int>(tabRects_.size()))
        return 0;
    const int to = static_cast<int>(tabRects_[static_cast<size_t>(index)].left);
    const int from = animFrom_[static_cast<size_t>(index)];
    if (from == to) return 0;
    const ULONGLONG elapsed = GetTickCount64() - animStart_;
    // 当帧读配置：改完设置下一次拖拽就按新时长跑，不必重启。
    // 若动画途中被设成 0，这里 t 变成无穷大 → TabAnimDx 返回 0（立即就位），
    // 正是"关掉动画"该有的表现，不需要额外分支。
    const int durMs = Config::Get().s().tabAnimMs;
    const double t = durMs <= 0 ? 1.0
                                : static_cast<double>(elapsed) / static_cast<double>(durMs);
    return TabAnimDx(from - to, t);
}

// 被拖标签的浮层：跟随光标抬升。
//
// 为什么不是真半透明：GDI 的 AlphaBlend 需要 32bpp DIB，拖动期间每帧建/销
// 位图是纯 churn；而标签栏底本来就是纯色，用 brBg 实底 + accent 描边已经能
// 读出"这一格被拿起来了"。等以后有统一的合成层再换成 alpha。
void TabStrip::DrawDragGhost(HDC dc, const RECT& clip) {
    if (!dragging_ || dragIdx_ < 0) return;
    // 浮层是可关的偏好（tabDragGhost）：关掉后拖动时只剩插入线，
    // 与上一版观感一致。原槽仍留空（那是"这一格被拿走了"的语义，不是装饰）。
    if (!Config::Get().s().tabDragGhost) return;
    const int i = dragIdx_;
    if (i >= static_cast<int>(tabRects_.size()) || i >= static_cast<int>(items_.size()))
        return;
    const RECT& src = tabRects_[static_cast<size_t>(i)];
    const int w = src.right - src.left;
    const int h = src.bottom - src.top;
    if (w <= 0 || h <= 0) return;
    // 抓取点：按下时记下的偏移，让浮层"粘手"而不是跳到光标中心。
    // 用缓存的 dragGrabDx_（自动滚动会改 src.left，现算会跳位）。
    int grab = dragGrabDx_;
    if (grab < 0) grab = 0;
    if (grab > w) grab = w;
    RECT g{dragMouse_.x - grab, src.top, dragMouse_.x - grab + w, src.top + h};

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);

    // 本体 + 1dp accent 描边（accent 是本工程"可交互/强调"的唯一令牌）。
    FillRect(dc, &g, theme_->brBg);
    const int t = theme_->S(1);
    ui::FillRectColor(dc, RECT{g.left, g.top, g.right, g.top + t}, theme_->accent);
    ui::FillRectColor(dc, RECT{g.left, g.bottom - t, g.right, g.bottom}, theme_->accent);
    ui::FillRectColor(dc, RECT{g.left, g.top, g.left + t, g.bottom}, theme_->accent);
    ui::FillRectColor(dc, RECT{g.right - t, g.top, g.right, g.bottom}, theme_->accent);

    ui::ScopedFont font(dc, theme_->font);
    const TabStripItem& it = items_[static_cast<size_t>(i)];
    int textLeft = g.left + theme_->S(10);
    if (it.icon) {
        const int iconPx = theme_->iconSmall();
        RECT ir{g.left + theme_->S(8), g.top, g.left + theme_->S(8) + iconPx, g.bottom};
        ui::DrawIconCentered(dc, it.icon, ir, iconPx);
        textLeft = g.left + theme_->S(8) + iconPx + theme_->S(6);
    }
    if (!it.pinned) {
        const RECT tr{textLeft, g.top, g.right - theme_->S(26), g.bottom};
        ui::DrawTextRow(dc, nullptr, it.title, tr, theme_->text,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    RestoreDC(dc, saved);
}
