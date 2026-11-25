// 三条栏之间"可伸缩边界"的**纯几何**：宽度求解 + 每条栏的矩形。
//
// 【为什么要单独成文件】三个边界（导航|文件、文件|AI、窗格0|窗格1）现在都能拖，
// 而"拖到多宽算合法"是同一个问题的三种问法：
//   · 配置里存的是像素（navPaneWidth / aiPanelWidth / paneSplit%）；
//   · 拖动时鼠标给的是像素；
//   · 窗口太窄时必须**由外向内**让位（先收 AI，再收导航），而不是把文件区挤没。
// 这三件事都是纯算术，不碰窗口也不读配置 —— 所以放在这里可以直接单测
// （AGENTS §1.1 的第一优先验证路径），不必建窗口去量。
//
// 【为什么不写进 app_layout.cpp】那样每验证一次"窗口 700 宽时 AI 该不该收"
// 都要起进程、开窗口、截图；而这里一行断言就够。app_layout 只负责把算出来的
// 矩形搬给窗口。
#pragma once

#include "layout_limits.h"

namespace sidelayout {

// 一条竖边界。坐标都在**主窗口客户区**里，y 段由调用方按 viewTop/viewH 给
// （见 app_layout.cpp 的 sEdge/tEdge）。
//
// 【为什么没有"缝"字段】侧栏边界不像双栏那样留空档：抓取条**压在交界上**
// （比可见的 1px 线宽、两侧各压住一点），两侧内容一点不让。
// 好处是 navPaneWidth=220 仍然意味着"文件区从 x=220 开始"—— 语义与拖动引入前
// 完全一致，落盘的旧配置不需要迁移，也不会因为多了个抓手而"文件区变窄了"。
// 双栏之间的缝是另一回事（那边本来就有 6px 缝，见 pane_split.h）。
struct Edge {
    int center = 0;    // 边界中心（客户区坐标），也是拖动落点口径
    int barLo = 0;     // 抓取窗口 [barLo, barHi)
    int barHi = 0;
};

struct Layout {
    int navW = 0;      // 导航窗格实际宽度（0 = 没显示/放不下）
    int aiW = 0;       // AI 面板实际宽度（0 = 没显示/放不下）
    int fileLo = 0;    // 文件区（含双栏）左边界
    int fileHi = 0;    // 文件区右边界
    Edge nav;          // navW > 0 时才有意义
    Edge ai;           // aiW > 0 时才有意义
};

// 宽度求解：只有宽度，不产生矩形（便于单测逐条断言）。Solve 内部调它。
//
// navWant / aiWant 是**配置里的期望值**（像素；0 = 用户关了这条栏）。
// 输出 0 表示"这条栏最终不显示"。让位顺序见 Solve 的说明。
void SolveWidths(int totalW, int navWant, int aiWant, int* navOut, int* aiOut);

// 在 SolveWidths 之上再算两条边界的抓取条矩形（barW = 可抓宽度，见 Theme::splitterHit）。
Layout Solve(int totalW, int navWant, int aiWant, int barW);

}  // namespace sidelayout