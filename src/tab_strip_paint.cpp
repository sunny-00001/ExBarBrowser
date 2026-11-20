// 标签栏绘制：底/悬停/激活底色、图标、标题、关闭钮、锁定字形、溢出箭头、新建 +。
//
// 只读 items_/tabRects_ 等已算好的状态（tab_strip_layout 负责几何），不改任何成员。
// 所有绘制走 ui:: 原语（双缓冲 / 分隔线 / 填充 / 画图标 / 量字），本文件零 GDI 句柄管理。
#include "tab_strip.h"

#include <algorithm>   // std::min（焦点矩形右侧裁剪）

#include "ui_draw.h"

void TabStrip::Paint(HWND h) {
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

            // 底色优先级：激活标签 > 悬停/拖放目标 > 标签自身颜色。
            // 彩色标签在悬停时仍要看得出"被指到了"，所以悬停色盖过颜色；
            // 但激活标签的颜色要靠底部色条表达（否则激活色会把标签色完全盖掉）。
            if (isActive)
                FillRect(mem, &r, theme_->brBg);
            else if (isHover || isDrop)
                FillRect(mem, &r, theme_->brHover);
            else if (items_[i].colorFill != CLR_INVALID)
                ui::FillRectColor(mem, r, items_[i].colorFill);

            if (!isActive && i + 1 < tabRects_.size()) {
                RECT vr{r.right - 1, theme_->S(7), r.right - 1, r.bottom - theme_->S(7)};
                ui::DrawSeparator(mem, vr, true, theme_->line);
            }

            int textLeft = r.left + theme_->S(10);
            if (items_[i].icon) {
                const int iconPx = theme_->iconSmall();
                RECT ir{r.left + theme_->S(8), r.top, r.left + theme_->S(8) + iconPx, r.bottom};
                ui::DrawIconCentered(mem, items_[i].icon, ir, iconPx);
                textLeft = r.left + theme_->S(8) + iconPx + theme_->S(6);
            }

            // 字体沿用外层已选的 theme_->font（传 nullptr 表示不换字体），
            // 颜色/文本/还原交给原语统一处理。
            const RECT tr{textLeft, r.top, r.right - theme_->S(26), r.bottom};
            ui::DrawTextRow(mem, nullptr, items_[i].title, tr,
                            isActive ? theme_->text : theme_->sub,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

            // 部分可见的标签（右缘在 » 之后）如果照画关闭钮，会把 X 画进溢出按钮区。
            const RECT& crc = closeRects_[i];
            const bool closeInView = crc.right > 0 && crc.left < clipR;
            if (items_[i].locked) {
                if (closeInView)
                    ui::DrawGlyph(mem, ico::IconId::Lock, crc, theme_->iconGlyph(), theme_->sub);
            } else if (closeInView && CloseVisible(static_cast<int>(i))) {
                ui::DrawGlyph(mem, ico::IconId::Close, crc, theme_->iconGlyph(),
                              (hoverKind_ == kClose && hoverIndex_ == static_cast<int>(i))
                                  ? theme_->text
                                  : theme_->sub);
            }

            // 底部色条：
            //   激活标签 -> 通栏色条（有颜色用标签色，否则用主题 accent，保持原观感）
            //   非激活但着色的标签 -> 内缩细色条
            // 为什么非激活也要画：26% 的底色在深色主题下太弱，只靠底色分不出组。
            // 为什么内缩：通栏是"当前标签"的专属信号（原有约定），不能因为它有色就模糊。
            if (isActive) {
                RECT ul{r.left, r.bottom - theme_->S(2), r.right, r.bottom};
                ui::FillRectColor(mem, ul, items_[i].colorBar != CLR_INVALID
                                               ? items_[i].colorBar
                                               : theme_->accent);
            } else if (items_[i].colorBar != CLR_INVALID) {
                RECT ul{r.left + theme_->S(4), r.bottom - theme_->S(2),
                        r.right - theme_->S(4), r.bottom};
                ui::FillRectColor(mem, ul, items_[i].colorBar);
            }
        }

        // 溢出按钮：用下箭头（ChevronDown）而不是 0xE76C（那是"更多/省略号"，
        // 与书签栏溢出按钮撞脸，也表达不出"滚动到看不见的标签"）。
        if (overflow_) {
            if (hoverKind_ == kOverflow) FillRect(mem, &ovfRc_, theme_->brHover);
            ui::DrawGlyph(mem, ico::IconId::Overflow, ovfRc_, theme_->iconGlyph(), theme_->sub);
        }

        // 新建标签 +
        if (hoverKind_ == kNew) FillRect(mem, &newRect_, theme_->brHover);
        ui::DrawGlyph(mem, ico::IconId::Add, newRect_, theme_->iconGlyph(),
                      hoverKind_ == kNew ? theme_->text : theme_->sub);

        // 键盘焦点可视：Shift+←/→、Home/End 等裸键的作用域就是标签栏
        // （SetFocus(h) 在 WM_LBUTTONDOWN 里），画点线框让焦点位置始终看得见。
        // 放在最后画，避免被后续标签底色覆盖。
        if (GetFocus() == h && active_ >= 0 &&
            active_ < static_cast<int>(tabRects_.size())) {
            RECT fr = tabRects_[static_cast<size_t>(active_)];
            if (fr.right > 0 && fr.left < clipR) {
                fr.right = std::min<LONG>(fr.right, clipR);
                DrawFocusRect(mem, &fr);   // XOR 点线框，深/浅主题都醒目
            }
        }

        SelectObject(mem, oldFont);
    });
}
