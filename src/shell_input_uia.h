// Shell 视图命中探测的 **UIA 分支**（Win11 DirectUI 视图）。
//
// 【为什么单独一个 TU】命中探测有两条互不相干的路：
//   · SysListView32（Win10 及部分视图）走 ListView_SubItemHitTest，纯本地、无 COM；
//   · DirectUI（Win11 文件资源管理器视图）没有可 hit-test 的列表控件，只能问
//     UIAutomation「屏幕上这个点是什么元素」。
// 后者的体量全在**让 UIA 在坏掉时不拖垮输入**：惰性启动的初始化重试、冷却、
// 失败计数、以及从元素祖先链反推"这是标签/列头/搜索框/地址栏/工具栏"的那套映射。
// 把它与鼠标分派放在一个文件里，读的人要先翻 200 行 COM 才能看到子类过程。
//
// 【这一组的对外契约只有两个函数】ResolveByDir 与 DetectViaUia —— 后者被
// DetectTarget 当作"ListView 路线不适用时的兜底"。其余（Uia / BStr /
// UiaCooldown / UiaFailStreak）都是这条路的内部状态，不进头文件。
#pragma once
#include "common.h"

#include <string>

namespace shellinput {
namespace uia {

// 把一个"目录 + 名字"解析成 MT_FILE / MT_FOLDER；解析不出来返回 MT_UNKNOWN。
// 走 SHCreateItemFromParsingName，失败再退到 GetFileAttributesW。
// 非文件系统目录（`::` 开头的 shell 位置、`shell:` 前缀、空目录）一律 MT_UNKNOWN ——
// 那些地方的"项"不能按路径解释，硬拼路径会得到一个不存在的名字。
int ResolveByDir(const std::wstring& dir, const std::wstring& name, bool* isFolder);

// UIA 命中：屏幕上这个点是什么。返回 mouse_rules.h 的 MouseTarget。
// 内部带冷却与失败计数：UIA 服务不可用时**快速返回 MT_UNKNOWN**而不是每次都等 RPC。
int DetectViaUia(HWND hWnd, POINT screen, const std::wstring& dir, std::wstring* name,
                 bool* isFolder);

}  // namespace uia
}  // namespace shellinput
