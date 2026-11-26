// 设置对话框的排布与命中：哪些矩形画在哪、哪个点落在哪儿。
//
// 【为什么排布单独成文件】排布产出的每一份矩形（RowLayout / RightLayout）
// 被**绘制与命中测试共用**。两处各算一遍，差一个像素就是"点复制却复制了
// 恢复默认"。所以几何只有这一个出处。
//
// 【为什么命中跟排布同文件】命中读的就是 Layout 产出的矩形（item.rc /
// rowLayout_ / right_ / rcDensity_ / rcHelp_）。把命中拆到别处就必然出现
// "另一份换算"，而错位没有编译错误，只有"点 A 改了 B"。
#include "settings_dialog.h"

#include <algorithm>

#include "config.h"
#include "settings_bounds.h"
#include "ui_draw.h"

namespace {

// 布局常量（逻辑像素，96 DPI）。都放一处：布局与绘制都要用同一批数字，
// 分开放就会差几个像素（"按钮压着文字"）。
// 左栏宽 / 右栏宽 / 值控件宽不再写死：它们各有设置项（见下面三个 *96 函数）。
constexpr int kHeadHeight = 46;
constexpr int kStatusHeight = 26;
constexpr int kSearchHeight = 28;
constexpr int kFooterHeight = 30;
constexpr int kWheelHintHeight = 28;  // 左栏滚轮提示行
constexpr int kFormPad = 24;       // 中栏左右留白
constexpr int kNameCol = 186;      // 名称列宽
constexpr int kCtrlPad = 16;       // 控件列比值控件宽的右留白
constexpr int kDescGap = 18;       // 说明列与控件列之间的空隙
constexpr int kDescMinCol = 96;    // 中栏说明列的最小宽（再窄就归零 = 说明静默消失）
constexpr int kChkSize = 16;       // 复选框边长
// 所有"值控件"（下拉、数字、自由字符串）共用 settingsControlWidth：
constexpr int kValueUnitGap = 6;   // 数值与右侧单位之间的留白
// ---- 右栏 ----
// 右栏没有滚动条，所以"排不下"只有一种后果：下面那几节直接不存在。
// 因此块高与块间距必须**只有一份**，最小窗口高度由它反推（见 MinClientSize）。
constexpr int kRightPad = 18;         // 左右留白
constexpr int kRightTop = 16;         // 第一段标题距栏顶
constexpr int kRightDescMin = 72;     // 说明块最小高（约 4 行）
constexpr int kRightHelpHeight = 18;  // "何时生效"块高
constexpr int kHHead = 16;            // 段落标题
constexpr int kHName = 18;            // 名称
constexpr int kHChip = 21;            // 键名芯片
constexpr int kHType = 16;            // 类型 / 取值域
constexpr int kHRestore = 29;         // "恢复默认值"按钮（只读项放说明文字）
constexpr int kHLink = 18;            // 两个复制链接
constexpr int kGapHead = 18;          // 段落标题与其后内容之间
constexpr int kGapTight = 4;          // 同一段内部的两行之间
constexpr int kGapMed = 8;            // 按钮与链接之间
constexpr int kGapWide = 20;          // 大段之间（类型 → 相关操作 → 何时生效）
constexpr int kGapAfterDesc = 10;     // 说明块之后

// 右栏除说明块以外的固定高度合计。"少了这一截"就等于那一节不存在，
// 所以最小窗口高度必须由它 + 说明块最小高反推，不能手写手感值。
// 注意含 kGapAfterDesc：说明块自己的高是可变的，但它下面那道间距是固定的。
constexpr int kRightFixed = kGapAfterDesc + (kHHead + kGapHead) + (kHName + kGapTight) +
                            (kHChip + kGapMed) + (kHType + kGapWide) + (kHHead + kGapHead) +
                            (kHRestore + kGapMed) + (kHLink + kGapTight) + (kHLink + kGapWide) +
                            (kHHead + kGapHead) + kRightHelpHeight;

// 设置窗口三栏的运行宽度（96 DPI 逻辑值）。正式夹取在 config_io/settings_reg，
// 这里再夹一次是为了让"测试直接改 Settings"也拿不到 0 宽控件。
int GroupPaneWidth96() {
    return std::clamp(Config::Get().s().settingsGroupWidth, kSettingsGroupWidthMin,
                      kSettingsGroupWidthMax);
}
int DetailPaneWidth96() {
    return std::clamp(Config::Get().s().settingsDetailWidth, kSettingsDetailWidthMin,
                      kSettingsDetailWidthMax);
}
int ValueControlWidth96() {
    return std::clamp(Config::Get().s().settingsControlWidth, kSettingsControlWidthMin,
                      kSettingsControlWidthMax);
}

}  // namespace

// ---------------------------------------------------------------- 布局

void SettingsDialog::MinClientSize(const Theme& theme, int* w, int* h) {
    // 宽：左栏 + 右栏 + 中栏自身的最小内容宽（左右留白 + 名称列 + 说明列 + 空隙 + 控件列）。
    // 中栏比它窄时名称与控件会重叠、说明列宽度归零 —— 那是"静默消失"而不是
    // "看起来挤"，所以下限必须由这批常量反推，不能手写手感值。
    // kDescMinCol 给说明列留一个能显示几个字的下限：完全归零时那一列不是变窄，
    // 是**没有了**（DrawText 在零宽矩形里什么都不画，也没有省略号）。
    const int midMin = kFormPad * 2 + kNameCol + kDescMinCol + kDescGap +
                       ValueControlWidth96() + kCtrlPad;
    if (w) *w = theme.S(GroupPaneWidth96() + DetailPaneWidth96() + midMin);
    // 高：栏顶留白 + 右栏固定块 + 说明块最小高 + 状态栏。
    // 右栏没有滚动，少了这一截就等于那一节不存在。
    if (h)
        *h = theme.S(kRightTop + kRightFixed + kRightDescMin) + theme.S(kStatusHeight);
}

int SettingsDialog::RowHeight() const {
    return theme_ ? theme_->S(kRowHeights[density_]) : kDefaultRowHeightPx;
}

int SettingsDialog::EditLineHeight() const {
    const int fallback = theme_ ? theme_->S(18) : 18;
    if (!hwnd_ || !theme_) return fallback;
    HDC dc = GetDC(hwnd_);
    if (!dc) return fallback;
    const int h = ui::EditLineHeight(dc, theme_->font, fallback);
    ReleaseDC(hwnd_, dc);
    return h;
}

int SettingsDialog::CtrlHeight() const {
    const int pad = theme_ ? theme_->S(ui::kEditVPad96) : ui::kEditVPad96;
    // 控件高不是手写常量：文字行高 + 上下等宽留白，和导航栏输入框同一口径。
    return EditLineHeight() + pad * 2;
}

const wchar_t* SettingsDialog::UnitOf(const std::wstring& key) {
    // 只有这几项的单位是"一看数字就知道是什么"的那种；其余不写单位
    // （凭空给一个单位比不给更糟）。
    if (key == L"paneSplit") return L"%";
    if (key == L"seqTimeoutMs" || key == L"holdDelayMs" || key == L"aiTimeoutMs" ||
        key == L"tabAnimMs")
        return L"毫秒";
    // 宽度项都是像素：少写一个就只剩裸数字，"420" 会被读成别的量纲。
    if (key == L"tabMinWidth" || key == L"tabMaxWidth" || key == L"navPaneWidth" ||
        key == L"aiPanelWidth" || key == L"navBarSearchWidth" ||
        key == L"navBarAddressMinWidth" || key == L"bookmarkBarTooltipWidth" ||
        key == L"statusBarTooltipWidth" || key == L"settingsGroupWidth" ||
        key == L"settingsDetailWidth" || key == L"settingsControlWidth" ||
        key == L"navPaneIndent")
        return L"像素";
    if (key == L"tabBarHeight" || key == L"navBarHeight" || key == L"bookmarkBarHeight" ||
        key == L"statusBarHeight" || key == L"navPaneItemHeight")
        return L"像素";
    // 磁吸阈值是"越过边缘多少像素"，不写单位会被读成毫秒或逻辑档位。
    if (key == L"tabTearOffDy") return L"像素";
    // 三个历史上限的单位是"条"：不写的话 "30" 会被读成毫秒或像素。
    if (key == L"recentLimit" || key == L"searchLimit" || key == L"closedTabLimit") return L"条";
    return nullptr;
}

void SettingsDialog::Layout() {
    if (!hwnd_ || !theme_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int cx = rc.right;
    const int cy = rc.bottom;

    const int statusH = theme_->S(kStatusHeight);
    statusBar_ = {0, cy - statusH, cx, cy};

    const int bodyH = cy - statusH;
    const int colAW = theme_->S(GroupPaneWidth96());
    const int colCW = theme_->S(DetailPaneWidth96());
    colA_ = {0, 0, colAW, bodyH};
    colB_ = {colAW, 0, std::max(colAW + 1, cx - colCW), bodyH};
    colC_ = {colB_.right, 0, cx, bodyH};

    const int headH = theme_->S(kHeadHeight);
    head_ = {colB_.left, 0, colB_.right, headH};
    form_ = {colB_.left, headH, colB_.right, bodyH};

    // 左栏：搜索框 → 分类列表 → 滚轮提示 → 底部计数。
    const int pad = theme_->S(12);
    searchBox_ = {colA_.left + pad, pad, colA_.right - pad, pad + theme_->S(kSearchHeight)};
    const int footH = theme_->S(kFooterHeight);
    foot_ = {colA_.left,
             std::max<LONG>(static_cast<LONG>(searchBox_.bottom), static_cast<LONG>(bodyH - footH)),
             colA_.right, bodyH};
    // 滚轮提示行：夹在列表与页脚之间。提示行不够高时它被压扁而不是消失 ——
    // 窗口被压得很扁时，少一行提示比多一行提示重要。
    const int hintH = theme_->S(kWheelHintHeight);
    hint_ = {colA_.left,
             std::max<LONG>(static_cast<LONG>(searchBox_.bottom), static_cast<LONG>(foot_.top - hintH)),
             colA_.right, foot_.top};
    catList_ = {colA_.left, searchBox_.bottom + theme_->S(8), colA_.right, hint_.top};

    // 左栏分类列表的滚动上限：分组多于可用高度时才有。与中栏同一套口径 ——
    // 可视高度是**矩形自己的高**，不是窗口高：两侧都夹着搜索框/提示行/页脚。
    const int gTotal = static_cast<int>(groups_.size()) * RowHeight();
    groupScrollMax_ = std::max(0, gTotal - static_cast<int>(catList_.bottom - catList_.top));
    groupScroll_ = std::max(0, std::min(groupScroll_, groupScrollMax_));

    // 头部右侧两个按钮（密度 / 帮助）。
    const int btn = theme_->S(26);
    rcHelp_ = {colB_.right - theme_->S(24) - btn, headH / 2 - btn / 2,
                colB_.right - theme_->S(24), headH / 2 - btn / 2 + btn};
    const int densW = theme_->S(72);
    rcDensity_ = {rcHelp_.left - theme_->S(6) - densW, rcHelp_.top, rcHelp_.left - theme_->S(6),
                  rcHelp_.bottom};

    LayoutRows();
    LayoutRight();

    // 搜索框里的 EDIT：贴着放大镜图标右边放。
    // 纵向不铺满框：与导航栏输入框一样，只占实测文字行高并在框内居中，
    // 于是文字上方/下方的留白必然相等（铺满整框会让单行 EDIT 文字贴顶）。
    if (searchEdit_) {
        const int iconW = theme_->S(22);
        const int lineH = EditLineHeight();
        const RECT line = ui::CenteredLineRect(searchBox_, lineH);
        const RECT er{searchBox_.left + iconW, line.top,
                      searchBox_.right - theme_->S(8), line.bottom};
        MoveWindow(searchEdit_, er.left, er.top, er.right - er.left, er.bottom - er.top, TRUE);
    }
    // 行内编辑框跟着被编辑的那一格走（数字框本体，不含单位）。
    // 定位逻辑收在 PlaceValueEdit 里：Layout 与 ScrollBy 都会动 rowLayout_。
    PlaceValueEdit();
}

void SettingsDialog::PlaceValueEdit() {
    if (!valueEdit_) return;
    if (editKey_.empty()) {
        ShowWindow(valueEdit_, SW_HIDE);
        return;
    }
    // 用键名重新定位下标：Layout 可能紧跟在 RebuildModel 之后被调用。
    const int idx = RowIndexByKey(editKey_);
    if (idx < 0 || idx >= static_cast<int>(rowLayout_.size())) {
        // 编辑的项已不在可见行里：收起编辑框，别让它浮在别的行上。
        ShowWindow(valueEdit_, SW_HIDE);
        editKey_.clear();
        return;
    }
    const RowLayout& rl = rowLayout_[static_cast<size_t>(idx)];
    // 滚出可视区就跟着收起（保留编辑态）：留一个输入框在中栏头部上，
    // 用户会以为那一行还在。
    const RECT& row = rl.row;
    if (row.bottom <= form_.top || row.top >= form_.bottom) {
        ShowWindow(valueEdit_, SW_HIDE);
        return;
    }
    // EDIT 只占 rl.edit（box 的横向 + line 的纵向），不是整个 box ——
    // 与导航栏地址框同一条规则：单行 EDIT 铺满整框会把文字贴顶。
    const RECT& n = rl.edit;
    MoveWindow(valueEdit_, n.left, n.top, n.right - n.left, n.bottom - n.top, TRUE);
    ShowWindow(valueEdit_, SW_SHOW);
}

void SettingsDialog::LayoutRows() {
    rowLayout_.assign(rows_.size(), RowLayout{});
    const int rh = RowHeight();
    const int ch = CtrlHeight();            // text lineH + 上下各 kEditVPad96
    const int lineH = EditLineHeight();
    const int padX = theme_ ? theme_->S(kFormPad) : 24;
    const int nameW = theme_ ? theme_->S(kNameCol) : 186;
    const int ctrlW = theme_ ? theme_->S(ValueControlWidth96() + kCtrlPad)
                             : ValueControlWidth96() + kCtrlPad;
    const int gap = theme_ ? theme_->S(kDescGap) : 18;

    // 【先夹滚动量，再算矩形】每一项的高度都是 rh（标题条同高），所以内容总高
    // 不需要试算一遍：项数 × 行高。夹必须在算矩形**之前** —— 反过来的话，
    // 换到项数更少的一组时 scroll_ 会先被用进坐标、再被夹小，于是整列行停在
    // 上一组的高度上：界面看起来"滚不动"，实际是算错了起点。
    const int viewH = static_cast<int>(form_.bottom - form_.top);
    const int contentH = static_cast<int>(items_.size()) * rh;
    scrollMax_ = std::max(0, contentH - viewH);
    scroll_ = std::max(0, std::min(scroll_, scrollMax_));

    // 量单位文字要一个 DC。这里取一次用完就放 —— 排版不在 WM_PAINT 里做，
    // 所以不存在"绘制中取 DC 改画布"的问题。
    HDC dc = hwnd_ ? GetDC(hwnd_) : nullptr;

    int y = form_.top - scroll_;
    for (auto& item : items_) {
        // 每一项（标题条与行）都要有自己的矩形 —— 命中测试（HitItem）与
        // 滚动定位（EnsureVisible）读的就是这份。
        //
        // 【曾经漏掉行的那一份】早先只在"标题条"这一支里赋 item.rc，
        // 普通行拿到的是默认的空矩形 —— 于是 HitItem 永远命中不到行，
        // 表现是"点行没反应"（点标题条倒是有反应，所以很容易误判为
        // "行不能点，只能点分类"）。这种错没有任何检查会报：截图照常画得
        // 出来，因为绘制读的是 rowLayout_（另一份矩形），只有命中读 item.rc。
        item.rc = {form_.left, y, form_.right, y + rh};
        if (item.header) {
            y += rh;
            continue;
        }
        RowLayout& rl = rowLayout_[static_cast<size_t>(item.rowIndex)];
        const Row& row = rows_[static_cast<size_t>(item.rowIndex)];
        // 行的矩形**就是**项的矩形：命中测试与绘制共用同一份，
        // 两处各存一份就迟早对不上（对不上的表现是"点 A 行改了 B 行"）。
        rl.row = item.rc;
        // 这一行共用的文字中心线：自绘文字与原生 EDIT 都按这条线定位。
        const int lineTop = y + (rh - lineH) / 2;
        rl.line = {form_.left, lineTop, form_.right, lineTop + lineH};
        rl.name = {form_.left + padX, lineTop, form_.left + padX + nameW, lineTop + lineH};
        rl.ctrl = {form_.right - padX - ctrlW, y, form_.right - padX, y + rh};
        // 说明列紧跟名称列：**左边不留空隙**。
        // 原型（app.css 的 grid-template-columns: 186px minmax(0,1fr) 208px）
        // 就是三列并排，说明列从名称列右缘开始，空隙放在右边（padding-right:18px）——
        // 空隙放在右边才填得满：放左边会把说明挤窄，白白少显示几个字。
        rl.desc = {rl.name.right, lineTop, std::max(rl.name.right, rl.ctrl.left - gap),
                   lineTop + lineH};

        const int ctlTop = y + (rh - ch) / 2;
        rl.unit = {};
        // 【枚举优先】带 options 的项一律画下拉（含布尔型枚举），所以这条要排在
        // Bool 之前 —— 反过来的话 dualPane 会得到一个复选框，而"单栏/双栏"两个
        // 状态在复选框上表达不了"取消双栏之后右栏去哪"这层意思。
        if (!row.entry->options.empty()) {
            const int w = theme_ ? theme_->S(ValueControlWidth96()) : ValueControlWidth96();
            rl.box = {rl.ctrl.right - w, ctlTop, rl.ctrl.right, ctlTop + ch};
        } else if (row.entry->kind == settings_reg::Kind::Bool) {
            const int s = theme_ ? theme_->S(kChkSize) : 16;
            rl.box = {rl.ctrl.right - s, y + (rh - s) / 2, rl.ctrl.right, y + (rh - s) / 2 + s};
        } else if (row.entry->kind == settings_reg::Kind::Int) {
            // 数值框和下拉/字符串框一样宽。单位画在框**内部**右侧：以前的
            // "窄数字框 + 框外单位"让一排输入框长短不一，还会把单位文字挤到
            // 控件列外面。原生 EDIT 只占框左侧到单位之前，编辑时不会盖住单位。
            const wchar_t* unit = UnitOf(row.entry->key);
            const int w = theme_ ? theme_->S(ValueControlWidth96()) : ValueControlWidth96();
            rl.box = {rl.ctrl.right - w, ctlTop, rl.ctrl.right, ctlTop + ch};
            int unitW = 0;
            if (unit && dc) unitW = ui::MeasureText(dc, theme_->font, unit);
            if (unitW) {
                const int ugap = theme_ ? theme_->S(kValueUnitGap) : kValueUnitGap;
                rl.unit = {rl.box.right - ugap - unitW, lineTop, rl.box.right - ugap,
                           lineTop + lineH};
            }
        } else if (row.entry->kind == settings_reg::Kind::Str && row.writable) {
            // 可写的自由字符串（当前只有 searchRangePath）：给它一个真正的输入框
            // 位置。不写这一支它会掉进下面那个 else（只读复合值的摘要），于是
            // "表里声明可写、界面上没有任何能点的地方"—— 点下去只得到一句
            // "请用命令改"。表与界面必须对同一批项给出同一个答案。
            const int w = theme_ ? theme_->S(ValueControlWidth96()) : ValueControlWidth96();
            rl.box = {rl.ctrl.right - w, ctlTop, rl.ctrl.right, ctlTop + ch};
        } else {
            // 只读的复合值（数组 / 对象）：没有控件盒子，右端画一句摘要。
            rl.box = {rl.ctrl.left, lineTop, rl.ctrl.right, lineTop + lineH};
        }
        // 原生 EDIT 的窗口：横向跟控件盒，纵向跟文字行。
        // 带单位的数字框要让出单位那一截（框仍是统一宽度，只是 EDIT 不盖到
        // 单位上）—— 否则编辑态的数字会压在"毫秒"两个字上面。
        rl.edit = {rl.box.left, lineTop, rl.box.right, lineTop + lineH};
        if (rl.unit.right > rl.unit.left) {
            const int gap = theme_ ? theme_->S(kValueUnitGap) : kValueUnitGap;
            rl.edit.right = std::max(rl.edit.left, rl.unit.left - gap);
        }
        y += rh;
    }
    if (dc) ReleaseDC(hwnd_, dc);
}

void SettingsDialog::LayoutRight() {
    right_ = RightLayout{};
    const auto S = [&](int v) { return theme_ ? theme_->S(v) : v; };
    const int left = static_cast<int>(colC_.left) + S(kRightPad);
    const int right =
        std::max(left + 10, static_cast<int>(colC_.right) - S(kRightPad));

    // 说明块的高度由**文字本身**决定（量一次），不写死。
    // 【为什么这是根因级的】固定高度 + DT_WORDBREAK 的组合只有两种结局：
    // 长说明被裁掉（用户看不出少了什么，只知道句子断在半路），或留一大片空白。
    // 而"说明全文只在这里能看到"，所以裁掉就等于那一项没有说明。
    int descH = S(kRightDescMin);
    if (RowByKey(selKey_)) {
        const std::wstring desc = RightDescText();   // 与 PaintRight 同一份文本
        if (HDC dc = hwnd_ ? GetDC(hwnd_) : nullptr) {
            const RECT box{0, 0, right - left, 0};
            const int need = ui::MeasureTextHeight(dc, theme_->font, desc, box,
                                                   DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            descH = std::max(descH, need + S(4));
            ReleaseDC(hwnd_, dc);
        }
    }
    // 可用高度：栏高减去固定的那些块。夹一下 —— 说明块的"该多高"不能撑破栏。
    const int avail = static_cast<int>(colC_.bottom - colC_.top) - S(kRightTop) - S(kRightFixed);
    descH = std::clamp(descH, S(kRightDescMin), std::max(S(kRightDescMin), avail));

    // 逐块往下走：块高与间距来自同一批常量，顺序即绘制顺序。
    int y = static_cast<int>(colC_.top) + S(kRightTop);
    auto block = [&](int h, int gapAfter) {
        RECT r{left, y, right, y + S(h)};
        y += S(h) + S(gapAfter);
        return r;
    };

    right_.head1 = block(kHHead, kGapHead);
    right_.name = block(kHName, kGapTight);
    // 说明块：高度是量出来的，所以不吃 block() 的固定高。
    right_.desc = {left, y, right, y + descH};
    y = right_.desc.bottom + S(kGapAfterDesc);
    right_.chip = block(kHChip, kGapMed);
    right_.type = block(kHType, kGapWide);
    right_.head2 = block(kHHead, kGapHead);
    right_.restore = block(kHRestore, kGapMed);
    right_.copyKey = block(kHLink, kGapTight);
    right_.copyCmd = block(kHLink, kGapWide);
    right_.head3 = block(kHHead, kGapHead);
    right_.help = block(kRightHelpHeight, 0);
}

// ---------------------------------------------------------------- 命中

int SettingsDialog::HitGroup(int x, int y) const {
    // 视口检查走 catList_ 本身：列表只占左栏中段（上下还有搜索框 / 提示行 /
    // 页脚），整个左栏都算"命中"的话，点搜索框下面那行空白会切换分类。
    POINT pt{x, y};
    if (!PtInRect(&catList_, pt)) return -1;
    const int rh = RowHeight();
    if (rh <= 0) return -1;
    // 加上滚动量：分类行画在 catList_.top - groupScroll_ 之上，命中必须用同一原点。
    const int idx = (y - catList_.top + groupScroll_) / rh;
    if (idx < 0 || idx >= static_cast<int>(groups_.size())) return -1;
    return idx;
}

int SettingsDialog::HitItem(int x, int y) const {
    POINT pt{x, y};
    // 视口检查：中栏可滚动，行的矩形会跑到 form_ 之外（上下两侧都有）。
    // 不判视口的话，滚下去的那几行仍然"在那"，于是点中栏头部（面包屑 / 密度 /
    // 帮助）或者状态栏时会命中一行看不见的设置 —— 表现是"点标题栏改了设置"。
    if (!PtInRect(&form_, pt)) return -1;
    for (size_t i = 0; i < items_.size(); ++i)
        if (PtInRect(&items_[i].rc, pt)) return static_cast<int>(i);
    return -1;
}

int SettingsDialog::HitRightAction(int x, int y) const {
    POINT pt{x, y};
    // "有没有选中项"是派生状态，现算（见 RightLayout 的说明）。
    const Row* row = RowByKey(selKey_);
    if (!row) return 0;
    // 只读项：右栏只画了"复制命令模板"这一个链接，另外两个位置放的是说明文字。
    // 命中必须跟着可写性走 —— 否则点那段说明文字会执行"恢复默认值"。
    if (!row->writable) return PtInRect(&right_.copyCmd, pt) ? 3 : 0;
    if (PtInRect(&right_.restore, pt)) return 1;
    if (PtInRect(&right_.copyKey, pt)) return 2;
    if (PtInRect(&right_.copyCmd, pt)) return 3;
    return 0;
}

int SettingsDialog::HitHeadAction(int x, int y) const {
    POINT pt{x, y};
    if (PtInRect(&rcDensity_, pt)) return 1;
    if (PtInRect(&rcHelp_, pt)) return 2;
    return 0;
}
