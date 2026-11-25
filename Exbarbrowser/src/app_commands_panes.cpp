// 双栏命令：开关、切换活动窗格、设置分栏比例。
//
// 这三条都 aiExposed=false：双栏是人的浏览姿态，AI 需要的是"在哪个窗格中操作"，
// 通过 state 的 dualPane/activePane 读回即可，不必让机器反复开合窗格。
#include "app.h"

#include "app_commands.h"

void App::RegisterViewPaneCommands() {
    auto reg = cmd_reg::Register;
    reg(CmdId::ViewToggleDualPane, [this](const Args&) {
        SetDualPane(!Config::Get().s().dualPane);
        return true;
    });
    reg(CmdId::ViewNextPane, [this](const Args&) {
        NextPane();
        return true;
    });
    reg(CmdId::ViewPaneSplitSet, [this](const Args& args) {
        SetPaneSplit(args.Int(L"percent", 50));
        return true;
    });
    reg(CmdId::ViewTogglePaneVertical, [this](const Args&) {
        SetPaneVertical(!Config::Get().s().paneVertical);
        return true;
    });
}
