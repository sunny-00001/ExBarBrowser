// 双栏的界面接线（纯函数，不持有状态）。
//
// 【为什么是自由函数而不是 App 成员】它们只做两件"把控件接上命令总线/状态"的事：
// 视图菜单的双栏族回调、菜单打勾用的双栏三态。写成 App 成员要在 app.h 里加
// 四条声明 —— 而 app.h 已在行数棘轮上（tests/baselines/file-length-baseline.json），
// 不该为"两段接线"再涨。收 NavBar& 参数即可，语义不变。
//
// 【实现放哪】app_pane_chips.cpp（与状态栏 chip 同一文件：都是"双栏在界面上的
// 落点"，改一处就能看到全部入口）。
#pragma once
#include "nav_bar.h"
#include "status_bar.h"

namespace paneui {

// 视图菜单的双栏族（开关 / 方向 / 比例预设）→ 命令总线。
// 与键位、命令面板、分割条右键菜单同一条路：前置检查、落盘、审计只有那一处。
void WireMenu(NavBar& bar);

// 视图菜单打勾用的双栏三态。
// 与档位/排序不同：布局没有 shell 侧的第二份真源，配置就是唯一真相（R19）。
void FillState(NavBar::ViewState* vs);

// 双栏分割条右键菜单（就地动作）。owner 是弹层宿主，theme 给菜单渲染。
// 每一项都转投命令总线 —— 本函数只组装菜单，不实现任何动作。
void ShowBarMenu(HWND owner, const Theme& theme, POINT screen);

// 状态栏「复制/移动到对侧」两个 chip 的文案与可用态。
// 单栏或没有选中 → 空文本（段不占位，自然消失）。
// 参数由调用方（RefreshStats）给：它已经在 App 内，能拿到对侧窗格目录与
// 选中数 —— 本函数因此不需要 App 的任何私有访问（R19 口径，不复制状态）。
void SyncChips(StatusBar& bar, const Settings& s, const std::wstring& otherPath,
               unsigned selCount);

}  // namespace paneui
