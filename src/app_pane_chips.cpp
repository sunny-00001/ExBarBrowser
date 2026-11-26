// 双栏在界面上的落点：状态栏 chip 的文案/可用态 + 视图菜单的接线与打勾状态。
//
// 【为什么单独成文件】这些都不是业务动作（动作在 app_panes_swap.cpp /
// app_commands_fs.cpp），而是"把双栏接到已有界面设施上"：状态栏段、视图菜单、
// 命令总线。它们改动的频率与"双栏语义"不同（界面调整多、语义少），
// 混进 app_panes.cpp 会让那个文件同时管"窗格生命周期"和"界面文案"。
//
// 【为什么是自由函数】app.h 已在行数棘轮上（tests/baselines/file-length-baseline.json），
// 这些函数只需 StatusBar& / NavBar& / App& 三样显式依赖，没有任何一条需要
// 当成员才能访问的私有状态。参数写出来，依赖关系反而更清楚。
#include "app.h"

#include "commands.h"   // CommandBus（菜单族转投）
#include "config.h"
#include "icon_glyph.h"  // 双栏族菜单行图标
#include "menu.h"        // ui::Menu：菜单组装
#include "pane_ui.h"     // 本文件实现的声明

namespace paneui {

void WireMenu(NavBar& bar) {
    // 视图菜单里的双栏族（开关 / 方向 / 比例预设）：与键位、命令面板、
    // 分割条右键菜单走同一条命令总线 —— 前置检查、落盘、审计只有那一处。
    bar.SetPaneCmdFn([](CmdId id, const std::wstring& arg) {
        if (arg.empty())
            (void)CommandBus::Get().Invoke(id, Args::Empty(), Actor::Ui());
        else
            (void)CommandBus::Get().InvokeLegacy(id, arg, Actor::Ui());
    });
}

void FillState(NavBar::ViewState* vs) {
    if (!vs) return;
    const Settings& s = Config::Get().s();
    vs->dualPane = s.dualPane;
    vs->paneVertical = s.paneVertical;
    vs->paneSplit = s.paneSplit;
}

// 两个 chip 的文案口径：
//   · 单栏 → 空（chip 消失）。不给"点了会自己开双栏"的按钮 —— 那会把
//     "开双栏"这个决定藏在一次复制里。
//   · 双栏但无选中 → 空。没东西可发，一个点不动的按钮只是噪音
//     （空文本的段不占位，见 status_bar.cpp 的排版）。
//   · 有选中 → "复制到对侧" / "移动到对侧"，tip 带目标目录与条数。
void SyncChips(StatusBar& bar, const Settings& s, const std::wstring& otherPath,
               unsigned selCount) {
    const bool usable = s.dualPane && selCount > 0;
    const std::wstring n = std::to_wstring(selCount);
    const std::wstring target =
        otherPath.empty() ? std::wstring() : (L"，目标：" + otherPath);

    std::wstring copyText, copyTip, moveText, moveTip;
    if (usable) {
        copyText = L"复制到对侧";
        copyTip = L"把当前选中的 " + n + L" 项复制到另一窗格" + target;
        moveText = L"移动到对侧";
        moveTip = L"把当前选中的 " + n + L" 项移动到另一窗格" + target + L"（执行前会确认）";
    }
    bar.SetSegmentText(Seg_CopyPane, copyText, copyTip);
    bar.SetSegmentText(Seg_MovePane, moveText, moveTip);
}

// 双栏分割条右键菜单（"就地动作"）。锚点由 SplitBar 给（鼠标屏幕坐标）。
//
// 【为什么值得有】双栏六条命令此前只有键位与命令面板两个入口：前者"记得住才
// 用得上"，后者"知道名字才用得上"。而用户想动双栏时，视线本来就在分割条上 ——
// 就地右键是最短的路径，且**零新增界面面积**（分割条本来就是真窗口，
// CS_DBLCLKS 与鼠标通路都是现成的）。
//
// 【为什么每一项都转投命令】与人机等价规则同源：前置检查、落盘、审计只有
// 命令总线那一处。菜单这里只组装，不实现动作 —— 否则键位与菜单会各有一份
// "开双栏要做什么"，迟早不一致。
void ShowBarMenu(HWND owner, const Theme& theme, POINT screen) {
    const Settings& s = Config::Get().s();
    ui::Menu menu;
    // 前两行是**勾选态**（当前姿态），不是"点一下变成什么" ——
    // 与视图菜单把当前档位打勾同一口径，避免"点了才知道现在是什么"。
    menu.AddChecked(L"双栏", s.dualPane, 1300, ico::IconId::Dual);
    menu.Cmd(1300, CmdId::ViewToggleDualPane);
    // 方向在单栏下也有意义（先切方向、再开双栏得到的就是上下两栏），故不禁用 ——
    // 与命令表里 view.pane.vertical 的说明一致。
    menu.AddChecked(L"上下堆叠", s.paneVertical, 1301, ico::IconId::PaneVertical);
    menu.Cmd(1301, CmdId::ViewTogglePaneVertical);
    menu.Separator();
    menu.Add(L"互换两栏目录", 1305, ico::IconId::SwapPane);
    menu.Cmd(1305, CmdId::ViewPaneSwap);
    menu.Add(L"对侧打开当前目录", 1306, ico::IconId::Mirror);
    menu.Cmd(1306, CmdId::ViewPaneMirror);
    menu.Separator();
    // 比例预设：拖动是主界面，这里给的是"键盘/精确归位"的短路径。
    // 三档都落在 panesplit 的 20..80 合法区间内（30/50/70），
    // 不存在"点了被夹回来"的跳变。
    menu.AddChecked(L"分栏 30%", s.paneSplit == 30, 1302, ico::IconId::Dual);
    menu.Cmd(1302, CmdId::ViewPaneSplitSet, L"30");
    menu.AddChecked(L"分栏 50%", s.paneSplit == 50, 1303, ico::IconId::Dual);
    menu.Cmd(1303, CmdId::ViewPaneSplitSet, L"50");
    menu.AddChecked(L"分栏 70%", s.paneSplit == 70, 1304, ico::IconId::Dual);
    menu.Cmd(1304, CmdId::ViewPaneSplitSet, L"70");
    // 锚点键：再按一次同一锚点 = 收起（与其它菜单一致）。
    ui::MenuOptions opt;
    opt.toggleKey = L"pane.bar";
    (void)menu.Show(owner, theme, screen, opt);
}

}  // namespace paneui
