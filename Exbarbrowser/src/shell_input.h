// Shell 文件视图的输入探测层：把 UIA/ListView 命中结果翻译成 MouseTarget。
//
// ExBar mod 在 explorer 进程内用 WH_MOUSE_LL + SetWindowSubclass 覆盖文件视图；
// 这里是独立进程，窗口树完全属于自己，所以不需要全局钩子：直接在
// SHELLDLL_DefView / SysListView32 / SysTreeView32 上装子类，命中后经 MouseSink
// 汇入 InputRouter。规则引擎与命令总线仍然只有一份。
#pragma once
#include "common.h"

#include <string>

namespace shellinput {

// 在 IShellView::GetWindow 返回的根窗口上枚举并安装子类。
// dir 是当前目录，供文件/文件夹目标解析；后续导航用 UpdateDir 更新。
bool Install(HWND viewWindow, HWND container, const std::wstring& dir, MouseSink sink);
void Uninstall(HWND container);
void UpdateDir(HWND container, const std::wstring& dir);

// 命中测试：Win10 走 SysListView32 hit-test；Win11 DirectUI 走 UIA。
// 返回 mouse_rules.h 的 MouseTarget；name/isFolder 用于 file/folder 目标。
int DetectTarget(HWND hWnd, POINT screen, const std::wstring& dir, std::wstring* name = nullptr,
                 bool* isFolder = nullptr);

}  // namespace shellinput
