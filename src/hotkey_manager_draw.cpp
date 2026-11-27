// 快捷键管理器：绘制。
//
// 【只用一套原语】颜色来自 Theme、量字走 ui::MeasureText、画字形走 ui::DrawGlyph、
// 分隔线走 ui::DrawSeparator、圆角底板走 ui::FillRound、双缓冲走 ui::PaintBuffered。
// 自己写一份就会与项目其余部分分叉 —— 而分叉的表现是"这个窗口和别处不是一个软件"。
//
// 【几何来自布局】行/列矩形在 hotkey_manager_layout.cpp 里算好，这里只按那份矩形画。
// 绘制与命中共用一份几何：两处各算一遍迟早错位。
#include "hotkey_manager.h"

#include <algorithm>

#include "chord.h"
#include "str.h"
#include "ui_draw.h"

namespace {

// 状态列文案。序列键优先于"可用"：它是另一类东西，不是"这条键能用"。
const wchar_t* StatusText(int status, bool sequence) {
    if (sequence) return L"序列";
    switch (status) {
    case 1: return L"冲突";       // kConflict
    case 2: return L"系统占用";   // kReserved
    case 3: return L"未实现";     // kUnimplemented
    default: return L"可用";
    }
}

}  // namespace

void HotkeyManager::Paint(HWND hwnd) {
    if (!theme_) return;
    ui::PaintBuffered(hwnd, theme_->bg, [this](HDC dc, const RECT& rc) {
        (void)rc;
        PaintToolbar(dc);
        PaintRail(dc);
        PaintTable(dc);
        PaintDetail(dc);
        PaintStatus(dc);
    });
}

// ---------------------------------------------------------------- 工具栏

void HotkeyManager::PaintRail(HDC dc) {
    ui::FillRectColor(dc, colA_, theme_->bg);

    // 标题
    RECT tr{colA_.left + theme_->S(16), colA_.top + theme_->S(8), colA_.right - theme_->S(10),
            colA_.top + theme_->S(38)};
    ui::DrawTextRow(dc, theme_->fontBold, L"筛选", tr, theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    // 行高与中栏同一个来源（RowHeight）：左栏以前写死 S(32)、中栏是 S(30)，
    // 两栏的行距肉眼可见地不齐。现在全库只有一个 28。
    const int rh = RowHeight();
    const int radius = theme_->radiusBtn();
    const int gSaved = SaveDC(dc);
    IntersectClipRect(dc, railList_.left, railList_.top, railList_.right, railList_.bottom);
    for (int i = 0; i < static_cast<int>(groups_.size()); ++i) {
        const Group& g = groups_[static_cast<size_t>(i)];
        const int top = railList_.top + i * rh;
        const RECT r{railList_.left + theme_->S(4), top, railList_.right - theme_->S(4), top + rh};
        if (r.bottom <= railList_.top || r.top >= railList_.bottom) continue;
        const bool on = group_ == g.id;
        if (on) {
            ui::FillRound(dc, r, theme_->softSel, radius);
            // 选中项左侧一条实心标记 —— 与设置对话框左栏同一手法（页签的选中边）。
            RECT bar{r.left, r.top + theme_->S(6), r.left + theme_->S(3), r.bottom - theme_->S(6)};
            ui::FillRound(dc, bar, theme_->accent, theme_->S(2));
        } else if (i == hoverGroup_) {
            ui::FillRound(dc, r, theme_->rowHover, radius);
        }

        const int ipx = theme_->iconSmall();
        RECT ir{r.left + theme_->S(10), r.top + (rh - ipx) / 2, r.left + theme_->S(10) + ipx,
                r.top + (rh - ipx) / 2 + ipx};
        ui::DrawGlyph(dc, g.icon, ir, ipx, on ? theme_->accent : theme_->sub);

        const RECT line = ui::CenteredLineRect(r, theme_->S(20));
        RECT nr{ir.right + theme_->S(10), line.top, r.right - theme_->S(46), line.bottom};
        ui::DrawTextRow(dc, on ? theme_->fontBold : theme_->font, g.name, nr,
                        on ? theme_->accent : theme_->text,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        // 有搜索词时显示"命中/总数"：用户才知道"是这一组没有，还是我搜错了"。
        const std::wstring cnt = query_.empty() ? std::to_wstring(g.total)
                                                : (std::to_wstring(g.hits) + L"/" +
                                                   std::to_wstring(g.total));
        RECT cr{r.right - theme_->S(44), line.top, r.right - theme_->S(10), line.bottom};
        ui::DrawTextRow(dc, theme_->font, cnt, cr,
                        (on ? theme_->accent : theme_->inert),
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT);
    }
    RestoreDC(dc, gSaved);

    // 页脚：全局统计（不受筛选影响 —— 筛选时用户仍想知道总量）。
    ui::DrawSeparator(dc, railFoot_, false, theme_->line);
    const std::wstring left = L"共 " + std::to_wstring(totalAll_) + L" 条";
    RECT lr{railFoot_.left + theme_->S(16), railFoot_.top, railFoot_.right - theme_->S(10),
            railFoot_.bottom};
    ui::DrawTextRow(dc, theme_->font, left, lr, theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    // 栏间分隔：左栏与中栏之间一条竖线（与设置对话框同一处理）。
    RECT sep{colA_.right, colA_.top, colA_.right + 1, colA_.bottom};
    ui::DrawSeparator(dc, sep, true, theme_->line);
}

// ---------------------------------------------------------------- 中栏

void HotkeyManager::PaintTable(HDC dc) {
    ui::FillRectColor(dc, colB_, theme_->bg);
    const int pad = theme_->S(12);   // 空结果提示与滚动条的内边距

    // 搜索框与录制按钮已搬到工具栏（见 PaintToolbar），这里不再画。

    // ---- 表头 ----
    //
    // 列矩形读 columns_（LayoutRows 算的那一份）—— 绘制与排序命中同一来源，
    // 不再"从第一行反推"。视图为空时 columns_ 仍然是有效几何（它只依赖
    // body_ 的宽度），所以搜索无结果时表头也不会跳位。
    ui::FillRectColor(dc, tableHead_, theme_->rowHover);
    for (const Column& c : columns_) {
        ui::DrawTextRow(dc, theme_->font, c.title, c.rc, theme_->sub,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // 排序列的指示：列标题换强调色 + 末尾一个上下箭头。
    //
    // 【为什么必须给指示】"我点了表头，列表变了但不知道按什么排的"是
    // 排序功能的常见失败态。箭头小，但它回答了"现在是升还是降"。
    for (const Column& c : columns_) {
        if (c.sortKey != sortKey_) continue;
        ui::DrawTextRow(dc, theme_->font, c.title, c.rc, theme_->accent,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        const int tw = ui::MeasureText(dc, theme_->font, c.title, -1);
        const int arrowPx = theme_->iconChevron();
        const int ax = c.rc.left + tw + theme_->S(3) + arrowPx / 2;
        const int ay = (c.rc.top + c.rc.bottom) / 2;
        ui::DrawChevron(dc, ax, ay, arrowPx, theme_->accent, sortAsc_ ? -1 : 1);
    }
    RECT hsep{tableHead_.left, tableHead_.bottom - 1, tableHead_.right, tableHead_.bottom};
    ui::DrawSeparator(dc, hsep, false, theme_->line);

    // ---- 行 ----
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, body_.left, body_.top, body_.right, body_.bottom);
    for (int vi = 0; vi < static_cast<int>(view_.size()); ++vi) {
        const RowLayout& rl = rowLayout_[static_cast<size_t>(vi)];
        if (rl.row.bottom <= body_.top || rl.row.top >= body_.bottom) continue;
        PaintRow(dc, vi);
    }
    RestoreDC(dc, saved);

    if (view_.empty()) {
        // 空结果必须说清是"没匹配到"，而不是让中栏一片空白（看起来像界面坏了）。
        const std::wstring msg = query_.empty() ? L"这一组没有命令"
                                                : (L"没有匹配「" + query_ + L"」的命令");
        RECT er{body_.left + pad, body_.top + theme_->S(40), body_.right - pad,
                body_.top + theme_->S(40) + theme_->S(24)};
        ui::DrawTextRow(dc, theme_->font, msg, er, theme_->inert,
                        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // 滚动条：只在内容超出时画一条细的，够看出"还能往下"。
    if (scrollMax_ > 0) {
        const int trackH = body_.bottom - body_.top;
        const int thumbH = std::max(theme_->S(24), trackH * trackH / (trackH + scrollMax_));
        const int thumbY = body_.top + (trackH - thumbH) * scroll_ / scrollMax_;
        RECT thumb{body_.right - theme_->S(3), thumbY, body_.right - theme_->S(1), thumbY + thumbH};
        ui::FillRound(dc, thumb, theme_->line, theme_->S(1));
    }
}

void HotkeyManager::PaintRow(HDC dc, int vi) {
    const int idx = view_[static_cast<size_t>(vi)];
    const Row& row = rows_[static_cast<size_t>(idx)];
    const RowLayout& rl = rowLayout_[static_cast<size_t>(vi)];

    const bool sel = row.src.setting == selSetting_;
    const bool multi = IsSelectedSetting(row.src.setting);
    if (multi) {
        // 多选的行统一用 wash 底（与单选同一个色）：多选是"单选的一个超集"，
        // 用两种颜色会让人以为它们是两种不同的东西。
        ui::FillRectColor(dc, rl.row, theme_->wash);
        // 最左一条 accent：单选时是 2px 实心，多选时 2px 也画 —— 但只给
        // **光标行**（selSetting_）画，其余多选行画 1px 淡色，
        // 于是"当前看的是哪一行"在多选里依然分得出来。
        const int barW = sel ? theme_->S(2) : theme_->S(1);
        RECT bar{rl.row.left, rl.row.top, rl.row.left + barW, rl.row.bottom};
        ui::FillRectColor(dc, bar, sel ? theme_->accent : theme_->softSel);
    } else if (vi == hoverRow_) {
        ui::FillRectColor(dc, rl.row, theme_->rowHover);
    }

    RECT dot{rl.row.left + theme_->S(16), 0, rl.row.left + theme_->S(16) + theme_->S(6), 0};
    dot.top = (rl.row.top + rl.row.bottom - theme_->S(6)) / 2;
    dot.bottom = dot.top + theme_->S(6);
    PaintSourceDot(dc, dot, row);

    ui::DrawTextRow(dc, theme_->font, row.src.title, rl.title,
                    row.status == kUnimplemented ? theme_->disabled : theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    ui::DrawTextRow(dc, theme_->font, row.src.scope, rl.scope, theme_->inert,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 键位格：正在改的这一行改画成"按下组合键"，并实时回显已按住的修饰键。
    const bool thisCapture = capturing_ && row.src.setting == captureSetting_;
    if (thisCapture) {
        const std::wstring hint = conflictPending_ ? (L"再按一次确认")
                                                   : (HeldModsText() + L"…");
        RECT chipIn{rl.chip.left, rl.chip.top, rl.chip.right, rl.chip.bottom};
        ui::FillRound(dc, chipIn, theme_->editBg, theme_->S(3));
        ui::FrameRectColor(dc, chipIn, conflictPending_ ? theme_->danger : theme_->accent);
        ui::DrawTextRow(dc, theme_->font, hint, chipIn,
                        conflictPending_ ? theme_->danger : theme_->accent,
                        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else {
        COLORREF fg = theme_->text;
        if (row.sequence) fg = theme_->sub;
        else if (row.unbound) fg = theme_->inert;
        else if (row.status == kReserved) fg = theme_->sub;
        PaintChip(dc, rl.chip, row.keyText, row.custom, fg);
    }

    COLORREF sc = theme_->sub;
    if (row.status == kConflict) sc = theme_->danger;
    else if (row.status == kReserved) sc = theme_->accent;
    else if (row.status == kUnimplemented) sc = theme_->inert;
    else if (row.sequence) sc = theme_->inert;
    // 可共存的同键：不标"冲突"（那是误报），改在"可用"后面加一个弱后缀。
    // 让用户知道"这个键还有别人用"，但不逼他去修。
    std::wstring statusText = StatusText(row.status, row.sequence);
    if (row.status == kOk && row.peers.empty() && !row.softPeers.empty()) {
        statusText += L"·同键";
        sc = theme_->inert;
    }
    ui::DrawTextRow(dc, theme_->font, statusText, rl.status, sc,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 序列行整体压暗一档：它们是**只读**的参考信息（要改得动 keymap.json 的状态机），
    // 与可编辑的热键混在一起时，用户点了发现按钮是灰的才知道"这条不能改"。
    // 与其让他在工具栏上试错，不如在行上先说清。只压标题，不动键位芯片 ——
    // 芯片本身是"信息"，压暗会让它读不清。
    if (row.sequence) {
        // 用"画一遍更淡的标题"实现：重画覆盖即可，不必引入 alpha 混合。
        // 底色要与该行当前的底一致（选中行是 wash、悬停是 rowHover、其余是 bg），
        // 否则会在选中/悬停行上擦出一道底色块 —— 那比"没压暗"更难看。
        COLORREF under = theme_->bg;
        if (IsSelectedSetting(row.src.setting)) under = theme_->wash;
        else if (vi == hoverRow_) under = theme_->rowHover;
        ui::FillRectColor(dc, rl.title, under);
        ui::DrawTextRow(dc, theme_->font, row.src.title, rl.title, theme_->inert,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
}

void HotkeyManager::PaintChip(HDC dc, const RECT& rc, const std::wstring& text, bool emphasis,
                              COLORREF fg) {
    ui::FillRound(dc, rc, theme_->editBg, theme_->S(3));
    ui::FrameRectColor(dc, rc, emphasis ? theme_->accent : theme_->editBorder);
    // 等宽字体：Combination 里的 + 与字母在比例字体下宽度不一，一串键位会左右抖。
    ui::DrawTextRow(dc, theme_->fontMono, text, rc, fg,
                    DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void HotkeyManager::PaintSourceDot(HDC dc, const RECT& rc, const Row& row) {
    if (!row.custom && !row.unbound) return;
    ui::FillRectColor(dc, rc, row.custom ? theme_->accent : theme_->inert);
}

// ---------------------------------------------------------------- 右栏

void HotkeyManager::PaintDetail(HDC dc) {
    // 右栏用 panelBg：与中栏内容区有可见分界，否则三栏看着是两块。
    ui::FillRectColor(dc, colC_, theme_->panelBg);
    RECT sep{colC_.left, colC_.top, colC_.left + 1, colC_.bottom};
    ui::DrawSeparator(dc, sep, true, theme_->line);

    const Row* row = Selected();
    if (!row) {
        RECT er{detailBox_.left, detailBox_.top + theme_->S(40), detailBox_.right,
                detailBox_.top + theme_->S(64)};
        ui::DrawTextRow(dc, theme_->font, L"在左侧选择一条命令", er, theme_->inert,
                        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        return;
    }

    ui::DrawTextRow(dc, theme_->fontBold, row->src.title, detail_.title, theme_->text,
                    DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    // 命令 id 用等宽：它是可复制、可粘到 hotkey.set 里的字符串。
    ui::DrawTextRow(dc, theme_->fontMono, row->src.command, detail_.cmd, theme_->inert,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    ui::DrawTextRow(dc, theme_->font, L"当前键位", detail_.keyLabel, theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    const bool thisCapture = capturing_ && row->src.setting == captureSetting_;
    if (thisCapture) {
        RECT box = detail_.keyChip;
        ui::FillRound(dc, box, theme_->editBg, theme_->S(3));
        ui::FrameRectColor(dc, box, conflictPending_ ? theme_->danger : theme_->accent);
        const std::wstring hint = conflictPending_ ? L"再按一次确认" : (HeldModsText() + L"…");
        ui::DrawTextRow(dc, theme_->font, hint, box,
                        conflictPending_ ? theme_->danger : theme_->accent,
                        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    } else if (row->sequence) {
        PaintChip(dc, detail_.keyChip, row->keyText, false, theme_->sub);
    } else {
        PaintChip(dc, detail_.keyChip, row->keyText, row->custom,
                  row->unbound ? theme_->inert : theme_->text);
    }

    // 来源：一句话说清"这个键是我改的还是默认的"。没有这一行，用户改完过两天
    // 就不知道哪些是自己动过的（VS Code 的 Source 列就是干这个的）。
    std::wstring source = L"默认";
    if (row->sequence) source = L"序列（只读）";
    else if (row->unbound) source = L"已解绑";
    else if (row->custom) source = L"已自定义";
    ui::DrawTextRow(dc, theme_->font, L"来源 " + source + L" · 范围 " + row->src.scope, detail_.meta,
                    theme_->sub, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (!row->sequence && !row->unbound && !row->src.nativeKey.empty() &&
        !str::IEquals(row->src.nativeKey, row->src.key)) {
        ui::DrawTextRow(dc, theme_->font, L"默认 " + row->src.nativeKey, detail_.defaultKey,
                        theme_->inert,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // 冲突：说清"跟谁撞了、后果是什么"。只标一个"冲突"字样的界面等于把
    // 排查工作全丢回给用户（281 行里自己找同键的那一条）。
    //
    // 【两级分开说】真冲突（红）与"可共存"（弱色）必须用不同颜色与不同措辞，
    // 否则用户看到一片红，分不清哪个必须修。可共存的典型是"标签栏裸键 vs
    // 其它"：焦点在标签栏时前者生效，焦点在别处时让位。
    if (!row->peers.empty()) {
        const std::wstring msg = L"与「" + row->peers + L"」共用同一个键。按先到先得，"
                                 L"只有排在前面的那条会触发，后面的按不出来。";
        ui::DrawTextRow(dc, theme_->font, msg, detail_.conflict, theme_->danger,
                        DT_WORDBREAK | DT_NOPREFIX);
    } else if (row->status == kReserved) {
        ui::DrawTextRow(dc, theme_->font, L"这个组合被系统占用，应用内注册也抢不到。",
                        detail_.conflict, theme_->danger, DT_WORDBREAK | DT_NOPREFIX);
    } else if (row->status == kUnimplemented) {
        ui::DrawTextRow(dc, theme_->font, L"该命令在当前版本未实现，绑了也不会生效。",
                        detail_.conflict, theme_->sub, DT_WORDBREAK | DT_NOPREFIX);
    } else if (!row->softPeers.empty()) {
        // 可共存：说清"什么时候谁生效"，而不是让用户以为键位坏了。
        const std::wstring msg = L"与「" + row->softPeers + L"」同键，但作用域不同："
                                 L"焦点在标签栏时它生效，在别处时让位。可以共存。";
        ui::DrawTextRow(dc, theme_->font, msg, detail_.conflict, theme_->sub,
                        DT_WORDBREAK | DT_NOPREFIX);
    }

    // 三个动作按钮。序列键不能在这儿改（要改序列得动 keymap.json 的状态机），
    // 所以按钮灰掉并把原因写在下面的提示里 —— 比点了没反应诚实。
    const bool editable = !row->sequence;
    const bool dim = !editable;
    auto drawButton = [&](const RECT& rc, const wchar_t* text, bool primary, bool disabled) {
        ui::FillRound(dc, rc, disabled ? theme_->rowHover : theme_->editBg, theme_->S(3));
        ui::FrameRectColor(dc, rc, disabled ? theme_->editBorder
                                           : (primary ? theme_->accent : theme_->editBorder));
        COLORREF fg = disabled ? theme_->disabled : (primary ? theme_->accent : theme_->text);
        ui::DrawTextRow(dc, theme_->font, text, rc, fg,
                        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    };
    drawButton(detail_.edit, thisCapture ? L"取消改键（Esc）" : L"编辑键位…", true, dim);
    drawButton(detail_.clear, L"清除键位", false, dim);
    // 逐条还原是常规动作（VS Code / JetBrains 的默认入口），不需要确认；
    // 全量还原是破坏性的，藏在两步确认后面，并且按钮文案会当场变。
    drawButton(detail_.reset, L"还原此项默认", false, dim);
    drawButton(detail_.resetAll, resetArmed_ ? L"再点一次：确认还原全部" : L"还原全部默认…",
               false, false);

    const wchar_t* hint = row->sequence
                              ? L"序列键要在 keymap.json 的 sequences 里改"
                              : L"回车 / 双击 = 改键 · Delete = 清除";
    ui::DrawTextRow(dc, theme_->font, hint, detail_.hint, theme_->inert,
                    DT_WORDBREAK | DT_NOPREFIX);
}

// ---------------------------------------------------------------- 状态栏

void HotkeyManager::PaintStatus(HDC dc) {
    ui::FillRectColor(dc, statusBar_, theme_->bg);
    RECT sep{statusBar_.left, statusBar_.top, statusBar_.right, statusBar_.top + 1};
    ui::DrawSeparator(dc, sep, false, theme_->line);

    std::wstring left = L"共 " + std::to_wstring(totalAll_) + L" 条";
    if (totalCustom_) left += L" · 自定义 " + std::to_wstring(totalCustom_);
    if (totalConflict_) left += L" · 冲突 " + std::to_wstring(totalConflict_);
    if (!query_.empty()) left += L" · 显示 " + std::to_wstring(view_.size());

    RECT lr{statusBar_.left + theme_->S(16), statusBar_.top, statusBar_.right / 2, statusBar_.bottom};
    ui::DrawTextRow(dc, theme_->font, left, lr, theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 右侧：优先显示捕获/录制的实时提示（那是当前唯一要紧的事），
    // 其次显示一次性的操作结果，都没有时给默认提示。
    std::wstring right;
    bool err = false;
    if (capturing_) {
        right = conflictPending_ ? (L"该键已被「" + pendingPeer_ + L"」占用，再按一次确认，Esc 取消")
                                 : (HeldModsText() + L"… 按下组合键，Esc 取消");
        err = conflictPending_;
    } else if (recording_) {
        right = L"录制中：按下组合键按键位筛选（Esc 退出）";
    } else if (!status_.empty()) {
        right = status_;
        err = statusError_;
    } else {
        right = L"↑↓ 选择 · 回车改键 · Delete 清除 · Ctrl+Z 撤销 · Ctrl+A 全选";
    }
    RECT rr{statusBar_.right / 2, statusBar_.top, statusBar_.right - theme_->S(16), statusBar_.bottom};
    ui::DrawTextRow(dc, theme_->font, right, rr, err ? theme_->danger : theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_RIGHT);
}
