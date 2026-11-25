#pragma once
#include "common.h"

// shell 宿主链上的窗口类查找。由 shell_view_helpers.cpp 提供唯一实现；
// shell_view.cpp（焦点落点）与 shell_view_theme.cpp（DirectUI 主题边界）共用。
bool ClassIsName(HWND h, const wchar_t* name);
HWND FindChildByClass(HWND parent, const wchar_t* name);
