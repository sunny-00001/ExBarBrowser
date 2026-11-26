// 边缘自动滚动的唯一实现（拖拽把条目拖向栏的左右边缘时持续滚动）。
//
// 【为什么抽出来】标签栏先有这套机制（UpdateAutoScroll/StopAutoScroll +
// kDragScrollTimer 定时器分支），书签栏引入横向滚动后同样需要 —— 想把书签
// 拖给视野外第 30 项，鼠标顶到右边缘也够不到。两栏各写一份必然分叉
// （起停收口、定时器周期、边缘带宽迟早各改各的），所以收成这一个组件：
//   · 状态（步进方向、最后鼠标 x）由宿主持有，本头文件只给纯逻辑与
//     "更新/停止"两个入口 —— 宿主把状态和窗口传进来，不引入第二份定时器约定。
//   · 定时器到点后宿主做的事（ScrollBy + 重算落点）在两栏不同，留在宿主侧。
//
// 【谁在用】tab_strip_* 与 bookmark_bar_*（见各自 WM_MOUSEMOVE / WM_TIMER 分支）。
// 周期共用设置 tabDragScrollMs（一个旋钮管两条栏，不新增第二份滚动周期配置）。
#pragma once

#include "common.h"

#include <windows.h>

namespace edge {

// 拖拽中每帧调用：x = 当前鼠标客户区 x；lo/hi = 自动滚动的触发带（客户区坐标，
// x < lo 向左滚、x > hi 向右滚，夹在中间 = 停）；stepPx = 每拍位移（已过 S()）；
// periodMs = 定时器周期；timerId = 宿主的定时器 id。
// step=0 时摘表；非 0 时每帧重设一次 SetTimer —— Win32 对同一个 timerId 重复
// SetTimer 只改间隔、不叠加定时器，所以这里是幂等的，不需要额外比较方向。
// 返回值：调用后 state.step 的镜像（宿主可直接据此判断"是否在滚"）。
inline int Update(int* step, HWND hwnd, UINT timerId, int x, int lo, int hi, int stepPx,
                  UINT periodMs) {
    *step = (x < lo) ? -stepPx : (x > hi) ? stepPx : 0;
    if (hwnd) {
        if (*step == 0)
            KillTimer(hwnd, timerId);
        else
            SetTimer(hwnd, timerId, periodMs, nullptr);  // 重复 SetTimer 只改间隔不叠加
    }
    return *step;
}

// 停止：步进清零 + 摘定时器。拖拽结束/捕获被夺/滚出边缘带都要调（幂等）。
inline void Stop(int* step, HWND hwnd, UINT timerId) {
    *step = 0;
    if (hwnd) KillTimer(hwnd, timerId);
}

}  // namespace edge
