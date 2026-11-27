// 设置对话框的绘制。
//
// 【只用一套原语】颜色来自 Theme、量字走 ui::MeasureText、画字形走 ui::DrawGlyph、
// 分隔线走 ui::DrawSeparator、圆角底板走 ui::FillRound、双缓冲走 ui::PaintBuffered。
// 自己写一份就会与项目其余部分分叉 —— 而分叉的表现是"这个窗口和别处不是
// 一个软件"。
//
// 【几何来自布局】行/列的矩形在 settings_dialog_layout.cpp 的 Layout 里算好，
// 这里只按那份矩形画。绘制与命中共用一份几何：两处各算一遍迟早错位。
#include "settings_dialog.h"

#include <algorithm>

#include "str.h"
#include "ui_draw.h"

namespace {

// 面包屑里的分隔符（原型用 ›）。
const wchar_t* kCrumbSep = L"›";

// 段标题的高度与普通行相同 —— 原型里两者都是 var(--rowH)。
// 段标题比行浅（用 rowHover 底），行才是内容。
}  // namespace

void SettingsDialog::Paint(HWND hwnd) {
    if (!theme_) return;
    ui::PaintBuffered(hwnd, theme_->bg, [this](HDC dc, const RECT& rc) {
        (void)rc;
        PaintLeft(dc, colA_);
        PaintMiddle(dc, colB_);
        PaintRight(dc, colC_);
        PaintStatus(dc, statusBar_);
    });
}

// ---------------------------------------------------------------- 左栏

void SettingsDialog::PaintLeft(HDC dc, const RECT& rc) {
    ui::FillRectColor(dc, rc, theme_->bg);

    // 搜索框：圆角底板 + 边框 + 放大镜（真正的输入框是子控件，画在底板之上）。
    const int radius = theme_->radiusBtn();
    ui::FillRound(dc, searchBox_, theme_->barAlt, radius);
    ui::FrameRectColor(dc, searchBox_, theme_->editBorder);
    const int iconPx = theme_->iconSmall();
    RECT ir{searchBox_.left + theme_->S(6), searchBox_.top + (searchBox_.bottom - searchBox_.top - iconPx) / 2,
            searchBox_.left + theme_->S(6) + iconPx,
            searchBox_.top + (searchBox_.bottom - searchBox_.top - iconPx) / 2 + iconPx};
    ui::DrawGlyph(dc, ico::IconId::Search, ir, iconPx, theme_->sub);

    // 分类列表（= 标签页）。
    //
    // 【必须裁剪到 catList_】分组数 × 行高会超过列表高度（分组只增不减，
    // 窗口却能被压小），不裁剪的话最后几行直接画到滚轮提示行和页脚的上面 ——
    // 看起来像"页脚上面压着半行文字"，而不是"列表还能滚"。
    const int gSaved = SaveDC(dc);
    IntersectClipRect(dc, catList_.left, catList_.top, catList_.right, catList_.bottom);
    const int rh = RowHeight();
    const int lineH = EditLineHeight();
    const bool searching = !query_.empty();
    for (size_t i = 0; i < groups_.size(); ++i) {
        const GroupItem& gi = groups_[i];
        // 原点减去滚动量：与 HitGroup 用的是同一个换算（画与点在同一个坐标里）。
        const int top = catList_.top + static_cast<int>(i) * rh - groupScroll_;
        const RECT r{catList_.left + theme_->S(6), top,
                     catList_.right - theme_->S(6), top + rh};
        // 整行在视口外就跳过（不是"画了再被裁"，那会把每条边都画一遍）。
        if (r.bottom <= catList_.top || r.top >= catList_.bottom) continue;
        const bool on = !searching && group_ == gi.group->key;
        if (on) {
            ui::FillRound(dc, r, theme_->softSel, radius);
            // 选中项左侧一条实心标记：像标签页的选中边。
            RECT bar{r.left, r.top + theme_->S(6), r.left + theme_->S(3), r.bottom - theme_->S(6)};
            ui::FillRound(dc, bar, theme_->accent, theme_->S(2));
        } else if (static_cast<int>(i) == hoverGroup_) {
            ui::FillRound(dc, r, theme_->rowHover, radius);
        }

        const int ipx = theme_->iconSmall();
        RECT ir2{r.left + theme_->S(8), r.top + (rh - ipx) / 2, r.left + theme_->S(8) + ipx,
                 r.top + (rh - ipx) / 2 + ipx};
        ui::DrawGlyph(dc, gi.group->icon, ir2, ipx, on ? theme_->accent : theme_->sub);

        const RECT line = ui::CenteredLineRect(r, lineH);
        RECT nr{r.left + theme_->S(8) + ipx + theme_->S(10), line.top, r.right - theme_->S(40),
                line.bottom};
        ui::DrawTextRow(dc, on ? theme_->fontBold : theme_->font, gi.group->name, nr,
                        on ? theme_->accent : theme_->text,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        std::wstring count = searching ? (std::to_wstring(gi.hits) + L"/" + std::to_wstring(gi.total))
                                       : std::to_wstring(gi.total);
        RECT cr{r.right - theme_->S(38), line.top, r.right - theme_->S(8), line.bottom};
        ui::DrawTextRow(dc, theme_->font, count, cr, on ? theme_->accent : theme_->inert,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    RestoreDC(dc, gSaved);

    // 滚轮提示：左栏滚轮切分类 —— 这个手势没有别处提示，不写出来就没人知道。
    // （原型里有这一行；它在左栏而不是状态栏，因为它是"左栏的手势"。）
    if (hint_.bottom > hint_.top) {
        ui::DrawSeparator(dc, hint_, false, theme_->line);
        // 上下两枚小箭头（⇕）：说明"滚轮"而不是"点"。
        const int cw = theme_->iconGlyph() / 2;
        const int cx = hint_.left + theme_->S(14) + cw;
        const int cy = (hint_.top + hint_.bottom) / 2;
        ui::DrawChevron(dc, cx, cy - cw, cw, theme_->sub, -1);
        ui::DrawChevron(dc, cx, cy + cw, cw, theme_->sub, 1);
        RECT tr{hint_.left + theme_->S(14) + cw * 2 + theme_->S(6), hint_.top, hint_.right - theme_->S(10),
                hint_.bottom};
        ui::DrawTextRow(dc, theme_->font, L"在左栏滚动滚轮 = 切换标签页", tr, theme_->sub,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // 底部计数：共几项 / 几项可改 / 几项只读（原型口径）。
    ui::DrawSeparator(dc, foot_, false, theme_->line);
    const std::wstring left = L"共 " + std::to_wstring(totalAll_) + L" 项";
    const std::wstring right = std::to_wstring(totalWritable_) + L" 可改 · " +
                               std::to_wstring(totalAll_ - totalWritable_) + L" 只读";
    // 两段各占页脚一半，中间留 S(8)。早先各写死 S(120)，左栏最小宽 160 时
    // 两段重叠 80px、右段向左压住左段 —— 按 foot_ 实际宽度分半就没这个问题。
    const int pad = theme_->S(14);
    const int half = (foot_.right - foot_.left) / 2;
    RECT lr{foot_.left + pad, foot_.top, foot_.left + half - theme_->S(4), foot_.bottom};
    RECT rr{foot_.left + half + theme_->S(4), foot_.top, foot_.right - pad, foot_.bottom};
    ui::DrawTextRow(dc, theme_->font, left, lr, theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    ui::DrawTextRow(dc, theme_->fontBold, right, rr, theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------- 中栏

void SettingsDialog::PaintMiddle(HDC dc, const RECT& rc) {
    ui::FillRectColor(dc, rc, theme_->bg);

    // 头部：面包屑 + 密度 + 帮助。
    const bool searching = !query_.empty();
    std::wstring crumb1;
    std::wstring crumb2;
    if (searching) {
        crumb1 = L"搜索「" + query_ + L"」";
        crumb2 = std::to_wstring(rows_.size()) + L" 项匹配";
    } else {
        const settings_reg::Group* g = settings_reg::FindGroup(group_);
        crumb1 = g ? g->name : L"设置";
        const Row* row = RowByKey(selKey_);
        crumb2 = row ? row->name : std::wstring();
    }
    const int hx = head_.left + theme_->S(24);
    // 面包屑整体止于密度按钮之前，不占按钮区。第二段（结果数 / 当前项名）右对齐到
    // 这里，第一段用剩余宽度并省略 —— 早先按完整 MeasureText 宽度定位第二段，
    // 长搜索词时它被推出 head_，整段消失或压住密度按钮。
    const int crumbRight = std::max(hx, static_cast<int>(rcDensity_.left) - theme_->S(8));
    const bool hasCrumb2 = !crumb2.empty();
    const int w2 = hasCrumb2 ? ui::MeasureText(dc, theme_->fontBold, crumb2) : 0;
    const int cr2left = hasCrumb2 ? std::max(hx, crumbRight - w2) : crumbRight;
    // 第一段止于分隔符之前（给分隔符留 kCrumbGap），空间不足时它自己省略。
    const int kCrumbGap = theme_->S(16);
    RECT cr1{hx, head_.top, std::max(hx, cr2left - kCrumbGap), head_.bottom};
    ui::DrawTextRow(dc, theme_->fontBold, crumb1, cr1, theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (hasCrumb2) {
        RECT sr{cr2left - kCrumbGap, head_.top, cr2left - theme_->S(4), head_.bottom};
        if (sr.right > hx)
            ui::DrawTextRow(dc, theme_->font, kCrumbSep, sr, theme_->sub,
                            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        RECT cr2{cr2left, head_.top, crumbRight, head_.bottom};
        ui::DrawTextRow(dc, theme_->fontBold, crumb2, cr2, theme_->accent,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // 密度按钮：一个小框，写着当前行高。
    ui::FrameRectColor(dc, rcDensity_, theme_->editBorder);
    // 行高读数显示实际像素（RowHeight 已过 DPI 缩放）—— 早先显示逻辑值
    // kRowHeights[density_]，150% DPI 下写着"32"实际占 48 像素，读数与眼睛看到的
    // 对不上。密度菜单/状态消息/状态栏用的是同一个缩放口径。
    const std::wstring densityText = L"行高 " + std::to_wstring(RowHeight());
    ui::DrawTextRow(dc, theme_->font, densityText, rcDensity_, theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

    // 帮助按钮：圆底 + "?"。
    ui::FillRound(dc, rcHelp_, theme_->wash, rcHelp_.bottom - rcHelp_.top);
    ui::FrameRectColor(dc, rcHelp_, theme_->editBorder);
    ui::DrawTextRow(dc, theme_->fontBold, L"?", rcHelp_, theme_->accent,
                    DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

    ui::DrawSeparator(dc, head_, false, theme_->line);

    // 表单区：段标题 + 行。
    //
    // 【裁剪到 form_】中栏可滚动，行/标题条的矩形会伸到 form_ 上下之外
    // （滚到一半时第一行只露出下半截）。不裁剪就会画进头部（面包屑 / 密度 /
    // 帮助按钮）和状态栏 —— 那两处是"不该被盖住"的东西：盖住之后按钮还在，
    // 但看不见了，点下去照样生效。
    const int fSaved = SaveDC(dc);
    IntersectClipRect(dc, form_.left, form_.top, form_.right, form_.bottom);
    for (const auto& item : items_) {
        if (item.header) {
            PaintSectionHeader(dc, item);
            continue;
        }
        const size_t idx = static_cast<size_t>(item.rowIndex);
        if (idx >= rows_.size() || idx >= rowLayout_.size()) continue;
        const RowLayout& rl = rowLayout_[idx];
        if (rl.row.bottom < form_.top || rl.row.top > form_.bottom) continue;
        PaintRow(dc, item.rowIndex);
    }
    RestoreDC(dc, fSaved);

    // 空结果提示：搜索命中 0 项时中栏会是一片空白，看起来像"界面坏了"。
    // 说清是"没匹配到"而不是"没有这个设置" —— 前者提醒换词，后者会让人以为
    // 程序缺了功能（原型同此口径）。
    if (items_.empty()) {
        const std::wstring msg =
            searching ? (L"没有匹配「" + query_ + L"」的设置项") : L"这一组没有设置项";
        RECT er{form_.left + theme_->S(24), form_.top + theme_->S(40),
                form_.right - theme_->S(24), form_.top + theme_->S(40) + theme_->S(24)};
        ui::DrawTextRow(dc, theme_->font, msg, er, theme_->inert,
                        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
}

void SettingsDialog::PaintSectionHeader(HDC dc, const Item& item) {
    ui::FillRectColor(dc, item.rc, theme_->rowHover);
    ui::DrawSeparator(dc, item.rc, false, theme_->line);
    const int ipx = theme_->iconSmall() - theme_->S(2);
    RECT ir{item.rc.left + theme_->S(24), item.rc.top + (item.rc.bottom - item.rc.top - ipx) / 2,
            item.rc.left + theme_->S(24) + ipx,
            item.rc.top + (item.rc.bottom - item.rc.top - ipx) / 2 + ipx};
    ui::DrawGlyph(dc, item.icon, ir, ipx, theme_->sub);
    const RECT line = ui::CenteredLineRect(item.rc, EditLineHeight());
    RECT nr{ir.right + theme_->S(8), line.top, item.rc.right - theme_->S(110), line.bottom};
    ui::DrawTextRow(dc, theme_->fontBold, item.title, nr, theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 右侧：这一段有几项、其中几项可改。
    int total = 0;
    int writable = 0;
    for (const auto& r : rows_) {
        if (r.entry->group != item.group) continue;
        ++total;
        if (r.writable) ++writable;
    }
    std::wstring cnt = std::to_wstring(total) + L" 项";
    if (writable) cnt += L" · " + std::to_wstring(writable) + L" 可改";
    RECT cr{item.rc.right - theme_->S(110), line.top, item.rc.right - theme_->S(24), line.bottom};
    ui::DrawTextRow(dc, theme_->font, cnt, cr, theme_->inert,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT);
}

void SettingsDialog::PaintRow(HDC dc, int rowIndex) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size())) return;
    if (rowIndex >= static_cast<int>(rowLayout_.size())) return;
    const Row& row = rows_[static_cast<size_t>(rowIndex)];
    const RowLayout& rl = rowLayout_[static_cast<size_t>(rowIndex)];

    const bool selected = row.entry->key == selKey_;
    if (selected) {
        ui::FillRectColor(dc, rl.row, theme_->wash);
        RECT bar{rl.row.left, rl.row.top, rl.row.left + theme_->S(2), rl.row.bottom};
        ui::FillRectColor(dc, bar, theme_->accent);
    } else if (rowIndex == hoverRow_) {
        ui::FillRectColor(dc, rl.row, theme_->rowHover);
    }
    // 行底一条极淡的分隔线（段与段之间靠标题条分，行与行之间靠这条线）。
    ui::DrawSeparator(dc, rl.row, false, theme_->line);

    const bool dim = !row.writable;
    ui::DrawTextRow(dc, theme_->font, row.name, rl.name, dim ? theme_->disabled : theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 说明列：只读项标一句"只读"，一眼看出这项点不动。
    //
    // 【为什么不直接加前缀】表里的只读项说明本身就以「只读：」开头
    // （见 settings_reg_table.cpp 的「名称（只读：细节）」），说明已经说过一遍
    // 只读 —— 再加前缀就成了「只读 · 只读：…」，同一个标记写两遍。
    // 所以这里只在说明**没提**只读时才补。
    std::wstring desc = row.desc;
    if (dim && !str::IStartsWith(desc, L"只读"))
        desc = desc.empty() ? L"只读" : (L"只读 · " + desc);
    // 说明为空时画一个短横 —— 表里少数项的 doc 只有名称没有说明，
    // 留空会让说明列看起来像漏画了（原型同样是「—」）。
    if (desc.empty()) desc = L"—";
    ui::DrawTextRow(dc, theme_->font, desc, rl.desc, theme_->inert,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 控件（枚举优先：带 options 的项一律画下拉，见 settings_dialog_model.cpp）。
    if (!row.entry->options.empty()) {
        PaintSelect(dc, rl.box, rl.line, row.display, dim);
    } else if (row.entry->kind == settings_reg::Kind::Bool) {
        PaintCheckbox(dc, rl.box, row.value.b, dim);
    } else if (row.entry->kind == settings_reg::Kind::Int) {
        const wchar_t* unit = UnitOf(row.entry->key);
        PaintNumber(dc, rl.box, rl.line, row.display, unit, dim,
                    !badKey_.empty() && row.entry->key == badKey_);
    } else if (row.entry->kind == settings_reg::Kind::Str && row.writable) {
        // 可写字符串：画成一个输入框（点进去就地编辑，与数字项同一条路径）。
        PaintTextField(dc, rl.box, rl.line, row.display, dim);
    } else {
        // 只读的复合值：右端一句摘要（没有控件盒子）。
        ui::DrawTextRow(dc, theme_->font, row.display, rl.box, theme_->inert,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_RIGHT);
    }
}

void SettingsDialog::PaintCheckbox(HDC dc, const RECT& rc, bool on, bool dim) {
    const int radius = theme_->S(3);
    if (on) {
        ui::FillRound(dc, rc, dim ? theme_->disabled : theme_->accent, radius);
        const int px = theme_->S(11);
        RECT gr{rc.left, rc.top, rc.right, rc.bottom};
        ui::DrawGlyph(dc, ico::IconId::Check, gr, px, theme_->bg, DT_CENTER | DT_VCENTER);
    } else {
        ui::FillRound(dc, rc, theme_->editBg, radius);
        ui::FrameRectColor(dc, rc, dim ? theme_->editBorder : theme_->sub);
    }
}

void SettingsDialog::PaintSelect(HDC dc, const RECT& rc, const RECT& line,
                                 const std::wstring& text, bool dim) {
    const int radius = theme_->S(3);
    ui::FillRound(dc, rc, dim ? theme_->rowHover : theme_->editBg, radius);
    ui::FrameRectColor(dc, rc, theme_->editBorder);
    const int chev = theme_->iconChevron();
    const int gap = theme_->S(4);   // 箭头与边框的距离：先算好再传，
                                    // 免得把间距混进"图标尺寸"里 —— 那样两个数会互相覆盖。
    RECT tr{rc.left + theme_->S(9), line.top, rc.right - chev - theme_->S(8), line.bottom};
    ui::DrawTextRow(dc, theme_->font, text, tr, dim ? theme_->disabled : theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    ui::DrawChevron(dc, rc.right - chev - gap, (line.top + line.bottom) / 2, chev,
                    dim ? theme_->disabled : theme_->sub, 1);
}

void SettingsDialog::PaintNumber(HDC dc, const RECT& rc, const RECT& line,
                                 const std::wstring& text, const wchar_t* unit, bool dim,
                                 bool bad) {
    const int radius = theme_->S(3);
    ui::FillRound(dc, rc, dim ? theme_->rowHover : theme_->editBg, radius);
    ui::FrameRectColor(dc, rc, bad ? theme_->danger : theme_->editBorder);
    // 【与 rl.edit 同界】EDIT 定位在 box.left..box.right（有单位时右边界收到
    // unit.left - gap），左右都不额外内缩。早先绘制写死 S(4)/S(6)，与 EDIT 差
    // 几个像素，点进编辑态文字就往左跳一截。现在数值文字矩形取 EDIT 的原边界，
    // 单位矩形取 layout 的 rl.unit 原边界 —— 两处都用共享的 kValueUnitGap。
    const int ugap = theme_->S(kValueUnitGap);
    RECT tr{rc.left, line.top, rc.right, line.bottom};
    // 单位住在框内右侧，数值右对齐到它左边。这样数字框与下拉/字符串框一样宽，
    // 一排值控件的左右边缘才会严格对齐（编辑时 EDIT 也只覆盖数值那一截）。
    if (unit && *unit) {
        const int unitW = ui::MeasureText(dc, theme_->font, unit);
        RECT ur{rc.right - ugap - unitW, line.top, rc.right - ugap, line.bottom};
        ui::DrawTextRow(dc, theme_->font, unit, ur, theme_->sub,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT);
        tr.right = std::min(tr.right, ur.left - ugap);
    }
    ui::DrawTextRow(dc, theme_->font, text, tr, dim ? theme_->disabled : theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_RIGHT);
}

void SettingsDialog::PaintTextField(HDC dc, const RECT& rc, const RECT& line,
                                    const std::wstring& text, bool dim) {
    const int radius = theme_->S(3);
    ui::FillRound(dc, rc, dim ? theme_->rowHover : theme_->editBg, radius);
    ui::FrameRectColor(dc, rc, theme_->editBorder);
    // 与 rl.edit 同界（EDIT 铺满整个 box，无内缩），进入编辑态不跳位。
    RECT tr{rc.left, line.top, rc.right, line.bottom};
    // 【右对齐 + 路径省略】与行内编辑框（EDIT 的 ES_RIGHT）保持同一种对齐 ——
    // 编辑时文字不能跳位置。而路径的**尾部**才是"要搜哪个目录"，所以省略号
    // 放在前面（DT_PATH_ELLIPSIS）："\a\b\c" 变成 "…\b\c" 比 "\a\b…" 有用。
    ui::DrawTextRow(dc, theme_->font, text, tr, dim ? theme_->disabled : theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_PATH_ELLIPSIS | DT_RIGHT);
}
