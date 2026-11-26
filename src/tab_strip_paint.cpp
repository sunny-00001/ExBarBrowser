// 标签栏绘制：底/悬停/激活底色、图标、标题、关闭钮、锁定字形、溢出箭头、新建 +。
//
// 只读 items_/tabRects_ 等已算好的状态（tab_strip_layout 负责几何），不改任何成员。
// 所有绘制走 ui:: 原语（双缓冲 / 分隔线 / 填充 / 画图标 / 量字），本文件零 GDI 句柄管理。
#include "tab_strip.h"

#include "ui_draw.h"

void TabStrip::Paint(HWND h) {
    const int clipRight = overflow_ ? tabViewRight_ : 0;

    ui::PaintBuffered(h, theme_->bar, [&](HDC mem, const RECT& rc) {
        ui::ScopedFont font(mem, theme_->font);
        const int clipR = clipRight ? clipRight : rc.right;

        for (size_t i = 0; i < tabRects_.size(); ++i) {
            // 让位动画：按 AnimDx 偏移一格；正在拖的那格在原槽留空，
            // 由 DrawDragGhost 跟随光标画（见 tab_strip_anim.cpp）。
            RECT r = tabRects_[i];
            const int dx = AnimDx(static_cast<int>(i));
            if (dx) OffsetRect(&r, dx, 0);
            if (r.right <= 0 || r.left >= clipR) continue;
            if (dragging_ && static_cast<int>(i) == dragIdx_) continue;
            bool isActive = static_cast<int>(i) == active_;
            bool isHover = hoverKind_ == kTab && hoverIndex_ == static_cast<int>(i);

            // 底色优先级：激活标签 > 悬停 > 标签自身颜色。
            // 拖拽目标不再整格刷底色：那读起来像"这个标签被选中了"，而用户真正要看的
            // 是插到哪一侧——改由底部那条竖线表达（DropLineX，见下方统一绘制）。
            if (isActive)
                FillRect(mem, &r, theme_->brBg);
            else if (isHover)
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

            // 固定标签只画图标（宽度已压到 40dp）：标题画进去也只会是省略号，
            // 反而挤掉图标。悬停提示（tooltip）仍给完整路径。
            if (!items_[i].pinned) {
                const RECT tr{textLeft, r.top, r.right - theme_->S(26), r.bottom};
                ui::DrawTextRow(mem, nullptr, items_[i].title, tr,
                                isActive ? theme_->text : theme_->sub,
                                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // 部分可见的标签（右缘在 » 之后）如果照画关闭钮，会把 X 画进溢出按钮区。
            RECT crc = closeRects_[i];
            if (dx) OffsetRect(&crc, dx, 0);
            const bool closeInView = crc.right > 0 && crc.left < clipR;
            if (items_[i].pinned) {
                // 固定标签没有关闭钮，用图钉图标占位（恒定可见，不靠悬停）。
                if (closeInView)
                    ui::DrawGlyph(mem, ico::IconId::Pin, crc, theme_->iconGlyph(), theme_->sub);
            } else if (items_[i].locked) {
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

        // 拖拽插入指示：tear-off 已进入"移到新窗口"模式时不画（此时松手不重排）。
        if (!tearOff_) {
            if (const int dropX = DropLineX(); dropX >= 0 && dropX < rc.right) {
                const int half = theme_->S(2);
                RECT dl{dropX - half, theme_->S(2), dropX + half, rc.bottom - theme_->S(2)};
                ui::FillRectColor(mem, dl, theme_->accent);
            }
        }
        // 被拖标签跟手；tear-off 时改成整条提示（此时浮层没有意义，
        // 而且整条换成 brHover 能明确告诉用户"松手是另一件事"）。
        RECT viewClip{0, 0, clipR, rc.bottom};
        if (tearOff_ && dragging_) {
            FillRect(mem, &viewClip, theme_->brHover);
            ui::DrawTextRow(mem, nullptr, L"松开：移到新窗口", viewClip, theme_->accent,
                            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        } else {
            DrawDragGhost(mem, viewClip);
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
    });
}
