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
        PaintRail(dc);
        PaintTable(dc);
        PaintDetail(dc);
        PaintStatus(dc);
    });
}

// ---------------------------------------------------------------- 左栏

void HotkeyManager::PaintRail(HDC dc) {
    ui::FillRectColor(dc, colA_, theme_->bg);

    // 标题
    RECT tr{colA_.left + theme_->S(16), colA_.top + theme_->S(8), colA_.right - theme_->S(10),
            colA_.top + theme_->S(38)};
    ui::DrawTextRow(dc, theme_->fontBold, L"筛选", tr, theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    const int rh = theme_->S(32);
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

    // ---- 搜索框（真正的 EDIT 画在它上面，这里只画框 + 放大镜）----
    const int radius = theme_->radiusBtn();
    ui::FillRound(dc, searchBox_, theme_->barAlt, radius);
    ui::FrameRectColor(dc, searchBox_, recording_ ? theme_->accent : theme_->editBorder);
    const int iconPx = theme_->iconSmall();
    RECT icon{searchBox_.left + theme_->S(8),
              searchBox_.top + (searchBox_.bottom - searchBox_.top - iconPx) / 2,
              searchBox_.left + theme_->S(8) + iconPx,
              searchBox_.top + (searchBox_.bottom - searchBox_.top - iconPx) / 2 + iconPx};
    ui::DrawGlyph(dc, ico::IconId::Search, icon, iconPx, theme_->sub);

    // ---- 录制按钮：键盘图标。开了之后按键不再进搜索框，而是被译成组合键做筛选
    //      （VS Code 的 Record Keys）。开着时整颗按钮转成强调色，不可能看错。----
    if (recording_) {
        ui::FillRound(dc, recordBtn_, theme_->accent, radius);
    } else if (hoverRecord_) {
        ui::FillRound(dc, recordBtn_, theme_->rowHover, radius);
    } else {
        ui::FillRound(dc, recordBtn_, theme_->barAlt, radius);
    }
    if (!recording_) ui::FrameRectColor(dc, recordBtn_, theme_->editBorder);
    const int kpx = theme_->iconSmall();
    RECT kir{recordBtn_.left + (recordBtn_.right - recordBtn_.left - kpx) / 2,
             recordBtn_.top + (recordBtn_.bottom - recordBtn_.top - kpx) / 2,
             recordBtn_.left + (recordBtn_.right - recordBtn_.left - kpx) / 2 + kpx,
             recordBtn_.top + (recordBtn_.bottom - recordBtn_.top - kpx) / 2 + kpx};
    ui::DrawGlyph(dc, ico::IconId::Keyboard, kir, kpx,
                  recording_ ? theme_->selSolidFg : theme_->sub);

    // ---- 表头 ----
    ui::FillRectColor(dc, tableHead_, theme_->rowHover);
    const int pad = theme_->S(12);
    if (!view_.empty() || !rowLayout_.empty()) {
        // 列标题从第一行的几何反推：两边都按同一份矩形，列宽改了不会有一处忘了跟。
        // 没有可见行时（搜索无结果）用同样的常量画一次，列位置才不会跳。
        const RowLayout probe = rowLayout_.empty() ? RowLayout{} : rowLayout_[0];
        RECT hcmd{body_.left + pad + theme_->S(16), tableHead_.top,
                  probe.title.right, tableHead_.bottom};
        RECT hscope{probe.scope.left, tableHead_.top, probe.scope.right, tableHead_.bottom};
        RECT hkey{probe.chip.left, tableHead_.top, probe.chip.right, tableHead_.bottom};
        RECT hstat{probe.status.left, tableHead_.top, probe.status.right, tableHead_.bottom};
        if (rowLayout_.empty()) {
            // 视图为空：用 body_ 的宽度临时按同一套比例摆一次（只为画标题）。
            const int right = body_.right - pad;
            hstat = {right - theme_->S(88), tableHead_.top, right, tableHead_.bottom};
            hkey = {hstat.left - theme_->S(10) - theme_->S(152), tableHead_.top,
                    hstat.left - theme_->S(10), tableHead_.bottom};
            hscope = {hkey.left - theme_->S(12) - theme_->S(88), tableHead_.top,
                      hkey.left - theme_->S(12), tableHead_.bottom};
            hcmd = {body_.left + pad + theme_->S(16), tableHead_.top, hscope.left, tableHead_.bottom};
        }
        ui::DrawTextRow(dc, theme_->font, L"命令", hcmd, theme_->sub,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        ui::DrawTextRow(dc, theme_->font, L"范围", hscope, theme_->sub,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        ui::DrawTextRow(dc, theme_->font, L"键位", hkey, theme_->sub,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        ui::DrawTextRow(dc, theme_->font, L"状态", hstat, theme_->sub,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
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
    if (sel) {
        ui::FillRectColor(dc, rl.row, theme_->wash);
        RECT bar{rl.row.left, rl.row.top, rl.row.left + theme_->S(2), rl.row.bottom};
        ui::FillRectColor(dc, bar, theme_->accent);
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
    ui::DrawTextRow(dc, theme_->font, StatusText(row.status, row.sequence), rl.status, sc,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
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
        right = L"↑↓ 选择 · 回车改键 · Delete 清除 · Ctrl+F 搜索";
    }
    RECT rr{statusBar_.right / 2, statusBar_.top, statusBar_.right - theme_->S(16), statusBar_.bottom};
    ui::DrawTextRow(dc, theme_->font, right, rr, err ? theme_->danger : theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_RIGHT);
}
