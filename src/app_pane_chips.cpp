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
#include "pane_ui.h"    // 本文件实现的声明

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

}  // namespace paneui
