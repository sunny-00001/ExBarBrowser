// 序列弹层的绘制（与参考 mod 的 SeqUiPaint 同构）。
//
// 【布局】列表视口在上、输入区在下（Listary 风格），弹层贴主窗口客户区右下角。
//   列表：序号 │ 序列键 │ 描述 │ 命令 │ 分组（5 列；非序列键模式退化为 序号 │ 名称 │ 说明）
//   输入：图标 │ 文本 │ 闪烁光标 │ 钉住 │ 清空
//
// 【色板】全部取 Theme 令牌（颜色唯一来源在 ui_palette.h）：
//   列表底 = listBg；输入区底 = editBg；分隔线 = line；
//   序列键 / 输入文本 / 光标 = accent；命令 / 分组 = sub；选中行 = selSolid 实心强调色。
#include <algorithm>

#include "icon_glyph.h"
#include "icon_resolve.h"
#include "sequence_overlay.h"
#include "ui_draw.h"

namespace {

// 模式 → 输入区图标（与原版 kSeqModes[].icon 同口径：图标语义随模式变）。
ico::IconId ModeIcon(const std::wstring& mode) {
    if (mode == SequenceOverlay::kSeqKeyMode) return ico::IconId::Keyboard;
    if (mode == L"书签") return ico::IconId::Folder;
    if (mode == L"标签") return ico::IconId::Tag;
    if (mode == L"搜索") return ico::IconId::Search;
    if (mode == L"命令") return ico::IconId::GridView;
    if (mode == L"快速跳转") return ico::IconId::Forward;
    if (mode == L"当前目录") return ico::IconId::Folder;
    if (mode == L"帮助") return ico::IconId::More;
    return ico::IconId::Keyboard;
}

// 面板底色 × 面板文字按比例混合（滚动条槽 / 滑块用；与原版 mix 口径一致）。
COLORREF Mix(COLORREF a, COLORREF b, int ratio) {
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * ratio / 256,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * ratio / 256,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * ratio / 256);
}

}  // namespace

void SequenceOverlay::Paint(HWND h) {
    const seqov::Metrics& m = metrics_;
    ui::PaintBuffered(h, theme_->listBg, [&](HDC mem, const RECT& rc) {
        const int totalRows = static_cast<int>(filtered_.size());
        const int viewRows = ViewRows();
        const int listH = m.rowH * viewRows;
        const int contentH = m.rowH * totalRows;
        const bool scrollable = contentH > listH;

        ui::ScopedFont font(mem, theme_->font);

        // ---- 5 列几何 ----
        // 序号列宽按**总行数**的位数算；序列键列宽按**全部**序列键里最长 token 算
        // （keyChars_，与过滤结果无关）—— 否则过滤一变列宽就跳。
        const bool wide = !filtered_.empty() && IsWideRow(filtered_[0]);
        // 等宽字体缺失时回退正文字体（列位仍算得出来，只是不等宽）。
        HFONT mono = theme_->fontMono ? theme_->fontMono : theme_->font;
        HFONT monoBig = theme_->fontMonoBig ? theme_->fontMonoBig : theme_->font;
        const int monoW = std::max(1, ui::MeasureText(mem, mono, L"0"));
        const seqov::Cols cols = seqov::ComputeCols(
            totalRows, monoW, wide ? keyChars_ : 1, m.winW, m.pad, m.colGap, m.numX,
            m.numGap, m.cmdW, m.grpW, m.scrollW, scrollable);

        // 模式列表（**唤起态**：Ctrl+Shift+P 且没选模式）走另一套列位：
        //   序号 │ 名称(含前缀) │ 说明（右对齐，固定宽 146）
        //
        // 【别和 `?` 帮助模式混】原版里这是两张不同的表：唤起态是"模式列表"
        // （SeqShowingModeList，三列、无图标槽）；`?` 帮助模式是**目标列表**
        // （SeqListHelp，图标槽 + 副标题）。原版同款。
        const bool modeList = (mode_ == SequenceOverlay::kUserMode);
        const seqov::ModeCols mc = seqov::ComputeModeCols(
            totalRows, monoW, m.winW, m.pad, m.numX, m.numGap, m.scrollW, scrollable,
            m.hintW, m.titleReserve);
        const bool modeCols = !wide && modeList;
        // 目标列表列位：序号 │ 图标槽 │ 名称 │ 副标题 │ [分组][触发]。
        const seqov::TargetCols tc = seqov::ComputeTargetCols(
            totalRows, monoW, m.winW, m.pad, m.numX, m.numGap, m.scrollW, scrollable,
            m.iconSz, m.iconGap, m.pathW);

        // ---- 列表区 ----
        for (int i = static_cast<int>(scroll_); i < totalRows && i < static_cast<int>(scroll_) + viewRows;
             ++i) {
            const Row& row = filtered_[static_cast<size_t>(i)];
            const int y = (i - static_cast<int>(scroll_)) * m.rowH;
            const bool sel = (static_cast<size_t>(i) == selected_);
            const bool hov = (i == hover_);
            if (sel) {
                // 选中行：整行实心强调色（原版 selBg = selectionBg）。
                ui::FillRectColor(mem, RECT{0, y, cols.right, y + m.rowH}, theme_->selSolid);
            } else if (hov) {
                ui::FillRectColor(mem, RECT{0, y, cols.right, y + m.rowH}, theme_->listHot);
            }
            const COLORREF fg = sel ? theme_->selSolidFg : theme_->text;
            const COLORREF muted = sel ? theme_->selSolidFg : theme_->inert;
            const COLORREF keyClr = sel ? theme_->selSolidFg : theme_->accent;

            // 序号（等宽，左对齐）
            RECT rn{m.numX, y, m.numX + cols.numW, y + m.rowH};
            ui::DrawTextRow(mem, mono, std::to_wstring(i + 1), rn, muted,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (wide) {
                // 序列键（等宽，强调色）
                RECT rk{cols.keyX, y, cols.keyX + cols.keyW, y + m.rowH};
                ui::DrawTextRow(mem, mono, row.key, rk, keyClr,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                    DT_END_ELLIPSIS);
                // 描述（命令中文名）
                RECT rd{cols.titleX, y, cols.cmdX - m.colGap, y + m.rowH};
                ui::DrawTextRow(mem, theme_->font, row.title, rd, fg,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                    DT_PATH_ELLIPSIS);
                // 命令（英文 id；等宽右对齐）
                RECT rc2{cols.cmdX, y, cols.cmdX + cols.cmdW, y + m.rowH};
                ui::DrawTextRow(mem, mono, row.cmdId, rc2, muted,
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                    DT_END_ELLIPSIS);
                // 分组（中文；最右，右对齐）
                RECT rg{cols.grpX, y, cols.right - m.pad, y + m.rowH};
                ui::DrawTextRow(mem, theme_->font, row.group, rg, muted,
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            } else if (modeCols) {
                // 模式列表（原版 SeqShowingModeList）：
                //   序号 │ 名称(含前缀) │ 说明（右对齐，固定宽 146）
                // 名称列左界 = 序列键列位置（原版 titleX = keyX），两种列表视觉节奏一致。
                RECT rt{mc.titleX, y, mc.titleRight, y + m.rowH};
                ui::DrawTextRow(mem, theme_->font, row.title, rt, fg,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                    DT_PATH_ELLIPSIS);
                RECT rs{mc.hintX, y, mc.hintRight, y + m.rowH};
                ui::DrawTextRow(mem, theme_->font, row.sub, rs, muted,
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                    DT_PATH_ELLIPSIS);
            } else {
                // 目标列表（原版 SeqShowingTargetList）：
                //   序号 │ 图标槽 │ 名称 │ 副标题 │ [分组][触发]
                // 触发列宽按**本行**文本量取，分组列让位到它左侧（原版同款）。
                //
                // 【「未绑定」】原版只在**高亮行**显示一次"未绑定"：整屏都写是
                // 噪音（绝大多数命令都没绑定），而空列又看不出是状态还是留白。
                std::wstring trigText = row.trigger;
                if (trigText.empty() && sel && row.isCommand) trigText = L"未绑定";
                const int trigW = trigText.empty()
                                      ? 0
                                      : ui::MeasureText(mem, theme_->font, trigText);
                const int grpW = (!row.group.empty() && trigW > 0)
                                     ? ui::MeasureText(mem, theme_->font, row.group)
                                     : 0;
                const seqov::TargetRight tr =
                    seqov::ComputeTargetRight(tc, m.pad, m.grpGap, trigW, grpW);
                // 图标槽：恒预留（原版「无图标就留白，下一帧数据到了自然就有了」）。
                // 只有路径型行有图标（命令行/帮助行恒空 —— 原版同款，
                // 否则每条命令都会挂一个通用文件夹图标）。
                if (!row.iconPath.empty()) {
                    HICON hi = ico::Cached(row.iconPath, true);
                    if (hi) {
                        RECT ri{tc.iconX, y, tc.iconX + tc.iconSz, y + m.rowH};
                        ui::DrawIconCentered(mem, hi, ri, tc.iconSz);
                    }
                }
                // 名称（列起点固定在图标槽之后；没有图标也留白 —— 原版同款）。
                RECT rt{tc.nameX, y, tr.nameRight, y + m.rowH};
                ui::DrawTextRow(mem, theme_->font, row.title, rt, fg,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                    DT_PATH_ELLIPSIS);
                // 副标题：跟在名称之后（点分名 / 父目录），次要色。
                if (!row.subtitle.empty()) {
                    const int titleW = ui::MeasureText(mem, theme_->font, row.title);
                    RECT rs{tc.nameX + titleW + m.colGap, y, tr.nameRight, y + m.rowH};
                    ui::DrawTextRow(mem, theme_->font, row.subtitle, rs, muted,
                                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                        DT_PATH_ELLIPSIS);
                }
                if (trigW > 0) {
                    // 触发键（最右，右对齐）。
                    RECT rk{tr.trigX, y, tc.right - m.pad, y + m.rowH};
                    ui::DrawTextRow(mem, theme_->font, trigText, rk, muted,
                                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                        DT_PATH_ELLIPSIS);
                    if (grpW > 0) {
                        // 分组（触发键左侧，右对齐）。
                        RECT rg{tr.grpX, y, tr.trigX - m.grpGap, y + m.rowH};
                        ui::DrawTextRow(mem, theme_->font, row.group, rg, muted,
                                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    }
                } else if (!row.rightText.empty()) {
                    // 无触发键 → 右列放路径 / 目标（固定宽 pathW，右对齐）。
                    RECT rp{tc.right - m.pad - tc.pathW, y, tc.right - m.pad, y + m.rowH};
                    ui::DrawTextRow(mem, theme_->font, row.rightText, rp, muted,
                                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                        DT_PATH_ELLIPSIS);
                }
            }
        }
        if (filtered_.empty()) {
            // 空态：不能静默空白（ARIA 规范）；居中一行说明（原版用 muted 色）。
            RECT empty{0, 0, cols.right, listH};
            ui::DrawTextRow(mem, theme_->font, L"无匹配项", empty, theme_->inert,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        // ---- 输入区（列表正下方，Listary 风格）：独立底色 + 顶部分隔线 ----
        const RECT ir{0, listH, m.winW, listH + m.bufH};
        ui::FillRectColor(mem, ir, theme_->editBg);
        ui::DrawSeparator(mem, RECT{ir.left, ir.top, ir.right, ir.top + 1}, false, theme_->line);

        // 图标：当前模式的芯片。
        // 【唤起态不画图标】原版判据是 g_seqIcon —— 唤起时图标**不亮**
        // （"图标不在"正是模式列表的判据），敲第一个字符才点亮。
        // 图标左边线与列表「序号」列对齐（列表里不画图标，就是为了让这处对齐）。
        const int iconW = theme_->iconGlyph();
        const bool chipOn = (mode_ != SequenceOverlay::kUserMode);
        if (chipOn) {
            RECT rcIcon{m.numX, ir.top, m.numX + iconW, ir.bottom};
            ui::DrawGlyph(mem, ModeIcon(mode_), rcIcon, iconW, theme_->accent,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // 输入文本：filter 原样显示；空就是空（绝不用提示文案顶替）。
        // 序列键模式下 filter 是紧凑前缀（"g g" → "gg"，原版 SeqCompact 口径）。
        std::wstring shown = filter_;
        if (mode_ == kSeqKeyMode) {
            std::wstring compact;
            for (wchar_t c : shown)
                if (c != L' ') compact.push_back(c);
            shown = compact;
        }
        // 有芯片时让开左侧图标（与列表键列起点对齐）；唤起态无芯片 → 贴左边 1px。
        const int tx = chipOn ? (1 + cols.keyX) : 1;
        const int textRight = seqov::BtnsLeft(m);
        RECT rt{tx, ir.top, textRight, ir.bottom};
        if (!shown.empty()) {
            ui::DrawTextRow(mem, monoBig, shown, rt, theme_->accent,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                DT_PATH_ELLIPSIS);
        }
        // 闪烁光标：定时器周期重绘输入区（对齐原版看门狗 tick）。
        const UINT blink = GetCaretBlinkTime();
        if (blink != INFINITE && (GetTickCount() / (blink ? blink : 530)) % 2 == 0) {
            int carX = tx;
            if (!shown.empty()) {
                const int w = ui::MeasureText(mem, monoBig, shown);
                carX += std::min(w, textRight - tx - m.colGap);
            }
            carX += m.caretGap;   // 与文本留一点间隙（原版 SX(3)）
            const int carMax = textRight - m.caretMaxGap;
            if (carX > carMax) carX = carMax;
            RECT car{carX, ir.top + m.caretInset, carX + std::max(1, m.caretW),
                     ir.bottom - m.caretInset};
            ui::FillRectColor(mem, car, theme_->accent);
        }

        // 输入框右侧按钮：钉住（其左）/ 清空（最右）。悬停才给底衬。
        {
            const RECT pin = PinBtnRect();
            const RECT clr = ClearBtnRect();
            ui::DrawGlyph(mem, ico::IconId::Pin, pin, iconW,
                          pinned_ ? theme_->accent : theme_->sub,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ui::DrawGlyph(mem, ico::IconId::Close, clr, iconW, theme_->sub,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // ---- 滚动条（内容超出一屏才有）----
        if (scrollable) {
            RECT slot{cols.right, 0, m.winW, listH};
            ui::FillRectColor(mem, slot, Mix(theme_->listBg, theme_->text, 4));
            // 滑块：高度按内容比例，最小 30px；位置按滚动偏移。
            const int maxScroll = contentH - listH;
            int thumbH = contentH > 0 ? listH * listH / contentH : listH;
            thumbH = std::max(m.thumbMin, thumbH);
            thumbH = std::min(thumbH, listH);
            const int scrollPx = static_cast<int>(scroll_) * m.rowH;
            const int thumbY = maxScroll > 0
                                   ? (listH - thumbH) * scrollPx / maxScroll
                                   : 0;
            RECT thumb{slot.left + m.scrollPadL, thumbY, slot.right - m.scrollPadR,
                       thumbY + thumbH};
            ui::FillRectColor(mem, thumb, Mix(theme_->listBg, theme_->text, 64));
        }

        // ---- 外边框（1px）----
        ui::FrameRectColor(mem, RECT{0, 0, rc.right - 1, rc.bottom - 1}, theme_->panelBorder);
    });
}
