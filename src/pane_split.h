// 双栏分割的几何与百分比换算（纯函数：不碰窗口、不读配置，可直接单测）。
//
// 【为什么单独成文件】"分割线在哪、两栏各占多少"是同一个问题的两个方向：
//   · 配置/命令给的是**百分比**（paneSplit，20..80）—— 这是落盘与 AI 读回的口径；
//   · 拖动时鼠标给的是**像素** —— 1% 在常见的 880px 窗格区上是 8.8px，
//     每移动一次就四舍五入到百分比，分割线会一跳一跳地跟手。
// 所以像素 ↔ 百分比必须有一份**双向**换算，而且两栏最小边长、分割条缝宽都要夹进去。
//
// 这段夹取以前在 app_layout.cpp 的左右/上下两个分支里各写一遍；拖动引入后
// 需要第三个调用点（拖动中每次移动都要夹一次），复制第三份必然漂：
// "左右拖得动、上下拖到边界就出负边长"就是这么来的。
#pragma once

namespace panesplit {

// 每栏最小边长（物理像素）。空间不够时退回对半分，不再夹 —— 否则会算出负边长。
constexpr int kMinPane = 80;
// 百分比上下限：与 Settings::paneSplit、view.pane.split 的 schema 同口径。
// 界面能拖到的范围**必须**等于这里 —— 界面给 90%、落盘只收 80%，
// 松手那一刻分割线会往回跳一大截。
constexpr int kMinPercent = 20;
constexpr int kMaxPercent = 80;

// 纯矩形（不引 windows.h，保持可单测）。
struct Rc {
    int left = 0, top = 0, right = 0, bottom = 0;
};

struct Geometry {
    Rc pane0{};      // 主窗格（槽 0）：左右并排时在左，上下堆叠时在上
    Rc pane1{};      // 第二窗格（槽 1）
    Rc splitter{};   // 中间分割条的热区（= 两栏之间的缝）
    int center = 0;  // 分割线中心（沿主轴，绝对坐标）
};

// 分割线中心的合法区间（**局部**坐标，0 = 窗格区起点）：两栏都不小于 kMinPane，
// 且主栏占比落在 [kMinPercent, kMaxPercent]。
//
// 返回 false = 这个尺寸下两条约束没法同时满足（极窄窗口），此时 *loOut / *hiOut
// 不可用，调用方应对半分而不是夹取 —— 那正是"极窄窗口下算出负边长"的成因。
//
// 【为什么要把区间单独暴露出来】分割条现在有三种（双栏 / 导航 / AI 面板），
// 它们共用一个"按像素拖动、夹到区间"的实现。区间若留在 ClampCenter 内部，
// 拖动路径就得再抄一遍两条约束 —— 抄漏一条就是"拖得到、松手跳回去"。
bool Bounds(int avail, int gap, int* loOut, int* hiOut);

// 把分割线中心夹到合法区间：两栏都不小于 kMinPane，且主栏占比落在
// [kMinPercent, kMaxPercent]。avail 是主轴可用长度，center 是**局部**坐标
// （0 = 窗格区起点）。空间不足以放下两栏时返回 avail/2（与旧行为一致）。
int ClampCenter(int avail, int gap, int center);

// 百分比 -> 分割线中心（局部坐标，已夹取）。
// 语义是"主栏占比"：主栏长度 = 中心 - gap/2，所以中心要加上 gap/2 的偏移。
int CenterFromPercent(int avail, int gap, int percent);

// 分割线中心（局部坐标）-> 百分比（四舍五入，已夹到上下限）。
int PercentFromCenter(int avail, int gap, int center);

// 算两栏与分割条的矩形。inner 是窗格区（父窗口客户区坐标），
// center 是分割线中心的**绝对**坐标；alongX=true 表示分割线沿 x 方向
// （左右并排），false 表示沿 y 方向（上下堆叠）。
Geometry Compute(const Rc& inner, int gap, int center, bool alongX);

}  // namespace panesplit