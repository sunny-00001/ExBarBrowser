// 书签栏绘制与几何：布局、工具提示、绘制、键盘焦点框。
// 职责单一：只读 items_ 与主题，不处理输入、不写书签数据。
// 与 bookmark_bar.cpp / bookmark_bar_hit.cpp 同属 BookmarkBar 类，头文件声明不变。
#include "bookmark_bar.h"

#include "config.h"
#include "icon_resolve.h"
#include "settings_bounds.h"
#include "ui_draw.h"

void BookmarkBar::Layout() {
    if (!hwnd_ || !theme_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    // 量字走唯一原语（ui::MeasureText 内部选字体并还原），不再各自 SelectObject
    HDC dc = GetDC(hwnd_);

    const int margin = theme_->S(6);
    const int padX = theme_->S(2);
    // 图标格宽必须与 Paint 里画图标的槽完全一致，否则文字会整体错位一格 ——
    // 两处都取同一个令牌，不再各写一个 S(16)。
    const int iconW = theme_->iconSmall();
    const int gap = theme_->S(6);
    const int ovfW = theme_->S(24);
    const int avail = rc.right - margin * 2;

    int total = 0;
    std::vector<int> widths(items_.size(), 0);
    for (size_t i = 0; i < items_.size(); ++i) {
        const std::wstring& t = items_[i].label;
        int textW = ui::MeasureText(dc, theme_->font, t);
        int w = padX * 2 + iconW + gap + textW + (items_[i].folder ? theme_->S(14) : 0);
        widths[i] = w;
        total += w;
        // 只有相邻项之间才有间距；末项之后不计，否则 total 偏大 1px，临界宽度会误判溢出。
        if (i + 1 < items_.size()) total += theme_->S(1);
    }

    overflow_ = total > avail;
    const int viewRight = overflow_ ? (rc.right - margin - ovfW - theme_->S(4)) : (rc.right - margin);
    int x = margin;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (x + widths[i] > viewRight) {
            items_[i].rc = RECT{0, 0, 0, 0};
            items_[i].iconRc = RECT{0, 0, 0, 0};
            continue;
        }
        items_[i].rc = RECT{x, theme_->S(2), x + widths[i], rc.bottom - theme_->S(2)};
        // 图标命中区与 Paint 里画图标的槽完全重合（左侧 padX 起、iconW 宽）。
        // 只有这一格内是"列子目录"，右侧文字区仍是打开/分类菜单。
        items_[i].iconRc = RECT{items_[i].rc.left + padX, items_[i].rc.top,
                                items_[i].rc.left + padX + iconW, items_[i].rc.bottom};
        x += widths[i] + theme_->S(1);
    }
    ovfRc_ = overflow_ ? RECT{rc.right - margin - ovfW, theme_->S(2), rc.right - margin,
                              rc.bottom - theme_->S(2)}
                       : RECT{};

    ReleaseDC(hwnd_, dc);
    RebuildTooltip(hwnd_);
}

void BookmarkBar::RebuildTooltip(HWND h) {
    // 工具提示走唯一实现（TooltipHost）：文本复制与 id 管理都在里面，
    // 不再需要"两趟稳定文本"的手工做法。
    // 提示窗最大宽以前写死 600：长书签路径一多就被截得看不出是哪一条。
    const int maxW = std::clamp(Config::Get().s().bookmarkBarTooltipWidth,
                                kBookmarkBarTooltipWidthMin, kBookmarkBarTooltipWidthMax);
    tooltip_.Attach(h, theme_->S(maxW));
    std::vector<ui::TooltipHost::Item> tips;
    tips.reserve(items_.size());
    for (const auto& it : items_) {
        if (it.rc.right <= it.rc.left) continue;
        tips.push_back({it.rc, it.folder ? (it.label + L"（分类）") : it.path});
    }
    tooltip_.SetTools(tips);
}

void BookmarkBar::Paint(HWND h) {
    ui::PaintBuffered(h, theme_->bar, [&](HDC mem, const RECT& rc) {
        ui::ScopedFont font(mem, theme_->font);

        const int padX = theme_->S(2);
        // 与 Layout 的 iconW 同一令牌（见那里的注释）
        const int iconPx = theme_->iconSmall();

        // 空状态提示：一条书签都没有时，栏是一条什么都没有的灰带。
        // 用户（尤其新用户）看不出"这里是书签栏、可以往里放东西"，
        // 而右键菜单里"添加当前目录"这条唯一能自救的入口完全不可见。
        // 与资源管理器/Opus 的收藏栏一致：给一行浅色提示文本。
        if (items_.empty()) {
            RECT tr{theme_->S(10), 0, rc.right - theme_->S(10), rc.bottom};
            ui::DrawTextRow(mem, theme_->font,
                            L"书签栏（Ctrl+D 收藏当前目录，Ctrl+B 聚焦本栏）", tr, theme_->disabled,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        for (size_t i = 0; i < items_.size(); ++i) {
            const Item& it = items_[i];
            if (it.rc.right <= it.rc.left) continue;
            const bool draggingThis = dragging_ && dragIdx_ == static_cast<int>(i);
            if (hover_ == static_cast<int>(i) && !draggingThis) FillRect(mem, &it.rc, theme_->brHover);
            // 图标格单独高亮：它是"列子目录"的按钮（等价面包屑分隔符），
            // 与右侧文字区（导航/分类菜单）是两个动作，需让用户看出边界。
            if (hoverIcon_ == static_cast<int>(i) && !draggingThis) {
                RECT ir = it.iconRc;
                InflateRect(&ir, theme_->S(1), 0);
                ui::FillRound(mem, ir, theme_->pressed, theme_->radiusBtn());
            }
            int x = it.rc.left + padX;
            if (it.icon) {
                RECT ir{x, it.rc.top, x + iconPx, it.rc.bottom};
                ui::DrawIconCentered(mem, it.icon, ir, iconPx);
            }
            x += iconPx + theme_->S(6);
            RECT tr{x, it.rc.top, it.rc.right - padX - (it.folder ? theme_->S(14) : 0), it.rc.bottom};
            ui::DrawTextRow(mem, nullptr, it.label, tr, draggingThis ? theme_->sub : theme_->text,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (it.folder) {
                // 与导航栏/标签栏同一个箭头原语：用 chevron 字形 + 主题色，
                // 而不是自绘折线（DPI 下尺寸写死、深色下也不跟随文字色）。
                RECT cr{it.rc.right - padX - theme_->S(14), it.rc.top,
                        it.rc.right - padX - theme_->S(2), it.rc.bottom};
                ui::DrawChevron(mem, (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2,
                                theme_->iconChevron(), theme_->sub, 1);
            }
        }

        if (overflow_) {
            if (hotOvf_) FillRect(mem, &ovfRc_, theme_->brHover);
            ui::DrawGlyph(mem, ico::IconId::More, ovfRc_, theme_->iconGlyph(), theme_->sub);  // More：与标签栏的下箭头区分
        }

        if (dropPreview_ && dropInto_ >= 0 && dropInto_ < static_cast<int>(items_.size())) {            // 拖进分类：整项描边（而不是画插入线）。两种提示必须一眼可分 ——
            // 插入线表示"放到它旁边"，描边表示"放进它里面"。
            RECT r = items_[static_cast<size_t>(dropInto_)].rc;
            if (r.right > r.left) {
                ui::FrameRectColor(mem, r, theme_->accent);
                RECT in = r;
                InflateRect(&in, -1, -1);
                ui::FrameRectColor(mem, in, theme_->accent);
            }
        } else if (dropPreview_ && dropIdx_ >= 0 &&
                   dropIdx_ <= static_cast<int>(items_.size())) {
            // 插入线：画在 dropIdx_ 位置（项间隙中线或末项右缘），压住分隔线但别盖下边框。
            int gx;
            if (dropIdx_ >= static_cast<int>(items_.size())) {
                gx = items_.empty() ? theme_->S(6) : items_.back().rc.right + theme_->S(1) / 2;
            } else if (dropIdx_ == 0) {
                gx = items_[0].rc.left - theme_->S(1) / 2;
            } else {
                const RECT& a = items_[static_cast<size_t>(dropIdx_ - 1)].rc;
                const RECT& b = items_[static_cast<size_t>(dropIdx_)].rc;
                gx = b.left - theme_->S(1) / 2;
                if (gx < a.right) gx = (a.right + b.left) / 2;  // 间隙被裁剪时取中线
            }
            RECT bar{gx, theme_->S(3), gx + theme_->S(2), rc.bottom - 2};
            ui::FillRectColor(mem, bar, theme_->accent);
        }

        RECT hr{0, rc.bottom - 1, rc.right, rc.bottom - 1};
        ui::DrawSeparator(mem, hr, false, theme_->line);

        // 键盘焦点框：整栏有焦点且落到了某一项。用系统 focus 点线框而不是自绘
        // 矩形，是为了在高对比度/不同 DPI 下都保持一致（这是唯一一处故意
        // 交给系统画的地方 —— 焦点指示是平台约定，不该自创样式）。
        // 只在有焦点时画：否则会和"悬停"看起来一模一样，用户分不清
        // "鼠标在这"和"键盘在这"。
        PaintFocusRing(mem);
    });
}

void BookmarkBar::PaintFocusRing(HDC dc) const {
    if (!hwnd_ || GetFocus() != hwnd_) return;
    if (kbdIdx_ < 0 || kbdIdx_ >= static_cast<int>(items_.size())) return;
    const RECT& r = items_[static_cast<size_t>(kbdIdx_)].rc;
    if (r.right <= r.left) return;
    RECT fr = r;
    InflateRect(&fr, -theme_->S(1), -theme_->S(1));
    DrawFocusRect(dc, &fr);
}
