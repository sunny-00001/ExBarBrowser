// 导航栏的「视图」菜单：视图档位 / 排序 / 分组，以及设置面板的锚点。
//
// 【为什么从 nav_bar_actions.cpp 拆出来】那个文件里装了两类动作：
//   · 地址编辑 + Tab 补全 + 面包屑/历史/折叠/段 菜单（"导航动作"）；
//   · 视图菜单一族（档位 8 项 + 排序 4 项 + 分组 4 项 + 三个入口）。
// 后者的读者与前者的读者不同：改视图菜单的人关心 view_modes 表与 shell 属性键，
// 改地址栏补全的人关心命中判定与编辑框状态。混在一起时，任何一次改动都要读
// 另一半代码；文件也已经顶到 500 行闸门（AGENTS.md §3）。
//
// 【拆分判据是作用域，不是行数】视图菜单作用于"当前标签的视图属性"，
// 导航动作作用于"地址与历史"—— 两者入参与回传都不同（ViewFn/SortGroupFn vs
// NavFn/ListFn）。按作用域切，读的人才不用翻到文件末尾才发现还有另一套语义。
#include "nav_bar.h"

#include "menu.h"            // ui::Menu（行构建与右端快捷键列）
#include "shell_columns.h"   // 属性键显示名（父菜单小结）
#include "view_modes.h"      // 视图档位 / 排序分组维度表（唯一来源）

namespace {

// 视图档位表在 view_modes.h（唯一来源）。这里原来有一份逐字相同的副本，
// 而两份表的**下标**同时被菜单回传与配置持久化使用 —— 改一处忘另一处就是
// "菜单显示的和实际切的不一致"，且无任何报错。
inline constexpr const viewmode::ViewMode* kViews = viewmode::kModes;
constexpr int kViewCount = viewmode::kCount;

// 排序/分组维度表：父菜单的小结与二级子菜单都从这里取，避免两处文案/图标漂移。
// 下标与 Config::sortMode / ViewState::sort / ViewState::group 一一对应（0..3）。
// 标签文案在 view_modes.h（唯一来源）—— 设置对话框的下拉项也读它。
constexpr int kSortGroupCount = viewmode::kSortGroupCount;
constexpr ico::IconId kSortGlyphs[kSortGroupCount] = {
    ico::IconId::Page, ico::IconId::Calendar, ico::IconId::Tag, ico::IconId::HardDrive};
constexpr ico::IconId kGroupGlyphs[kSortGroupCount] = {
    ico::IconId::Close, ico::IconId::Tag, ico::IconId::Calendar, ico::IconId::HardDrive};

// 任意属性键的中文显示名（父菜单小结用）。查不到返回空串 —— 调用方退回档位标签。
std::wstring PropertyTitle(const std::wstring& id) {
    for (const auto& p : shellcolumns::KnownProperties())
        if (p.id == id) return p.title;
    return L"";
}

}  // namespace

// 「设置…」面板的锚点：视图按钮的左下角，与右键菜单弹在同一处。
//
// 抽成函数的原因：鼠标路径（右键菜单 900 项）与命令路径（win.preferences）
// 是两条独立入口，锚点各算一份就会在布局调整后指向不同按钮。
POINT NavBar::SettingsAnchor() const {
    RECT anchor = fieldRc_;
    for (const auto& b : buttons_)
        if (b.id == kView) anchor = b.rc;
    POINT at{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &at);
    return at;
}

void NavBar::ShowViewMenu(POINT screen) {
    (void)screen;
    // 视图菜单要回答"现在用的是哪一档、按什么排序、怎么分组"。
    // 以前完全不标当前值 —— 菜单看起来像"设置向导"，用户点完不知道有没有生效。
    // 资源管理器 / Opus / TC / XYplorer 的视图菜单都给当前项打点或打勾。
    //
    // 用"✓ "前缀打勾而不是高亮：高亮是"光标所在行"，会被误读成已选中
    // （与标签颜色菜单同一取舍，见 tab_strip_actions.cpp）。
    const ViewState vs = viewState_ ? viewState_() : ViewState{};

    ui::Menu menu;
    int initial = -1;
    for (int i = 0; i < static_cast<int>(kViewCount); ++i) {
        const int row = menu.AddChecked(kViews[i].label, vs.mode == i, i, kViews[i].glyph);
        // 必须带**档位参数**：8 档在键位表里是 ViewMode:0..7 八条，只写身份不带参
        // 会让 8 行全显示"第一个绑定的档位键"—— 那是纯粹的错。
        menu.Cmd(row, CmdId::ViewMode, std::to_wstring(i));
        if (i == vs.mode) initial = row;
    }
    // 排序 / 分组收进二级层（与 Explorer 的「排序方式 →」「分组依据 →」一致）。
    // 父层只显示当前值小结 + 右向 chevron，具体四档在子菜单里选 —— 平铺时
    // 视图档位 + 4 排序 + 4 分组共 16 行，一屏放不下还混在一起。
    // 小结里保留方向（升序/降序），这样不展开也知道当前排序方向。
    // 小结优先用从 shell 现读的属性键中文名（能表达任意列，D2）；
    // 键为空/查不到时退回 0..3 档位标签。方向总是在小结里显示。
    std::wstring sortHint = L"排序方式";
    std::wstring sortTitle = PropertyTitle(vs.sortKey);
    if (!sortTitle.empty())
        sortHint += L"：" + sortTitle;
    else if (vs.sort >= 0 && vs.sort < kSortGroupCount)
        sortHint += L"：" + std::wstring(viewmode::kSortLabels[vs.sort]);
    if (!sortTitle.empty() || (vs.sort >= 0 && vs.sort < kSortGroupCount))
        sortHint += vs.ascending ? L"（升序）" : L"（降序）";
    std::wstring groupHint = L"分组依据";
    std::wstring groupTitle = PropertyTitle(vs.groupKey);
    if (!groupTitle.empty())
        groupHint += L"：" + groupTitle;
    else if (vs.group >= 0 && vs.group < kSortGroupCount)
        groupHint += L"：" + std::wstring(viewmode::kGroupLabels[vs.group]);
    menu.Separator();
    // 这两行**故意不挂 Cmd**：它们只是"打开二级层"的入口，而 ViewSort / ViewGroup
    // 按下键是直接改排序/分组、并不打开子菜单 —— 挂上去就是"显示的键与这一行
    // 做的事不是一回事"。二级层里那四档各自挂（见 ShowSortGroupSubMenu）。
    menu.AddPath(sortHint, L"", 1100, /*folder=*/true);   // 右端 chevron = 有下一级
    menu.AddPath(groupHint, L"", 1200, /*folder=*/true);

    // 「选择详细信息…」「图标大小…」「设置…」放在最下面：它们是"其它可调项"的入口，
    // 与上面三组（视图/排序/分组）不是同一层级的即时动作。
    // 动作号 900/901/902 与 0..7 / 1xx / 2xx 分开，免得视图档位扩到 9 档时撞号。
    menu.Separator();
    menu.Add(L"选择详细信息…", 901, ico::IconId::DetailsView).Cmd(CmdId::ViewColumnsChoose);
    menu.Add(L"图标大小…", 902, ico::IconId::GridView).Cmd(CmdId::ViewZoomChoose);
    menu.Add(L"设置…", 900, ico::IconId::Settings).Cmd(CmdId::Preferences);

    const POINT at = SettingsAnchor();
    // 预选当前档位：键盘用户按 ↓ 从这里继续，鼠标用户一眼看到自己在哪一档。
    ui::MenuOptions opt;
    opt.toggleKey = L"nav.view";
    opt.initialRow = initial;
    const int data = menu.Show(hwnd_, *theme_, at, opt);
    if (data == 1100 || data == 1200) {
        // 二级层：从父菜单右上角落下，避免盖住父菜单当前行。
        POINT sub{screen.x + 8, screen.y + 8};
        ShowSortGroupSubMenu(data == 1100 ? 0 : 1, sub);
    } else if (data == 901) {
        if (onColumns_) onColumns_(at);
    } else if (data == 902) {
        if (onZoom_) onZoom_(at);
    } else if (data == 900) {
        // 面板自己会按同一锚点重新打开，这里只把位置交出去。
        if (onSettings_) onSettings_(at);
    } else if (data >= 0 && data < static_cast<int>(kViewCount) && onView_) {
        onView_(kViews[data].fvm, kViews[data].icon);
    }
}

void NavBar::ShowSortGroupSubMenu(int kind, POINT screen) {
    const ViewState vs = viewState_ ? viewState_() : ViewState{};
    const wchar_t* const* labels = kind == 0 ? viewmode::kSortLabels : viewmode::kGroupLabels;
    const ico::IconId* glyphs = kind == 0 ? kSortGlyphs : kGroupGlyphs;
    const int current = kind == 0 ? vs.sort : vs.group;

    ui::Menu menu;
    int initial = -1;
    for (int i = 0; i < kSortGroupCount; ++i) {
        const int row = menu.AddChecked(labels[i], current == i, i, glyphs[i]);
        // 带**档位参数**：四档在键位表里是 view.sort:0..3 / view.group:0..3 各一条。
        // 身份 + 参数一起匹配，于是四行各自对上自己那一键（只写身份会让四行
        // 全显示同一个键）。键位表里写的是 legacy 名（ViewSort），由
        // FindCommand(id) 反查 → 规范化后比对，两边不需要认识对方的书写形式。
        menu.Cmd(row, kind == 0 ? CmdId::ViewSort : CmdId::ViewGroup,
                 std::to_wstring(i));
        if (i == current) initial = row;
    }
    // 排序方向：二级层里放一条可点的切换项，而不是只读标注 ——
    // 资源管理器在「排序方式」子菜单里也能直接改方向。arg 用 100 表示"翻转方向"，
    // 与 0..3 的档位分开；App 侧据此调 ViewSort 的同列反向语义（ascending=-1）。
    //
    // 【这行不挂 Cmd】它执行的是"同列反向"，而 ViewSort 的按压实参是"切到某一档"。
    // 挂上会显示一个"按下去把排序改成那一档、而不是翻转方向"的键 ——
    // 方向翻转没有独立的 CommandDef，所以留空是如实的。
    if (kind == 0) {
        menu.Separator();
        // 文案是"点下去会变成什么"，不是当前态（当前方向已在父菜单小结里）。
        menu.Add(vs.ascending ? L"改为降序" : L"改为升序", 100, ico::IconId::Sort);
    }

    ui::MenuOptions opt;
    opt.toggleKey = kind == 0 ? L"nav.sort.sub" : L"nav.group.sub";
    opt.initialRow = initial;
    const int pick = menu.Show(hwnd_, *theme_, screen, opt);
    if (pick < 0 || !onSortGroup_) return;
    if (pick == 100)
        onSortGroup_(0, current >= 0 && current < kSortGroupCount ? current : 0);
    else if (pick >= 0 && pick < kSortGroupCount)
        onSortGroup_(kind, pick);
}

void NavBar::ShowViewMenuNow() {
    RECT anchor = fieldRc_;
    for (const auto& b : buttons_)
        if (b.id == kView) anchor = b.rc;
    POINT at{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &at);
    ShowViewMenu(at);
}
