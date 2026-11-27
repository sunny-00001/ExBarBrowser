// 快捷键管理器：排布与命中。
//
// 【排布与命中必须同住一个文件】它们共用 rowLayout_：画的时候按它画，点的
// 时候按它判。两处各算一遍几何，差一个像素就是"点键位芯片却改了下一行"，
// 而这种错在界面上完全看不出规律，只能靠对着代码数像素。
//
// 【所有尺寸都从 Theme 来】不写死像素：128% / 168% DPI 下写死的 30 会变成
// "行高还是 30 物理像素、字已经 20 像素"——文字被裁一半，而代码里毫无异常。
#include "hotkey_manager.h"

#include <algorithm>

#include "config.h"
#include "settings_bounds.h"
#include "ui_draw.h"

namespace {

// 三栏的基础宽度（96 DPI 逻辑值）。左栏只放筛选项与计数，够放下"系统对话框"
// 四个字 + 图标 + 计数；右栏要放下一个键位芯片与三四行说明。
//
// 【为什么这几个数现在从 Config 读】它们以前是这里的 constexpr —— 用户看得见的
// 界面却改不动，而"行距太挤""左栏太窄"正是最常被提的两条。现在落进 settings.json
// （hkRowHeight / hkRailWidth / ...），走 settings_reg 的夹取与命令入口。
// 常量只剩"中栏最小宽"这类**不是偏好而是下限**的值。
constexpr int kMidMinW96 = 420;
// 中栏表格的列宽：状态列要放下"系统占用"，芯片列要放下 "Ctrl+Shift+PageDown"。
constexpr int kStatusW96 = 88;
constexpr int kChipW96 = 152;
constexpr int kScopeW96 = 88;
constexpr int kPad96 = 12;

// 从配置读尺寸，并**夹在合法区间**。
//
// 【为什么要夹】config_io 读盘时已经夹过一次，但这里是运行期每次重排都会走的路：
// 用户用 config.set 改了、或者有人直接改了内存里的 Settings，界面不该因此画出
// 一个 0 高的工具栏。夹取用 settings_bounds.h 的同一对边界 —— 与命令 schema、
// 与读盘端是同一份数字（三个地方各写一遍就是"设置自己变回去"的来源）。
int CfgRowHeight() {
    return std::clamp(Config::Get().s().hkRowHeight, kHkRowHeightMin, kHkRowHeightMax);
}
int CfgRailWidth() {
    return std::clamp(Config::Get().s().hkRailWidth, kHkRailWidthMin, kHkRailWidthMax);
}
int CfgDetailWidth() {
    return std::clamp(Config::Get().s().hkDetailWidth, kHkDetailWidthMin, kHkDetailWidthMax);
}

}  // namespace

// 工具栏按钮表（唯一来源）。布局、命中、绘制、启用判定都读这一份。
//
// 【为什么用数据表而不是一串手写矩形】加一颗按钮要同步改四处（布局/命中/
// 绘制/禁用），手写必然漏一处；漏了的表现是"按钮画出来了但点不动"或反过来。
//
// 【为什么是成员函数里的静态数组】ToolButton 是私有嵌套类型，放在匿名
// 命名空间里编不过；做成成员函数既满足访问权限，又保证只有一份实例。
const HotkeyManager::ToolButton* HotkeyManager::ToolbarItems(int* count) {
    static const ToolButton kItems[] = {
        // 两张表的开关（并排两颗，当前那颗填强调色）。
        {kToolTabChord, ico::IconId::Keyboard, L"组合键", L"可改键的组合键表（回车改键 / Delete 清除）"},
        {kToolTabSeq, ico::IconId::Bolt, L"序列键", L"多键序列表（只读参考：改 keymap.json 的 sequences）"},
        {kToolNone, ico::IconId::None, L"", L""},   // 分隔
        {kToolEdit, ico::IconId::Edit, L"改键", L"为选中的命令录制新组合键（回车 / 双击行）"},
        {kToolClear, ico::IconId::Close, L"清除", L"解绑选中的命令（Delete）"},
        {kToolReset, ico::IconId::Undo, L"还原", L"把选中的命令还原成默认键位（F2）"},
        {kToolNone, ico::IconId::None, L"", L""},   // 分隔
        {kToolUndo, ico::IconId::Undo, L"撤销", L"撤销上一次改键（Ctrl+Z）"},
        {kToolRedo, ico::IconId::Refresh, L"重做", L"重做被撤销的改键（Ctrl+Shift+Z）"},
        {kToolNone, ico::IconId::None, L"", L""},   // 分隔
        {kToolCopyId, ico::IconId::Copy, L"复制 id", L"复制选中命令的 id（Ctrl+C）"},
        {kToolExport, ico::IconId::Export, L"导出", L"把自定义键位导出到剪贴板（JSON）"},
        {kToolImport, ico::IconId::Import, L"导入", L"从剪贴板导入键位覆盖（JSON）"},
        {kToolNone, ico::IconId::None, L"", L""},   // 分隔
        {kToolPreset, ico::IconId::Settings, L"方案", L"套用一套键位方案（资源管理器 / TC / Opus / Vim）"},
    };
    if (count) *count = static_cast<int>(std::size(kItems));
    return kItems;
}

void HotkeyManager::MinClientSize(const Theme& theme, int* w, int* h) {
    // 最小宽度 = 三栏 + 两条内边距（栏与栏之间不重叠，所以只加 pad 不加边框）。
    if (w) *w = theme.S(CfgRailWidth()) + theme.S(kMidMinW96) + theme.S(CfgDetailWidth());
    // 最小高度 = 工具栏 + 表头 + 8 行 + 状态栏 + 上下留白。前四项各占**一行**，
    // 所以写成一个行高乘 11 —— 行高改了它自动跟着走。以前这里还多算了一条
    // "34"（那是搜索框独占中栏顶部时的残留），工具栏与行高统一后那条已经不存在了。
    // 8 行不是随手定的：短于这个高度，列表就只剩两三行，"筛选"这个动作
    // 看不到结果，界面等于坏了。
    if (h) *h = theme.S(CfgRowHeight()) * 11 + theme.S(24);
}

int HotkeyManager::RowHeight() const { return theme_->S(CfgRowHeight()); }

void HotkeyManager::Layout() {
    if (!hwnd_ || !theme_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // 工具栏、表头、行、状态栏**同高**：它们都是"一行"，只是内容不同。
    //
    // 【工具栏为什么不再有自己的高度】它以前是独立设置项（默认 40），于是
    // 顶栏比列表每一行都高，两套节奏叠在同一屏里 —— 看着就是"工具栏胖了"。
    // 一个数管到底之后，"工具栏比行高"这种不一致从结构上不可能出现；
    // 想让整体更疏就调 hkRowHeight，四处一起变。
    const int rowH = theme_->S(CfgRowHeight());
    const int statusH = rowH;
    const int toolH = rowH;

    statusBar_ = {0, rc.bottom - statusH, rc.right, rc.bottom};
    toolbar_ = {0, 0, rc.right, toolH};
    // 三栏从工具栏下沿开始：工具栏是**通栏**的，不属于任何一栏。
    // 这样它才能同时管左栏（筛选）与中栏（排序/改键）的动作 ——
    // 嵌进某一栏会让另一栏的动作看起来"不在这栏里"。
    const int top = toolH;
    const int topH = rc.bottom - statusH;

    colA_ = {0, top, theme_->S(CfgRailWidth()), topH};
    colC_ = {rc.right - theme_->S(CfgDetailWidth()), top, rc.right, topH};
    colB_ = {colA_.right, top, colC_.left, topH};

    LayoutToolbar();

    // ---- 左栏：标题 + 筛选项 + 页脚计数 ----
    railList_ = {colA_.left + theme_->S(6), colA_.top + theme_->S(44), colA_.right - theme_->S(6),
                 colA_.bottom - theme_->S(36)};
    railFoot_ = {colA_.left, colA_.bottom - theme_->S(34), colA_.right, colA_.bottom};

    // ---- 中栏：表头 + 行 ----
    //
    // 【搜索框为什么搬去工具栏】它原来是中栏顶部的一条独立头带，占掉一整行
    // 高度（~34px），而那一行只放了一个输入框与一颗录制按钮。搬到工具栏右端后
    // 省下这一整行给列表，且"搜索"与"动作"落在同一条视觉带上 —— 它们本来
    // 就是同一件事的两半（找到它 → 改它）。
    //
    // 【这里以前还有两行清零】`searchBox_ = {}` / `recordBtn_ = {}` 是搬家的残留：
    // 它们在 LayoutToolbar() **之后**执行，把刚算好的录制按钮矩形抹成零 ——
    // 于是那颗按钮既画不出来也点不到（PtInRect 对零矩形恒假），而代码里看着
    // 像"已经挪到工具栏了"。搬走的东西就不要再在原处清一遍。
    tableHead_ = {colB_.left, colB_.top, colB_.right, colB_.top + rowH};
    body_ = {colB_.left, tableHead_.bottom, colB_.right, colB_.bottom - theme_->S(6)};

    // 真正的 EDIT 落在画出来的圆角框里。左右各缩 2px 让圆角露出来，否则
    // 矩形的 EDIT 会把四个角切平 —— 看着像"框没画完"。
    if (searchEdit_ && searchField_.right > searchField_.left) {
        const int iconPx = theme_->iconSmall();
        const int lineH = ui::EditLineHeight(nullptr, theme_->font, theme_->S(18));
        RECT inner{searchField_.left + theme_->S(8) + iconPx + theme_->S(6),
                   searchField_.top + theme_->S(2), searchField_.right - theme_->S(2),
                   searchField_.bottom - theme_->S(2)};
        const RECT line = ui::CenteredLineRect(inner, lineH);
        MoveWindow(searchEdit_, line.left, line.top, std::max(1, static_cast<int>(line.right - line.left)),
                   std::max(1, static_cast<int>(line.bottom - line.top)), TRUE);
    }

    LayoutRows();
    LayoutRight();
}

void HotkeyManager::LayoutToolbar() {
    if (!theme_) return;
    toolRects_.clear();
    int n = 0;
    const ToolButton* items = ToolbarItems(&n);
    // 按钮高度 = 栏高减上下各 2px。栏高现在跟行高走（见 Layout），所以这里
    // **必须由栏高推出来**，不能再挂一个独立常量 —— 行高调到 22 时那个常量
    // 会让按钮顶出栏外，而界面上只表现为"按钮压着分隔线"。
    const int barH = toolbar_.bottom - toolbar_.top;
    const int btnH = std::max(theme_->S(16), barH - theme_->S(4));
    const int y = toolbar_.top + (barH - btnH) / 2;
    // 分组内边距与组间距：组间距明显大于组内，才有"这是两组"的读法。
    const int padIn = theme_->S(4);
    const int padGroup = theme_->S(14);
    const int iconPx = theme_->iconSmall();
    int x = toolbar_.left + theme_->S(10);

    HDC dc = GetDC(hwnd_);
    for (int i = 0; i < n; ++i) {
        const ToolButton& b = items[i];
        if (b.id == kToolNone) {
            // 分隔符：位置在布局里就定下来（画的时候直接用这一份）。
            //
            // 【为什么不再"画的时候从相邻按钮反推中点"】那套算法依赖"分隔符前面
            // 恰好有 i 颗按钮"这个巧合才能按下标取到正确的矩形；加一颗按钮、
            // 或两个分隔符相邻，就会取到别的按钮上（甚至越界）。
            // 现在它跟按钮一样是一条记录，只是 id = kToolNone（命中时天然等于
            // "没命中"），于是"有几颗按钮、几条线"两处不再各算一遍。
            const int sx = x + padGroup / 2;
            ToolRect tr;
            tr.id = kToolNone;
            tr.rc = {sx, toolbar_.top + theme_->S(6), sx + 1, toolbar_.bottom - theme_->S(6)};
            toolRects_.push_back(tr);
            x += padGroup;
            continue;
        }
        // 按钮宽度 = 图标 + 间距 + 文字宽 + 左右内边距。
        // 量字必须用与绘制同一个字体，否则按钮会窄一点（文字被截）。
        const int textW = b.text && *b.text
                              ? ui::MeasureText(dc, theme_->font, b.text, -1)
                              : 0;
        const int w = padIn * 2 + iconPx + (textW > 0 ? theme_->S(6) + textW : 0);
        ToolRect tr;
        tr.id = b.id;
        tr.rc = {x, y, x + w, y + btnH};
        toolRects_.push_back(tr);
        x += w + theme_->S(2);
    }
    ReleaseDC(hwnd_, dc);

    // ---- 右端：搜索框 + 录制按钮，吃按钮剩下的空间 ----
    //
    // 【顺序是"先按钮、后搜索框"】按钮宽度由内容定死（文字不能截），搜索框
    // 能屈能伸，所以后者吃剩余。这里必须等按钮排完才知道剩多少 —— 以前是
    // 先按固定 220 摆好搜索框、再看左侧够不够，于是加两颗按钮（表开关）之后
    // 它要么压住按钮、要么整块消失，两种都不对。
    //
    // 【先缩后收】剩余空间不足 220 时按剩余给，低到 140 以下才整块收掉：
    // 从"能打字"到"没有"之间应该有一段"窄但能用"，否则窗口稍微窄一点搜索框
    // 就凭空消失，用户会以为搜索功能坏了。
    const int rightIn = toolbar_.right - theme_->S(10);
    const int recW = theme_->S(32);
    const int gap = theme_->S(6);
    recordBtn_ = {rightIn - recW, y, rightIn, y + btnH};
    const int avail = recordBtn_.left - gap - x;
    const int searchW = std::min(theme_->S(220), avail);
    if (searchW < theme_->S(140)) {
        // 按钮永远优先 —— 看不见搜索框只是不方便，看不见按钮是功能缺失。
        searchField_ = {};
        recordBtn_ = {};
    } else {
        searchField_ = {recordBtn_.left - gap - searchW, y, recordBtn_.left - gap, y + btnH};
    }
}

bool HotkeyManager::ToolEnabled(int id) const {
    const Row* row = Selected();
    switch (id) {
    case kToolTabChord:
    case kToolTabSeq:
        // 表开关永远可点：点当前这张表是"没有变化"，不是"不允许"。
        // 禁用它会让用户以为另一张表也坏了。
        return true;
    case kToolEdit:
    case kToolClear:
    case kToolReset:
        // 序列键不能在这里改（要动 keymap.json 的状态机）；没选中也不能。
        return row && !row->sequence;
    case kToolUndo:
        return !undo_.empty();
    case kToolRedo:
        return !redo_.empty();
    case kToolCopyId:
        return row != nullptr || !multiSel_.empty();
    case kToolExport:
        // 没有任何自定义时导出得到空表 —— 那是"按了没反应"的观感，直接禁用。
        return totalCustom_ > 0;
    case kToolImport:
        return true;   // 能不能导入要看剪贴板内容，点下去再判并说清原因
    case kToolPreset:
        // 方案与选中行无关：套用是整表动作，任何时候都该能点。
        //
        // 【这条曾经漏过】新增枚举没写分支时落到 default 的 false —— 按钮画成
        // 禁用色，点上去什么都不发生。而"能不能点"与"点了做什么"在两个文件里
        // （layout / actions），编译期不会提示漏了哪一个。
        return true;
    case kToolNone:
        // 分隔符不是按钮，永远不可点。显式列出它，是为了让上面每一个 ToolId
        // 都出现在 case 里 —— 这样新增枚举项时 -Wswitch 会报"未处理"，
        // 而不是静默落到 default。
        return false;
    }
    // 走到这里说明枚举加了新值却没加 case。-Wswitch 已经会报，这里再兜一层。
    return false;
}

int HotkeyManager::HitTool(int x, int y) const {
    const POINT pt{x, y};
    for (const ToolRect& tr : toolRects_)
        if (PtInRect(&tr.rc, pt)) return tr.id;
    return kToolNone;
}

int HotkeyManager::HitSortHeader(int x, int y) const {
    const POINT pt{x, y};
    if (!PtInRect(&tableHead_, pt)) return -1;
    // 直接读 LayoutRows 算好的列矩形 —— 与绘制表头同一份，不再各反推一遍。
    for (const Column& c : columns_)
        if (PtInRect(&c.rc, pt)) return c.sortKey;
    return -1;
}

void HotkeyManager::LayoutRows() {
    if (!theme_) return;
    const int pad = theme_->S(kPad96);
    const int rh = RowHeight();
    const int right = body_.right - pad;
    const int statusW = theme_->S(kStatusW96);
    const int chipW = theme_->S(kChipW96);
    const int scopeW = theme_->S(kScopeW96);

    const int statusL = right - statusW;
    const int chipL = statusL - theme_->S(10) - chipW;
    const int scopeL = chipL - theme_->S(12) - scopeW;
    const int titleL = body_.left + pad + theme_->S(16);   // 16 = 自定义圆点的位置

    // ---- 表头列：在这里算一次，绘制与排序命中都读 columns_ ----
    //
    // 【为什么不再"从第一行反推"】原来绘制表头时取 rowLayout_[0] 的列矩形反推，
    // 命中时又反推一遍。两份反推在"视图为空"时各走一条兜底分支，而那两条
    // 兜底是各写一遍的常量 —— 于是搜索无结果时点表头可能排错列。
    // 现在只有这一处算，两边都读同一份。
    columns_.clear();
    columns_.push_back({kSortTitle, L"命令", {titleL, tableHead_.top, scopeL - theme_->S(8),
                                              tableHead_.bottom}});
    columns_.push_back({kSortScope, L"范围", {scopeL, tableHead_.top, scopeL + scopeW,
                                              tableHead_.bottom}});
    columns_.push_back({kSortKey, L"键位", {chipL, tableHead_.top, chipL + chipW,
                                            tableHead_.bottom}});
    columns_.push_back({kSortStatus, L"状态", {statusL, tableHead_.top, right,
                                               tableHead_.bottom}});

    if (rowLayout_.size() != view_.size()) rowLayout_.assign(view_.size(), RowLayout{});
    for (int vi = 0; vi < static_cast<int>(view_.size()); ++vi) {
        const int top = body_.top + vi * rh - scroll_;
        RowLayout& rl = rowLayout_[static_cast<size_t>(vi)];
        rl.row = {body_.left, top, body_.right, top + rh};
        rl.title = {titleL, top, scopeL - theme_->S(8), top + rh};
        rl.scope = {scopeL, top, scopeL + scopeW, top + rh};
        rl.chip = {chipL, top + theme_->S(2), chipL + chipW, top + rh - theme_->S(2)};
        rl.status = {statusL, top, right, top + rh};
    }

    // 滚动范围：内容总高减视口高。少算一次就会"滚到底还差半行"，
    // 或者"内容早没了还能继续滚"（滚出大片空白）。
    const int total = static_cast<int>(view_.size()) * rh;
    const int viewH = body_.bottom - body_.top;
    scrollMax_ = std::max(0, total - viewH);
    scroll_ = std::min(scroll_, scrollMax_);
    scroll_ = std::max(0, scroll_);
}

void HotkeyManager::LayoutRight() {
    if (!theme_) return;
    const int pad = theme_->S(kPad96);
    detailBox_ = {colC_.left + pad, colC_.top + theme_->S(10), colC_.right - pad,
                  colC_.bottom - theme_->S(10)};

    const int l = detailBox_.left;
    const int r = detailBox_.right;
    const int lineH = theme_->S(20);
    int y = detailBox_.top;

    detail_.title = {l, y, r, y + theme_->S(46)};   // 两行
    y = detail_.title.bottom + theme_->S(2);
    detail_.cmd = {l, y, r, y + lineH};
    y = detail_.cmd.bottom + theme_->S(14);
    detail_.keyLabel = {l, y, r, y + lineH};
    y = detail_.keyLabel.bottom + theme_->S(4);
    // 键位芯片高度与列表行同高：芯片是"一行里的那个东西"，
    // 它比行还高的话，右栏的芯片与中栏的芯片看起来就不是同一种控件。
    detail_.keyChip = {l, y, r, y + theme_->S(CfgRowHeight())};
    y = detail_.keyChip.bottom + theme_->S(10);
    detail_.meta = {l, y, r, y + lineH};
    y = detail_.meta.bottom + theme_->S(2);
    detail_.defaultKey = {l, y, r, y + lineH};

    // 四颗按钮自下而上叠：还原全部（破坏性，最靠下、离主操作最远）/
    // 还原此项 / 清除 / 编辑。主操作放最上面，因为它是这个面板 90% 的动作。
    const int btnH = theme_->S(28);
    const int gap = theme_->S(6);
    detail_.resetAll = {l, detailBox_.bottom - btnH, r, detailBox_.bottom};
    detail_.reset = {l, detail_.resetAll.top - gap - btnH, r, detail_.resetAll.top - gap};
    detail_.clear = {l, detail_.reset.top - gap - btnH, r, detail_.reset.top - gap};
    detail_.edit = {l, detail_.clear.top - gap - btnH, r, detail_.clear.top - gap};
    detail_.hint = {l, detail_.edit.top - theme_->S(8) - theme_->S(32), r,
                    detail_.edit.top - theme_->S(8)};
    // 冲突说明吃掉中间剩下的空间（可能三四行）。
    detail_.conflict = {l, detail_.defaultKey.bottom + theme_->S(10), r,
                        std::max(detail_.defaultKey.bottom + theme_->S(10), detail_.hint.top - theme_->S(6))};
}

int HotkeyManager::HitGroup(int x, int y) const {
    if (!theme_) return -1;
    const POINT pt{x, y};
    if (!PtInRect(&railList_, pt)) return -1;
    // 与绘制同一个行高（RowHeight）：左栏与中栏现在统一 28px，
    // 以前这里是 S(32)、中栏是 S(30)，两栏的行距肉眼可见地不齐。
    const int rh = RowHeight();
    const int idx = (y - railList_.top) / rh;
    if (idx < 0 || idx >= static_cast<int>(groups_.size())) return -1;
    return idx;
}

int HotkeyManager::HitRow(int x, int y) const {
    const POINT pt{x, y};
    if (!PtInRect(&body_, pt)) return -1;
    const int rh = RowHeight();
    const int vi = (y - body_.top + scroll_) / rh;
    if (vi < 0 || vi >= static_cast<int>(view_.size())) return -1;
    return vi;
}

int HotkeyManager::HitDetail(int x, int y) const {
    const POINT pt{x, y};
    if (PtInRect(&detail_.edit, pt)) return 1;
    if (PtInRect(&detail_.clear, pt)) return 2;
    if (PtInRect(&detail_.reset, pt)) return 3;
    if (PtInRect(&detail_.resetAll, pt)) return 4;
    return 0;
}

void HotkeyManager::ScrollBy(int delta) {
    if (!theme_) return;
    const int step = RowHeight() * (delta > 0 ? -1 : 1) * 3;
    const int before = scroll_;
    scroll_ = std::max(0, std::min(scrollMax_, scroll_ + step));
    if (scroll_ != before) {
        LayoutRows();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void HotkeyManager::EnsureVisible(int viewIndex) {
    if (!theme_ || viewIndex < 0) return;
    const int rh = RowHeight();
    const int top = viewIndex * rh;
    const int viewH = body_.bottom - body_.top;
    if (top < scroll_) {
        scroll_ = top;
    } else if (top + rh > scroll_ + viewH) {
        scroll_ = top + rh - viewH;
    }
    scroll_ = std::max(0, std::min(scrollMax_, scroll_));
    LayoutRows();
}
