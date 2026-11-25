// 导航树组间分隔线的**绘制**。
//
// 【为什么是"画在树自己身上"，而不是"在外面盖一层"】三条路都试过：
//
//   1) 往 NSTC 内部的 SysTreeView32 插"外来"空行树项 —— **NSTC 会崩**：
//      它把树项的 lParam 当成自己的节点指针解引用（lParam=魔数 -> 启动即 SIGSEGV；
//      改成 lParam=0 + 文本留空/空格，仍然在启动路径里挂掉，主窗口起不来）。
//
//   2) 在宿主里叠一个**分层窗口**（WS_EX_LAYERED）—— **本机根本创建不出来**：
//      CreateWindowExW 直接返回 NULL 且 GetLastError()==0；事后用
//      SetWindowLongPtr(GWL_EXSTYLE, |WS_EX_LAYERED) 也补不上（读回 LAYERED=0），
//      随后 UpdateLayeredWindow / SetLayeredWindowAttributes 一律报
//      0x57 (ERROR_INVALID_PARAMETER)。最小复现见 tools/probe_sep_layer.cpp
//      与 tools/probe_sep_layer2.cpp（结论：不带 LAYERED 立刻成功）。
//
//   3) 用 CreateWindowRgn 把一个**非分层**子窗口裁成"只有线"的形状 ——
//      形状可行，但线位要靠外部缓存，树的滚动/展开会让缓存过期
//      （树一滚就得重算区域，而重算又得回树里问行矩形）。
//
// 所以最终选：**在内部树的 WM_PAINT 里画**。
//   · 内部树已经被我们子类化（SetWindowSubclass）：DefSubclassProc 让树画完，
//     我们在**同一个 DC** 上补画那几条 1px 横线。线的位置在**每次绘制时现算**
//     （而不是缓存），所以滚动/展开/收起天然跟着走 —— 不需要任何同步钩子。
//   · 树每次重绘都会把可视区整块重画，所以旧位置的线会被同一次重绘擦掉，
//     不会留下鬼影。
//   · 画在树自己的客户区里 = 完全不挡鼠标（不是独立窗口，没有命中测试问题）。
#pragma once
#include "common.h"

#include <vector>

namespace navpane {

// 分隔线左边留的空白（像素）。explorer 实测：线跨 x=10..149，不贴左边缘。
constexpr int kSepLeftInset = 10;

// 在 dc 上把一组 y（**客户区坐标**）画成 1px 高的横线，从 x=kSepLeftInset 到 width-1。
//
// height 是客户区高度：超出范围的 y 直接跳过（滚出可视区的组界不画）。
// color 为 CLR_INVALID 时什么都不画（主题还没定色）。
void DrawSeparatorLines(HDC dc, const std::vector<int>& ys, int width, int height,
                        COLORREF color);

}  // namespace navpane
