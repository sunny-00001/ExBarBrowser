// 标签栏几何：标签宽度分配、溢出判定、滚动量、tooltip 热区。
//
// 所有尺寸都过 theme_->S()，本文件不出现裸像素；DPI 变化由 ReloadTheme 触发重算。
#include "tab_strip.h"

#include "config.h"
#include "edge_autoscroll.h"
#include "ui_draw.h"

void TabStrip::EnsureVisible(int index) {
    if (!overflow_ || index < 0 || index >= static_cast<int>(tabRects_.size())) return;
    const RECT& r = tabRects_[static_cast<size_t>(index)];
    if (r.left < 0)
        ScrollBy(r.left);
    else if (r.right > tabViewRight_)
        ScrollBy(r.right - tabViewRight_);
}

void TabStrip::ScrollBy(int dx) {
    int next = scrollX_ + dx;
    if (next < 0) next = 0;
    if (next > maxScroll_) next = maxScroll_;
    if (next != scrollX_) {
        scrollX_ = next;
        Layout();
        UpdateTooltipRects();   // 只挪热区；重建的话 N 个标签要发 2N 次消息
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void TabStrip::Layout() {
    tabRects_.clear();
    closeRects_.clear();
    if (!hwnd_ || !theme_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    // 量字走唯一原语（ui::MeasureText 内部选字体并还原）
    HDC dc = GetDC(hwnd_);

    const int margin = theme_->S(6);
    const int plusW = theme_->S(28);
    const int ovfW = theme_->S(26);
    const int gap = theme_->S(4);
    // 标签的最大可用宽度：栏两侧 margin，右侧再给 + 留出 (plusW + gap)。
    // + 虽改为紧跟标签，但"标签最多能铺到哪"仍要给它留位，否则铺满时 + 会被挤出栏外。
    const int avail = rc.right - margin * 2 - plusW - gap;
    const int h = rc.bottom;
    const int n = static_cast<int>(items_.size());

    // 宽度区间来自设置（默认 112–230dp，与旧行为一致）。
    // 只用一处读取，量字/均分/溢出三种分支共用同一对边界。
    const int minW = theme_->S(Config::Get().s().tabMinWidth);
    const int maxW = theme_->S(Config::Get().s().tabMaxWidth);
    std::vector<int> widths(static_cast<size_t>(n), minW);
    int total = 0;

    // Chromium 的快速关闭：关掉一个标签后，右侧标签左移、下一个关闭钮仍落在
    // 鼠标下。本栏目有三种宽度模式（内容宽/均分/溢出），任何一种重算都会让
    // 关闭钮漂移 —— 所以 rapid 期间直接沿用关闭前的宽度表，跳过重算。
    const bool rapid = !rapidWidths_.empty() && rapidCloseIndex_ >= 0 &&
                       rapidWidths_.size() == static_cast<size_t>(n + 1);
    if (rapid) {
        for (int i = 0, k = 0; i < static_cast<int>(rapidWidths_.size()); ++i) {
            if (i == rapidCloseIndex_) continue;
            // 接管被关位置的标签用**被关标签的宽度**，它的右边界就落在原关闭钮
            // 位置 —— 鼠标不用动，下一次点击仍是关闭。其余标签保持原宽不重排。
            const int w = (k == rapidCloseIndex_)
                              ? rapidWidths_[static_cast<size_t>(rapidCloseIndex_)]
                              : rapidWidths_[static_cast<size_t>(i)];
            widths[static_cast<size_t>(k)] = w;
            total += w;
            ++k;
        }
    } else {
        for (int i = 0; i < n; ++i) {
            // 固定标签只留图标：宽度固定、不参与"按标题量字"。
            // 这是 pin 的核心视觉信号 —— 与 Chrome/VS Code 的图标化一致，
            // 也让固定区在标签很多时占用可控。
            if (items_[static_cast<size_t>(i)].pinned) {
                widths[static_cast<size_t>(i)] = theme_->S(40);
                total += widths[static_cast<size_t>(i)];
                continue;
            }
            int w = ui::MeasureText(dc, theme_->font, items_[static_cast<size_t>(i)].title) +
                    theme_->S(62);
            w = std::max(minW, std::min(w, maxW));
            widths[static_cast<size_t>(i)] = w;
            total += w;
        }
    }

    overflow_ = false;
    int width = minW;
    if (!rapid && n > 0) {
        if (total <= avail) {
            // 按内容宽排，剩余空间留白
        } else if (n * minW <= avail) {
            width = avail / n;
        } else {
            overflow_ = true;
            width = minW;
        }
    }

    // 溢出时右侧要同时容纳 » 和 +：»(ovfW) + gap + +(plusW) + gap。
    // 不是溢出时只有 +，标签可以一直铺到栏右缘附近（avail 已给 + 留过位）。
    const int rightReserve = overflow_ ? (ovfW + gap + plusW + gap) : (plusW + gap);
    // 标签可视右缘：裁切线。溢出时 »/+ 在它右边，非溢出时等于栏右缘。
    tabViewRight_ = rc.right - margin - rightReserve;

    int x = margin - (overflow_ ? scrollX_ : 0);
    lastWidths_.clear();
    lastWidths_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int w = rapid ? widths[static_cast<size_t>(i)]
                            : (items_[static_cast<size_t>(i)].pinned
                                   ? widths[static_cast<size_t>(i)]
                                   : (overflow_ ? width
                                                : (total <= avail
                                                       ? widths[static_cast<size_t>(i)]
                                                       : width)));
        lastWidths_.push_back(w);
        RECT r{x, 0, x + w, h};
        tabRects_.push_back(r);
        int cy = h / 2;
        RECT c{r.right - theme_->S(24), cy - theme_->S(8), r.right - theme_->S(8),
               cy + theme_->S(8)};
        closeRects_.push_back(c);
        x += w;
    }
    // + 紧跟标签右缘：非溢出取内容末端 x，溢出取可视右缘（标签滚走后 x 在栏外，
    // 跟着它会把 + 一起带走）。minX/maxX 夹逼保证按钮完整落在栏内。
    const int tabsRight = overflow_ ? tabViewRight_ : x;
    const int plusX = TabNewButtonX(tabsRight + gap, margin, rc.right - margin - plusW);
    newRect_ = {plusX, 0, plusX + plusW, h};
    // » 在 + 的右边（溢出才有）。两者紧邻、不重叠：+ 右缘 + gap = » 左缘。
    ovfRc_ = overflow_ ? RECT{plusX + plusW + gap, 0, plusX + plusW + gap + ovfW, h} : RECT{};

    int contentEnd = x + (overflow_ ? scrollX_ : 0);
    // 滚动上限按"内容末端 - 可视右缘"算：滚到最后时内容右缘刚好贴住裁切线。
    // 右缘用 tabViewRight_ 而不是 ovfRc_.left —— 后者现在在 + 的右边，
    // 用它会使最后几个标签滚到 +/» 底下仍算"没滚到底"。
    maxScroll_ = overflow_ ? std::max(0, contentEnd - tabViewRight_) : 0;
    if (scrollX_ > maxScroll_) scrollX_ = maxScroll_;

    ReleaseDC(hwnd_, dc);
}

// 拖拽插入位置的那条竖线画在哪（客户区 x）。-1 = 不画。
//
// 判定本体在 TabDropLineX（tab_strip.h 的纯函数），这里只负责把成员传进去 ——
// 逻辑与"是否需要读取窗口状态"分开，那条规则才能被单测覆盖。
int TabStrip::DropTargetAt(int x, int y) const {
    int target = HitTab(x, y);
    if (target >= 0) return target;
    // 落在标签之间（或边缘滚动后悬在任何标签外）：按 x 推断插入位置。
    target = static_cast<int>(tabRects_.size()) - 1;
    for (size_t i = 0; i < tabRects_.size(); ++i) {
        if (x < (tabRects_[i].left + tabRects_[i].right) / 2) {
            target = static_cast<int>(i);
            break;
        }
    }
    return target;
}

// 边缘带与起停判定已收口到 edge::Update（src/edge_autoscroll.h，与书签栏共用）；
// 这里只保留"边缘带怎么算 + 最后鼠标 x 缓存"这两个标签栏自己的细节。
void TabStrip::UpdateAutoScroll(int x, int stepPx) {
    dragMouseX_ = x;
    // 边缘带：左缘留出边距，右缘以裁切线为准（»/+ 区域不算标签）。
    const int edge = theme_->S(28);
    const int lo = theme_->S(6) + edge;
    const int hi = tabViewRight_ - edge;
    edge::Update(&dragScrollStep_, hwnd_, kDragScrollTimer, x, lo, hi, stepPx,
                 static_cast<UINT>(Config::Get().s().tabDragScrollMs));
}

void TabStrip::StopAutoScroll() {
    edge::Stop(&dragScrollStep_, hwnd_, kDragScrollTimer);
}

int TabStrip::DropLineX() const {
    return TabDropLineX(tabRects_, dragIdx_, dropIdx_, dragging_);
}

void TabStrip::UpdateTooltipRects() {
    // 与 RebuildTooltip 用同一套筛选口径（空路径不登记），否则 id 会对不上。
    std::vector<RECT> rects;
    rects.reserve(tabRects_.size());
    for (size_t i = 0; i < tabRects_.size() && i < items_.size(); ++i) {
        if (!items_[i].path.empty()) rects.push_back(tabRects_[i]);
    }
    tooltip_.MoveTools(rects);
}

void TabStrip::RebuildTooltip(HWND h) {
    // 工具提示走唯一实现（TooltipHost）
    tooltip_.Attach(h, theme_->S(600));
    std::vector<ui::TooltipHost::Item> items;
    items.reserve(tabRects_.size());
    for (size_t i = 0; i < tabRects_.size() && i < items_.size(); ++i) {
        if (items_[i].path.empty()) continue;
        items.push_back({tabRects_[i], items_[i].path});
    }
    tooltip_.SetTools(items);
}
