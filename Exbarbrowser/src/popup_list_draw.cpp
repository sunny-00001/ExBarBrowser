#include "popup_list_draw.h"

#include "ui_draw.h"

namespace {

// ---------- 树模式的一行（面包屑文件夹下拉） ----------
static bool DrawTreeRow(HDC dc, const DRAWITEMSTRUCT* dis, const PopupDrawParams& p) {
    if (!p.tree || !p.visible) return true;
    if (dis->itemID >= p.visible->size()) return true;
    const int ni = (*p.visible)[static_cast<size_t>(dis->itemID)];
    const TreeNode& n = (*p.tree)[static_cast<size_t>(ni)];
    RECT rc = dis->rcItem;
    const bool hot = static_cast<int>(dis->itemID) == p.hot;
    const bool sel = static_cast<int>(dis->itemID) == p.curSel;
    // 画刷来自 Theme（唯一持有者），不再每行 CreateSolidBrush/DeleteObject。
    FillRect(dc, &rc, sel ? p.theme->brListSel
                          : (hot ? p.theme->brListHot : p.theme->brList));

    SetBkMode(dc, TRANSPARENT);
    const int indent = rc.left + p.theme->S(6) + n.depth * p.theme->S(16);
    if (n.icon) {
        RECT ir{indent, rc.top, indent + p.iconS, rc.bottom};
        ui::DrawIconCentered(dc, n.icon, ir, p.iconS);
    } else if (n.glyph != ico::IconId::None) {
        RECT gr{indent, rc.top, indent + p.iconS, rc.bottom};
        ui::DrawGlyph(dc, n.glyph, gr, p.iconS, p.theme->text);
    }
    const RECT tr{indent + p.iconS + p.theme->S(4), rc.top,
                  rc.right - p.theme->S(26), rc.bottom};
    ui::DrawTextRow(dc, p.theme->font, n.label, tr, p.theme->text,
                    DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (n.hasChildren) {
        // 命中区与 popup_list.cpp 的点击判定共用同一个实现（PopupExpanderRect）。
        RECT er = PopupExpanderRect(rc, *p.theme);
        const int cx = (er.left + er.right) / 2;
        const int cy = (er.top + er.bottom) / 2;
        const bool expHot = static_cast<int>(dis->itemID) == p.hotExpander;
        const COLORREF clr = expHot ? p.theme->text : p.theme->chevron;
        // 与地址栏分隔符完全同一个原语：展开 down(dir=1)，收起 left(dir=0)
        ui::DrawChevron(dc, cx, cy, p.theme->iconChevron(), clr, n.expanded ? 1 : 0);
    }
    return true;
}

// ---------- 平铺模式的一行 ----------
static bool DrawListRow(HDC dc, const DRAWITEMSTRUCT* dis, const PopupDrawParams& p) {
    if (!p.rows) return true;
    if (dis->itemID == static_cast<UINT>(-1) || dis->itemID >= p.rows->size()) return true;
    const PopupRow& row = (*p.rows)[dis->itemID];
    RECT rc = dis->rcItem;

    if (row.separator) {
        // 行高只有 sepPx（默认 2px）：整行铺底色，横线画在行的垂直中心。
        // 上下各留 1px 底色 = 视觉上就是"条目之间 2px 的间隙 + 一条分隔线"，
        // 不再是原来那样占满一整行 24px 的空白。
        FillRect(dc, &rc, p.theme->brList);
        const int y = (rc.top + rc.bottom) / 2;
        RECT hr{rc.left + p.theme->S(8), y, rc.right - p.theme->S(8), y};
        ui::DrawSeparator(dc, hr, false, p.theme->line);
        return true;
    }

    const bool sel = (dis->itemState & ODS_SELECTED) != 0;
    const bool hot = static_cast<int>(dis->itemID) == p.hot;
    FillRect(dc, &rc, sel ? p.theme->brListSel
                          : (hot ? p.theme->brListHot : p.theme->brList));

    // 拖拽插入线：画在"插到这一行之前"的行上沿；落在末尾时画在末行下沿。
    // 颜色用 accent（与书签栏顶层的插入线同一个令牌），粗 2px。
    if (p.dropIndex >= 0 && p.rows) {
        const int n = static_cast<int>(p.rows->size());
        int y = -1;
        if (p.dropIndex < n) {
            if (static_cast<int>(dis->itemID) == p.dropIndex) y = rc.top;
        } else if (p.dropIndex == n && static_cast<int>(dis->itemID) == n - 1) {
            y = rc.bottom;
        }
        if (y >= 0) {
            RECT bar{rc.left + p.theme->S(4), y - 1, rc.right - p.theme->S(4), y + 1};
            ui::FillRectColor(dc, bar, p.theme->accent);
        }
    }

    SetBkMode(dc, TRANSPARENT);
    int x = rc.left + p.theme->S(8);

    // 勾选列（最左一列，独立于图标列）：勾号是**系统字形**（Segoe Fluent Icons
    // U+E73E），不是正文字体里的 "✓" 字符 —— 后者粗细与旁边的图标不是一个体系。
    // 未勾选的行这一格留空，整列因此上下对齐（不再用空格占位）。
    if (p.checkCol) {
        if (row.checked) {
            RECT cr{x, rc.top, x + p.iconS, rc.bottom};
            ui::DrawGlyph(dc, ico::IconId::Check, cr, p.iconS,
                          row.disabled ? p.theme->disabled : p.theme->text);
        }
        x += PopupCheckColWidth(p.iconS) + p.theme->S(4);
    }

    if (row.icon) {
        RECT ir{x, rc.top, x + p.iconS, rc.bottom};
        ui::DrawIconCentered(dc, row.icon, ir, p.iconS);
    } else if (row.glyph != ico::IconId::None) {
        // 与 HICON 共用同一条图标槽：不新建字体、不建临时图标，
        // 直接走 ui::DrawGlyph（内部选 GlyphFace 并还原字体）。
        RECT gr{x, rc.top, x + p.iconS, rc.bottom};
        ui::DrawGlyph(dc, row.glyph, gr, p.iconS,
                      row.disabled ? p.theme->disabled : p.theme->text);
    }
    x += p.iconS + p.theme->S(8);

    // ---- 行右端的两条"附属信息"：chevron（有下一级）与快捷键 ----
    //
    // 几何必须**先算完再画**，否则长文案会穿到键位上。业界顺序（Win32 菜单 /
    // Explorer / Win11 Fluent / VS Code）从右往左是：右内边距 → 快捷键 → 间距 → 文案；
    // 有 chevron 的行 chevron 占最右，键位退到它左边。
    const int padRight = p.theme->S(10);
    int edge = rc.right - padRight;   // 文案可用的最右边界（逐项往左收）

    // chevron 占 16dp（= 旧代码里 folder 行文案右边界 rc.right-26 与无 chevron 行
    // rc.right-10 之差）：这个值不能改，否则"有没有下一级"会让文案截断位置跳一格。
    const int chevW = p.theme->S(16);
    if (row.folder) edge -= chevW;

    int labelRight = edge;
    if (!row.shortcut.empty()) {
        const int keyW = ui::MeasureText(dc, p.theme->font, row.shortcut);
        // 文案与键位之间留固定最小间距（业界 24dp 量级；这里 12dp 是下限，
        // 因为本弹层的行本来就窄，间距再大就会把常用文案过早截断）。
        labelRight = edge - keyW - p.theme->S(12);
        const RECT kr{edge - keyW, rc.top, edge, rc.bottom};
        // 键位是"提示"，比主文案弱一档：用 inert（列表次要字令牌）；
        // 禁用行跟着 disabled 一起淡下去，不让它就着主文案还更醒目。
        ui::DrawTextRow(dc, p.theme->font, row.shortcut, kr,
                        row.disabled ? p.theme->disabled : p.theme->inert,
                        DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }

    // 文案区右边界 = labelRight。**没有**快捷键时它就是 edge，与从前完全一致
    // （不允许"有没有键"改变文案的截断位置之外的东西）。
    const RECT tr{x, rc.top, labelRight, rc.bottom};
    ui::DrawTextRow(dc, p.theme->font, row.label, tr,
                    row.disabled ? p.theme->disabled : p.theme->text,
                    DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (row.folder) {
        const int cx = rc.right - p.theme->S(16);
        const int cy = (rc.top + rc.bottom) / 2;
        // 与地址栏分隔符共用通用原语 DrawChevron；rest 同形态，选中提亮
        ui::DrawChevron(dc, cx, cy, p.theme->iconChevron(),
                        hot || sel ? p.theme->text : p.theme->chevron, 0);
    }
    return true;
}

}  // namespace

bool PopupDrawItem(HDC dc, const DRAWITEMSTRUCT* dis, const PopupDrawParams& p) {
    if (!p.theme) return true;
    return p.treeMode ? DrawTreeRow(dc, dis, p) : DrawListRow(dc, dis, p);
}

int PopupMeasureWidth(HWND owner, const Theme& theme, const std::vector<PopupRow>* rows,
                      const std::vector<TreeNode>* tree, int iconS, bool exactWidth,
                      int minWidthPx) {
    // 量字走唯一原语（ui::MeasureText 内部选字体并还原）
    HDC dc = GetDC(owner);
    int textW = 0;
    if (tree) {
        for (const auto& n : *tree) {
            textW = std::max<int>(textW,
                                  ui::MeasureText(dc, theme.font, n.label) +
                                      n.depth * theme.S(16));
        }
    } else if (rows) {
        for (const auto& r : *rows) {
            if (r.separator) continue;
            // 快捷键列在**行右端**，必须逐行并入宽度：只按最长文案算宽，键位会被
            // 右边距压掉（表现为键位末尾缺一截）；反过来按"最长文案 + 最长键位"
            // 硬凑，短文案的菜单会白多一大块空白。
            int w = ui::MeasureText(dc, theme.font, r.label);
            if (!r.shortcut.empty()) w += theme.S(12) + ui::MeasureText(dc, theme.font, r.shortcut);
            // chevron 槽必须按行计入（与 DrawListRow 的 edge -= S(16) 同一判据）：
            // 只有 folder 行才占那 16dp。漏掉它 → 有下一级的那几行键位会被压掉一截。
            if (r.folder) w += theme.S(16);
            textW = std::max(textW, w);
        }
    }
    ReleaseDC(owner, dc);

    // 勾选列的宽度要算进去，否则勾号会被右边距压掉 / 挤到文字上。
    const bool checkCol = rows ? HasCheckedRow(*rows) : false;
    // 行内几何（与 DrawListRow 同一套令牌、同一累加顺序）：
    //   左内边距 8 + 图标槽 iconS + 图标后间距 8 + 右内边距 10。
    // 【为什么必须用 S()】原式是 `textW + iconS + 8 + 8 + 26`：26 是个**不随 DPI
    // 缩放的裸数**，而绘制侧用的是 S()。96 DPI 下两者恰好都是 26，到 150% DPI
    // 就差 13px —— 表现为键位右端被右边距吃掉一截。
    // chevron 的 16dp **不在这里**：只有 folder 行才占它，所以在上面的逐行循环里
    // 按行累加（漏一处就是"有下一级的那几行键位被挤掉"）。
    int width = textW + iconS + theme.S(8) + theme.S(8) + theme.S(10);
    if (checkCol) width += PopupCheckColWidth(iconS) + theme.S(4);
    const int minW = minWidthPx > 0 ? minWidthPx : theme.S(160);
    if (exactWidth) {
        width = std::max(minW, theme.S(120));
    } else {
        width = std::max(width, minW);
        width = std::min(width, theme.S(460));
    }
    return width;
}
