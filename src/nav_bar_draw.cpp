// 导航栏绘制（从 nav_bar.cpp 拆出，保持单文件 <=500 行）
//
// 绘制原语一律走 ui::（字体池 / 字形 / 图标 / 填充），与 mod 的 NavDrawBar /
// DrawRichBreadcrumb 同源；这里只保留"布局矩形 → 调用原语"的胶水。
#include "nav_bar.h"

#include "ui_draw.h"
#include "nav_metrics.h"

namespace {

// = DrawCrumbSep：面包屑分隔符用 Fluent 字形（不是 1px 折线，折线在深色下发锯齿）
void DrawSep(HDC dc, const RECT& zone, const Theme& th, COLORREF color, wchar_t glyph) {
    // 走尺寸令牌，不按"区高比例"反推：这里原来是 MulDiv(12, 区高, 24)，
    // 于是同一套 chevron 在面包屑里随栏高漂移、在标签栏/弹层里固定 —— 三套观感。
    ui::DrawGlyph(dc, glyph, zone, th.iconChevron(), color, DT_CENTER | DT_VCENTER);
}

}  // namespace

void NavBar::DrawButton(HDC dc, const Btn& b, bool hover, bool down) {
    const int inset = theme_->S(1);
    RECT hr{b.rc.left + inset, b.rc.top + inset, b.rc.right - inset, b.rc.bottom - inset};
    // 圆角底板走唯一原语（ui::FillRound）：以前这里有一份私有 FillRound，
    // 别的栏拿不到只能画直角 —— 那就是"导航栏圆角、其余栏直角"的成因。
    if (down)
        ui::FillRound(dc, hr, theme_->pressed, theme_->radiusBtn());
    else if (hover)
        ui::FillRound(dc, hr, theme_->hover, theme_->radiusBtn());

    int arrowW = b.hasArrow ? theme_->S(kArrow96) : 0;
    RECT box{b.rc.left, b.rc.top, b.rc.right - arrowW, b.rc.bottom};
    const int isz = theme_->iconCompact();
    COLORREF color = b.enabled ? theme_->text : theme_->disabled;
    if (b.icon) {
        ui::DrawIconCentered(dc, b.icon, box, isz);
    } else if (b.glyph) {
        ui::DrawGlyph(dc, b.glyph, box, isz, color);
    }

    if (b.hasArrow) {
        int divX = b.rc.right - arrowW;
        RECT vr{divX, b.rc.top + theme_->S(6), divX, b.rc.bottom - theme_->S(6)};
        ui::DrawSeparator(dc, vr, true, theme_->line);
        // 分裂箭头曾经传 S(4)，被 DrawGlyph 的 8px 下限静默抬成 8px：96%~168% DPI
        // 区间里按钮从 24px 长到 42px，箭头却一动不动（见 theme.h iconChevron96 注释）。
        ui::DrawChevron(dc, divX + arrowW / 2, (b.rc.top + b.rc.bottom) / 2, theme_->iconChevron(),
                        color, 1);
    }
}

void NavBar::DrawBreadcrumb(HDC dc, const RECT& rc) {
    int save = SaveDC(dc);
    RECT clip{rc.left, rc.top, dropRc_.left, rc.bottom};
    HRGN rgn = CreateRectRgnIndirect(&clip);
    SelectClipRgn(dc, rgn);
    DeleteObject(rgn);

    int cx = rc.left + theme_->S(kLpad96);

    // 前导图标：当前文件夹的 shell 图标（与 mod 同尺寸 iconSize）
    if (leadIcon_) {
        ui::DrawIconVCentered(dc, leadIcon_, cx, rc.top, rc.bottom, theme_->iconCompact());
    }
    cx += theme_->S(kLead96);

    // 常驻折叠钮（「‹」）：hot 时整格高亮，箭头提亮
    moreRc_ = {cx, rc.top, cx + theme_->S(kSep96), rc.bottom};
    {
        bool hot = hotMore_;
        RECT zone{cx + theme_->S(3), rc.top, cx + theme_->S(kSep96) - theme_->S(3), rc.bottom};
        if (hot) {
            RECT hr{zone.left - theme_->S(3), rc.top, zone.right + theme_->S(3), rc.bottom};
            ui::FillRectColor(dc, hr, theme_->hover);
        }
        DrawSep(dc, zone, *theme_, hot ? theme_->text : theme_->chevron,
                ico::Glyph(ico::IconId::ChevronLeft));
    }
    cx += theme_->S(kSep96);

    const auto& parts = model_.parts();
    for (size_t k = 0; k < segRects_.size(); ++k) {
        const int pi = firstShown_ + static_cast<int>(k);
        // segRects_ 由 layout 按 parts 的实际范围生成，pi 必在 [0, parts.size()) 内。
        const CrumbPart& part = parts[static_cast<size_t>(pi)];

        if (hotSeg_ == pi) {
            RECT hr{segRects_[k].left - theme_->S(4), rc.top, segRects_[k].right + theme_->S(4),
                    rc.bottom};
            ui::FillRectColor(dc, hr, theme_->hover);
        }
        ui::DrawTextRow(dc, theme_->font, part.label, segRects_[k],
                        part.ghost ? theme_->disabled : theme_->text,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        // 每级分隔符：复用通用 DrawChevron，rest 保持原来的箭头形态，
        // 仅点开菜单那一级切到 down（dir>0）体现展开态。与弹层 folder 行同一原语、同一尺寸。
        bool chevOpen = openChev_ == pi;
        bool chevHot = hotChev_ == pi || chevOpen;
        RECT zone{chevRects_[k].left + theme_->S(3), rc.top,
                  chevRects_[k].right - theme_->S(3), rc.bottom};
        if (chevHot) {
            RECT hr{zone.left - theme_->S(3), rc.top, zone.right + theme_->S(3), rc.bottom};
            // 与 mod 一致：展开态按 pressed 底色，悬停才是 hover
            ui::FillRectColor(dc, hr, chevOpen ? theme_->pressed : theme_->hover);
        }
        // 用 chev 前缀：cx/cy 在外层已经用于"沿栏推进的 x 坐标"，
        // 同名会让读者以为这是同一个量。
        const int chevCx = (zone.left + zone.right) / 2;
        const int chevCy = (zone.top + zone.bottom) / 2;
        ui::DrawChevron(dc, chevCx, chevCy, theme_->iconChevron(),
                        chevHot ? theme_->text : theme_->chevron, chevOpen ? 1 : 0);
    }
    RestoreDC(dc, save);
}

void NavBar::Paint(HWND h) {
    ui::PaintBuffered(h, theme_->bar, [&](HDC mem, const RECT& rc) {
        for (const auto& b : buttons_) DrawButton(mem, b, hoverBtn_ == b.id, downBtn_ == b.id);

        if (!buttons_.empty()) {
            const int sepX = buttons_.back().rc.right + theme_->S(kGap96) / 2;
            RECT vr{sepX, rc.top + theme_->S(6), sepX, rc.bottom - theme_->S(6)};
            ui::DrawSeparator(mem, vr, true, theme_->line);
        }

        if (editing_) {
            ui::FillRectColor(mem, dropRc_, theme_->editBg);
        } else {
            DrawBreadcrumb(mem, fieldRc_);
        }

        // 字段内下拉箭头：hover 高亮整格，箭头本身用向下字形
        if (hotDrop_) ui::FillRectColor(mem, dropRc_, theme_->hover);
        ui::DrawChevron(mem, (dropRc_.left + dropRc_.right) / 2,
                        (dropRc_.top + dropRc_.bottom) / 2, theme_->iconChevron(), theme_->text, 1);

        // 边框最后描（否则被框内填充盖掉）
        ui::FrameRectColor(mem, fieldRc_, theme_->editBorder);
        // 搜索框的边框由本栏画（EDIT 自己只有内容区，没有边框）。
        // 与地址框同色同粗：两个输入框并排时边框不一致会显得像坏了一个。
        if (searchVisible_) ui::FrameRectColor(mem, searchRc_, theme_->editBorder);
    });
}
