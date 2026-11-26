// 双栏协同导航：两窗格之间"换目录 / 跟到对侧"的两条命令。
//
// 【为什么从 app_panes.cpp 拆出来】那个文件是**双栏编排**（窗格访问、按需创建、
// 活动窗格切换、比例与回调反查），而这里是**两条协同导航命令**——
// 语义是"这一栏显示什么"，与"槽位结构"无关。混在一起时，读者找 swap/mirror
// 要在编排代码里翻。
//
// 【设计边界】两条都只动**导航**，不动槽位结构：Pane::path/历史/视图归属全部
// 保留，交换的只是"这一栏显示什么"。这与 SetPaneVertical"方向只是摆法"是同一条
// 边界 —— 槽位是持久化单位（会话/锁定/别名挂在槽上），协同导航不该碰它。
#include "app.h"

#include "config.h"

// ---- 双栏协同导航（P1，对标 TC cm_Exchange / cm_MatchSrc）----
//
// 两条都只动**导航**，不动槽位结构：Pane::path/历史/视图归属全部保留，
// 交换的只是"这一栏显示什么"。这与 SetPaneVertical"方向只是摆法"是同一条
// 设计边界 —— 槽位是持久化单位（会话/锁定/别名挂在槽上），协同导航不该碰它。

// 两窗格目录互换（view.pane.swap，TC Ctrl+U / cm_Exchange）。
// 每一栏都走自己的 NavigateInPane：目标路径写进那一栏**自己的**历史栈，
// 所以交换后两边都能各自后退回原目录（与整块交换 Pane 对象不同 ——
// 那会把历史栈也换走，"后退"就不知道该退谁了）。
void App::SwapPanes() {
    Tab* t = Active();
    if (!t) return;
    // 单栏时先开双栏（与 TransferToOtherPane / OpenPathInOtherPane 同前置语义）。
    // 开出来时两栏同目录，交换是幂等的 —— 用户看到的就是"出现了对侧"。
    if (!Config::Get().s().dualPane) SetDualPane(true);
    t = Active();
    if (!t) return;
    Pane* a = PaneAt(t, 0);
    Pane* b = PaneAt(t, 1);
    if (!a || !b) return;
    // EnsurePaneView：还没视图的一栏在导航前建出来（NavigateInPane 需要 view）。
    if (!EnsurePaneView(t, 0) || !EnsurePaneView(t, 1)) return;
    const std::wstring pa = a->path();
    const std::wstring pb = b->path();
    if (pa.empty() && pb.empty()) return;
    if (!pa.empty()) (void)NavigateInPane(b, pa, true);
    if (!pb.empty()) (void)NavigateInPane(a, pb, true);
    // 导航是异步落地的（Shell 回调才改 path），这里不主动 Sync ——
    // OnTabNav 回调链自然会刷外壳，与 go.* 命令的收尾一致。
}

// 另一窗格导航到活动窗格当前目录（view.pane.mirror，TC cm_MatchSrc 目录版）。
// 活动窗格不动；对侧若没开就先开（OpenPathInOtherPane 已实现这一整套前置，
// 直接复用 —— "单栏先开双栏 + 不动焦点 + push 进对侧历史"三点都对）。
void App::MirrorPane() {
    Tab* t = Active();
    if (!t) return;
    Pane* ap = ActivePane();
    if (!ap) return;
    const std::wstring here = ap->path();
    if (here.empty()) return;
    (void)OpenPathInOtherPane(here);
}
