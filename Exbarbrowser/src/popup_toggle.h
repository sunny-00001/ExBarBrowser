// 「再点即收」的唯一实现：弹层开着时点它的锚点，收起；不重开。
//
// ## 为什么必须是「窗口存活」而不是「连点时间窗」
//
// 第一版用 400 ms 窗口判"连点"，实测无效。原因不是参数没调好，而是**判据选错了**：
//
//   · 用户的操作是"看一眼列表 → 再点同一处收起"，间隔 1~2 秒，任何毫秒级窗口都过期。
//   · 更要命的是时序。第二次点击时系统**同步**发 WM_ACTIVATE(WA_INACTIVE) 给弹层，
//     弹层 PostMessage(WM_CLOSE) **入队**；随后 WM_LBUTTONDOWN 才入队。等那一下点击
//     被派发给锚点控件时，旧弹层已经关干净、`done` 已置位 —— 任何"关闭后再靠记录猜"
//     的方案，判据都已经过期了。
//
// 换句话说：**点击锚点时，弹层是否还开着，这件事在消息层就已经是过去时了。**
// 想靠"事后记录"补，永远差一拍。
//
// ## 正确模型：让弹层自己吃掉那一下点击（业界标准）
//
// TrackPopupMenu、组合框下拉、资源管理器都用同一招 —— 弹层持有捕获，或把
// "关闭它的那一下点击"吞掉。于是锚点**根本收不到**第二次 WM_LBUTTONDOWN，
// 重入在消息层就不存在，不需要任何时间窗、任何记录、任何调用点配合。
//
// 本工程的具体做法（见 popup_list.cpp 的 PopupProc::WM_ACTIVATE）：
//
//   · 弹层活着期间，这里记着它的锚点键。
//   · 弹层因外部点击而失活时，不立刻关；先 SetCapture 到自己，
//     吞掉紧随其后的那一次 LBUTTONDOWN / LBUTTONUP，再关。
//     于是锚点收不到点击 → 不会重入 → 菜单真的"关了"，而不是"关了又开"。
//
// "再点收起"由**消息流本身**保证，与用户隔多久点、点在锚点的哪个像素都无关。
//
// ## 这个头文件现在的职责
//
// 只回答一个问题："某个锚点的弹层此刻是否开着？" 供两处使用：
//   1. popup_list.cpp —— 同一锚点在"弹层仍开着"时被再次请求 → 直接返回取消。
//   2. nav_bar_actions.cpp —— 面包屑箭头形态（开着的那一级画 ⌄，其余画 ›）。
//
// 键的约定：同一控件的同一下拉用同一个串；带索引的位置把索引编进去；不同下拉必须不同。
// key 为空 = 不存在"同一个锚点"（一次性命令面板），不参与判定。
#pragma once

#include <windows.h>   // HWND（ShouldCloseOnDeactivate 的重载用）；纯逻辑部分不依赖它

#include <string>

namespace popup {

// 登记 / 注销**当前打开的弹层**。由 popup_list.cpp 的 RunModal 独家调用
// （窗口创建成功后登记，销毁前注销）。弹层是模态的，同一时刻只会有一个。
void NoteOpened(const wchar_t* key);
void NoteClosed(const wchar_t* key);

// 锚点 key 的弹层现在是否开着。openKey 非空时回填当前打开的那一个（便于调试）。
bool IsOpen(const wchar_t* key);
bool IsOpen(const wchar_t* key, std::wstring* openKey);

// 当前打开的弹层键（无则空串）。调试 / 自测用。
std::wstring OpenKey();

// 清空（自测用；正常流程不需要）。
void Reset();

// ---------------------------------------------------------------------------
// 失活（WM_ACTIVATE WA_INACTIVE）时是否应当关闭自己
//
// ## 为什么需要一个判定，而不是"一失活就关"
//
// 这是"点了两下菜单一直在"的**真正**成因，比时间窗说更底层：
//
//   ① 用户点书签栏/标签栏 → 系统**同步**给弹层发 WM_ACTIVATE(WA_INACTIVE)
//   ② 弹层若在这里 PostMessage(WM_CLOSE)，消息就进了队列
//   ③ 紧接着才派发这次点击的 LBUTTONDOWN / LBUTTONUP
//   ④ 模态循环按队列顺序先吃 WM_CLOSE → DestroyWindow → 捕获释放
//   ⑤ 剩下的 LBUTTONUP 落到光标下的那个控件上
//   ⑥ 本工程各栏都在 **UP** 里执行动作 → 又调一次 Show* → 菜单"关了又开"
//
// 关键点：**鼠标捕获不会阻止激活切换**。弹层是 WS_POPUP 顶层窗口，
// 点主窗的任一子控件都会让主窗激活、弹层失活，哪怕捕获仍在弹层手里。
// —— 所以"持捕获就安全"这个前提不成立，必须显式区分两种情况。
//
// ## 判据
//
//   同一程序内的点击（前景窗与弹层同线程）且**持有鼠标捕获**
//       → 不关。这一组 DOWN/UP 正被我们吞着（见 popup_list.cpp 的
//         ListSubclass::WM_LBUTTONDOWN/UP），等 UP 确认点在列表外再关。
//         锚点自始至终收不到点击，重开在消息层就不存在。
//   其余（点到别的程序、Alt+Tab 切走、捕获已被抢走）
//       → 关。这类"失活"不会再有点击回到我们手上，留着就是一个关不掉的菜单。
//
// 纯函数：不碰窗口、不碰时钟，可单测（tests/popup_toggle_test.cpp）。
// ---------------------------------------------------------------------------
bool ShouldCloseOnDeactivate(bool holdsCapture, bool foregroundSameThread);

// 从窗口句柄取上面两个条件，供 popup_list.cpp 直接调用。
// list：持有捕获的列表窗口；self：弹层窗口。
bool ShouldCloseOnDeactivate(HWND self, HWND list);

}  // namespace popup
