// 标签栏几何：标签宽度分配、溢出判定、滚动量、tooltip 热区。
//
// 所有尺寸都过 theme_->S()，本文件不出现裸像素；DPI 变化由 ReloadTheme 触发重算。
#include "tab_strip.h"

#include "ui_draw.h"

void TabStrip::EnsureVisible(int index) {
    if (!overflow_ || index < 0 || index >= static_cast<int>(tabRects_.size())) return;
    const int viewRight = ovfRc_.left - theme_->S(2);
    const RECT& r = tabRects_[static_cast<size_t>(index)];
    if (r.left < 0)
        ScrollBy(r.left);
    else if (r.right > viewRight)
        ScrollBy(r.right - viewRight);
}

void TabStrip::ScrollBy(int dx) {
    int next = scrollX_ + dx;
    if (next < 0) next = 0;
    if (next > maxScroll_) next = maxScroll_;
    if (next != scrollX_) {
        scrollX_ = next;
        Layout();
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
    const int avail = rc.right - margin * 2 - plusW - gap;
    const int h = rc.bottom;
    const int n = static_cast<int>(items_.size());

    std::vector<int> widths(static_cast<size_t>(n), theme_->S(112));
    int total = 0;
    for (int i = 0; i < n; ++i) {
        int w = ui::MeasureText(dc, theme_->font, items_[static_cast<size_t>(i)].title) +
                theme_->S(62);
        w = std::max(theme_->S(112), std::min(w, theme_->S(230)));
        widths[static_cast<size_t>(i)] = w;
        total += w;
    }

    overflow_ = false;
    int width = theme_->S(112);
    if (n > 0) {
        if (total <= avail) {
            // 按内容宽排，剩余空间留白
        } else if (n * theme_->S(112) <= avail) {
            width = avail / n;
        } else {
            overflow_ = true;
            width = theme_->S(112);
        }
    }

    const int ovfSpace = overflow_ ? (ovfW + gap) : 0;
    const int viewRight = rc.right - margin - plusW - gap - ovfSpace;

    int x = margin - (overflow_ ? scrollX_ : 0);
    for (int i = 0; i < n; ++i) {
        int w = overflow_ ? width : (total <= avail ? widths[static_cast<size_t>(i)] : width);
        RECT r{x, 0, x + w, h};
        tabRects_.push_back(r);
        int cy = h / 2;
        RECT c{r.right - theme_->S(24), cy - theme_->S(8), r.right - theme_->S(8),
               cy + theme_->S(8)};
        closeRects_.push_back(c);
        x += w;
    }
    int contentEnd = x + (overflow_ ? scrollX_ : 0);
    // 不多加 margin：滚动到最后时右侧本就会空出半个标签宽的 margin，滚过头等于永久留白。
    const int viewW = static_cast<int>(rc.right) - margin - plusW - gap - ovfSpace;
    maxScroll_ = overflow_ ? std::max(0, contentEnd - viewW) : 0;
    if (scrollX_ > maxScroll_) scrollX_ = maxScroll_;

    int plusX = rc.right - margin - plusW;
    newRect_ = {plusX, 0, plusX + plusW, h};
    ovfRc_ = overflow_ ? RECT{viewRight + gap / 2, 0, viewRight + gap / 2 + ovfW, h} : RECT{};
    (void)viewRight;

    ReleaseDC(hwnd_, dc);
    RebuildTooltip(hwnd_);
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
