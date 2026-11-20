// 标签栏绘制：底/悬停/激活底色、图标、标题、关闭钮、锁定字形、溢出箭头、新建 +。
//
// 只读 items_/tabRects_ 等已算好的状态（tab_strip_layout 负责几何），不改任何成员。
// 所有绘制走 ui:: 原语（双缓冲 / 分隔线 / 填充 / 画图标 / 量字），本文件零 GDI 句柄管理。
#include "tab_strip.h"

#include "ui_draw.h"

namespace {

// 关闭钮的 ✕ 是描边折线（不是字形）：10px 见方的 Fluent 字形在小尺寸下发虚，
// 折线在 1px/2px 下更锐利。这是**唯一**保留的折线图标，其余一律走字形表。
void DrawCross(HDC dc, const RECT& rc, COLORREF color, int thickness) {
    HPEN pen = CreatePen(PS_SOLID, thickness, color);
    HGDIOBJ old = SelectObject(dc, pen);
    int pad = thickness * 2;
    MoveToEx(dc, rc.left + pad, rc.top + pad, nullptr);
    LineTo(dc, rc.right - pad, rc.bottom - pad);
    MoveToEx(dc, rc.right - pad, rc.top + pad, nullptr);
    LineTo(dc, rc.left + pad, rc.bottom - pad);
    SelectObject(dc, old);
    DeleteObject(pen);
}

// 新建标签的 "+" 同理用两条线（比字形更锐利），复用 Separator 原语。
void DrawPlus(HDC dc, const RECT& rc, COLORREF color, int thickness) {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    const int s = (rc.right - rc.left) / 2 - 4;
    if (s <= 0) return;
    RECT h{cx - s, cy, cx + s + 1, cy};
    RECT v{cx, cy - s, cx, cy + s + 1};
    ui::DrawSeparator(dc, h, false, color, thickness);
    ui::DrawSeparator(dc, v, true, color, thickness);
}

}  // namespace

void TabStrip::Paint(HWND h) {
    const int line = std::max(1, theme_->S(1));
    const int clipRight = overflow_ ? ovfRc_.left - theme_->S(2) : 0;

    ui::PaintBuffered(h, theme_->bar, [&](HDC mem, const RECT& rc) {
        HGDIOBJ oldFont = SelectObject(mem, theme_->font);
        const int clipR = clipRight ? clipRight : rc.right;

        for (size_t i = 0; i < tabRects_.size(); ++i) {
            const RECT& r = tabRects_[i];
            if (r.right <= 0 || r.left >= clipR) continue;
            bool isActive = static_cast<int>(i) == active_;
            bool isHover = hoverKind_ == kTab && hoverIndex_ == static_cast<int>(i);
            bool isDrop = dragging_ && dropIdx_ == static_cast<int>(i);

            if (isActive)
                FillRect(mem, &r, theme_->brBg);
            else if (isHover || isDrop)
                FillRect(mem, &r, theme_->brHover);

            if (!isActive && i + 1 < tabRects_.size()) {
                RECT vr{r.right - 1, theme_->S(7), r.right - 1, r.bottom - theme_->S(7)};
                ui::DrawSeparator(mem, vr, true, theme_->line);
            }

            int textLeft = r.left + theme_->S(10);
            if (items_[i].icon) {
                int iconPx = theme_->S(16);
                RECT ir{r.left + theme_->S(8), r.top, r.left + theme_->S(8) + iconPx, r.bottom};
                ui::DrawIconCentered(mem, items_[i].icon, ir, iconPx);
                textLeft = r.left + theme_->S(8) + iconPx + theme_->S(6);
            }

            RECT tr{textLeft, r.top, r.right - theme_->S(26), r.bottom};
            SetTextColor(mem, isActive ? theme_->text : theme_->sub);
            DrawTextW(mem, items_[i].title.c_str(), -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

            // 部分可见的标签（右缘在 » 之后）如果照画关闭钮，会把 X 画进溢出按钮区。
            const RECT& crc = closeRects_[i];
            const bool closeInView = crc.right > 0 && crc.left < clipR;
            if (items_[i].locked) {
                if (closeInView) ui::DrawGlyph(mem, ico::IconId::Lock, crc, theme_->S(11), theme_->sub);
            } else if (closeInView && CloseVisible(static_cast<int>(i))) {
                DrawCross(mem, closeRects_[i],
                          (hoverKind_ == kClose && hoverIndex_ == static_cast<int>(i))
                              ? theme_->text
                              : theme_->sub,
                          line);
            }

            if (isActive) {
                RECT ul{r.left, r.bottom - theme_->S(2), r.right, r.bottom};
                ui::FillRectColor(mem, ul, theme_->accent);
            }
        }

        // 溢出按钮：用下箭头（ChevronDown）而不是 0xE76C（那是"更多/省略号"，
        // 与书签栏溢出按钮撞脸，也表达不出"滚动到看不见的标签"）。
        if (overflow_) {
            if (hoverKind_ == kOverflow) FillRect(mem, &ovfRc_, theme_->brHover);
            ui::DrawGlyph(mem, ico::IconId::Overflow, ovfRc_, theme_->S(11), theme_->sub);
        }

        // 新建标签 +
        if (hoverKind_ == kNew) FillRect(mem, &newRect_, theme_->brHover);
        DrawPlus(mem, newRect_, hoverKind_ == kNew ? theme_->text : theme_->sub, line);

        SelectObject(mem, oldFont);
    });
}
