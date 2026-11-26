// 双栏命令：开关、切换活动窗格、设置分栏比例、方向、互换、镜像。
//
// 这几条 view.pane.* 都 aiExposed=false 或 true 的口径：
//   · 开合/切换/比例/方向（本文件原有四条）：双栏是人的浏览姿态，AI 需要的是
//     "在哪个窗格中操作"，通过 state 的 dualPane/activePane 读回即可。
//   · swap / mirror（2026-11-26 新增）：它们改变**两栏各显示什么目录**，
//     是文件编排动作而不只是姿态 —— AI 无法用现有命令表达"把 B 目录换到
//     活动栏"，所以这两条暴露给 AI（与 fs.open.pane 同一批）。
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
    // 双栏协同导航（实现见 app_panes.cpp 的 SwapPanes/MirrorPane）：
    // 都只动导航不动槽位；导航异步落地，成败由 OnTabNav 回调链收尾。
    reg(CmdId::ViewPaneSwap, [this](const Args&) {
        SwapPanes();
        return true;
    });
    reg(CmdId::ViewPaneMirror, [this](const Args&) {
        MirrorPane();
        return true;
    });
}
