// 快捷键管理器：顶部工具栏的绘制。
//
// 【为什么单独一个文件】工具栏是"所有动作的入口面"，它的绘制规则与中栏/右栏
// 不是一回事：按钮有四种态（禁用 / 悬停 / 主操作常驻 / 普通）、有分隔符、
// 右端还有统计。混在 draw.cpp 里时，改一处按钮态要跨过 200 行行绘制。
//
// 【几何只从 toolRects_ 来】布局（layout.cpp 的 LayoutToolbar）算好矩形，
// 这里按它画，HitTool 按它判命中 —— 三处共用一份，不存在"画在这里、点在那里"。
#include "hotkey_manager.h"

#include "ui_draw.h"

void HotkeyManager::PaintToolbar(HDC dc) {
    // 工具栏是通栏的，底色比内容区高一档（barAlt = editBg）：
    // 不这样的话它与下面的三栏糊成一片，"这是一条可点的条"这个读法就没有了。
    ui::FillRectColor(dc, toolbar_, theme_->barAlt);
    RECT sep{toolbar_.left, toolbar_.bottom - 1, toolbar_.right, toolbar_.bottom};
    ui::DrawSeparator(dc, sep, false, theme_->line);

    int n = 0;
    const ToolButton* items = ToolbarItems(&n);
    const int iconPx = theme_->iconSmall();
    const int radius = theme_->radiusBtn();

    // 分隔符：矩形在布局里就算好了（id = kToolNone 的那几条），这里照画。
    for (const ToolRect& tr : toolRects_) {
        if (tr.id == kToolNone) ui::DrawSeparator(dc, tr.rc, true, theme_->line);
    }

    for (int i = 0; i < n; ++i) {
        const ToolButton& b = items[i];
        if (b.id == kToolNone) continue;   // 分隔符已在上面画过
        // 分隔符占了一个下标但不在 toolRects_ 里，所以按下标取矩形会错位 ——
        // 按 id 找。按钮数是个位数，线性找比维护映射表更不容易错。
        RECT rc{};
        bool found = false;
        for (const ToolRect& tr : toolRects_) {
            if (tr.id == b.id) { rc = tr.rc; found = true; break; }
        }
        if (!found) continue;

        const bool enabled = ToolEnabled(b.id);
        const bool hot = hoverTool_ == b.id;
        const bool isPrimary = (b.id == kToolEdit);
        // 表开关：当前这张表填强调色。
        //
        // 【为什么用"填色"而不是"按下态"】它表达的是**当前状态**（我在看哪张表），
        // 与录制按钮开着时整颗转强调色是同一套读法。反过来用"凹陷/描边"表达，
        // 用户得先知道"这两颗是互斥开关"才看得懂。
        const bool isTab = (b.id == kToolTabChord || b.id == kToolTabSeq);
        const bool on = (b.id == kToolTabChord && tableType_ == kTypeChord) ||
                        (b.id == kToolTabSeq && tableType_ == kTypeSeq);

        if (isTab) {
            if (on) {
                ui::FillRound(dc, rc, theme_->accent, radius);
            } else if (hot) {
                ui::FillRound(dc, rc, theme_->hover, radius);
            } else {
                // 未选中的那颗也给一层底与描边：两颗并排时，只有一颗有形状会让
                // 人以为另一颗是标签而不是按钮。
                ui::FillRound(dc, rc, theme_->editBg, radius);
                ui::FrameRectColor(dc, rc, theme_->editBorder);
            }
        } else if (!enabled) {
            // 禁用态不画底：一条灰字已经说明"现在不能用"，
            // 再给它一块底会让人以为可以点。
        } else if (hot) {
            ui::FillRound(dc, rc, isPrimary ? theme_->accent : theme_->hover, radius);
        } else if (isPrimary) {
            // 主操作常驻强调色描边：它是这个面板 90% 的动作，
            // 不能要求用户先悬停才发现"这里能点"。
            ui::FillRound(dc, rc, theme_->editBg, radius);
            ui::FrameRectColor(dc, rc, theme_->accent);
        }

        COLORREF fg = theme_->text;
        if (isTab) fg = on ? theme_->selSolidFg : theme_->sub;
        else if (!enabled) fg = theme_->disabled;
        else if (hot && isPrimary) fg = theme_->selSolidFg;
        else if (isPrimary) fg = theme_->accent;

        const int padIn = theme_->S(4);
        RECT ir{rc.left + padIn, rc.top + (rc.bottom - rc.top - iconPx) / 2,
                rc.left + padIn + iconPx, rc.top + (rc.bottom - rc.top - iconPx) / 2 + iconPx};
        ui::DrawGlyph(dc, b.icon, ir, iconPx, fg);
        if (b.text && *b.text) {
            RECT tr{ir.right + theme_->S(6), rc.top, rc.right - padIn, rc.bottom};
            ui::DrawTextRow(dc, theme_->font, b.text, tr, fg,
                            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    // ---- 右端：搜索框 + 录制按钮 ----
    //
    // 【为什么搜索框在这里而不是中栏顶部】它原来独占中栏顶部一整行（~34px），
    // 而那一行只放了一个输入框。搬到工具栏后：① 列表多出整整一行的高度；
    // ② "找到它 → 改它"落在同一条视觉带上，眼睛不用上下跳两次。
    const int searchRadius = theme_->radiusBtn();
    if (searchField_.right > searchField_.left) {
        ui::FillRound(dc, searchField_, theme_->barAlt, searchRadius);
        ui::FrameRectColor(dc, searchField_, recording_ ? theme_->accent : theme_->editBorder);
        const int sipx = theme_->iconSmall();
        RECT icon{searchField_.left + theme_->S(8),
                  searchField_.top + (searchField_.bottom - searchField_.top - sipx) / 2,
                  searchField_.left + theme_->S(8) + sipx,
                  searchField_.top + (searchField_.bottom - searchField_.top - sipx) / 2 + sipx};
        ui::DrawGlyph(dc, ico::IconId::Search, icon, sipx, theme_->sub);
    }
    if (recordBtn_.right > recordBtn_.left) {
        // 录制按钮：开了之后按键不再进搜索框，而是被译成组合键做筛选
        // （VS Code 的 Record Keys）。开着时整颗转成强调色，不可能看错。
        if (recording_) {
            ui::FillRound(dc, recordBtn_, theme_->accent, searchRadius);
        } else if (hoverRecord_) {
            ui::FillRound(dc, recordBtn_, theme_->rowHover, searchRadius);
        } else {
            ui::FillRound(dc, recordBtn_, theme_->barAlt, searchRadius);
        }
        if (!recording_) ui::FrameRectColor(dc, recordBtn_, theme_->editBorder);
        const int kpx = theme_->iconSmall();
        RECT kir{recordBtn_.left + (recordBtn_.right - recordBtn_.left - kpx) / 2,
                 recordBtn_.top + (recordBtn_.bottom - recordBtn_.top - kpx) / 2,
                 recordBtn_.left + (recordBtn_.right - recordBtn_.left - kpx) / 2 + kpx,
                 recordBtn_.top + (recordBtn_.bottom - recordBtn_.top - kpx) / 2 + kpx};
        ui::DrawGlyph(dc, ico::IconId::Keyboard, kir, kpx,
                      recording_ ? theme_->selSolidFg : theme_->sub);
    }

    // 左端统计与搜索框之间的空白：放"自定义 N 条 · 冲突 N"。
    // 它贴在搜索框左边（而不是最右端），因为最右端已经被搜索框占了。
    std::wstring right = L"自定义 " + std::to_wstring(totalCustom_) + L" 条";
    if (totalConflict_) right += L" · 冲突 " + std::to_wstring(totalConflict_);
    const int statRight = searchField_.right > searchField_.left
                              ? searchField_.left - theme_->S(12)
                              : toolbar_.right - theme_->S(12);
    RECT rr{toolbar_.left, toolbar_.top, statRight, toolbar_.bottom};
    ui::DrawTextRow(dc, theme_->font, right, rr,
                    totalConflict_ ? theme_->danger : theme_->inert,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------- 左栏
