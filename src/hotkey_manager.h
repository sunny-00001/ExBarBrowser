// 快捷键管理器：按范围筛选、查看冲突、改键/清除/恢复默认。
//
// 列表数据来自 InputRouter::Rows()（键位表的唯一投影），改键写回
// Config::hotkeyOverrides 并立即 Reload，所以管理器关闭后新键即刻生效。
#pragma once
#include "common.h"
#include "input_router.h"
#include "theme.h"

void ShowHotkeyManager(HWND owner, const Theme& theme, InputRouter& router);
bool HotkeyManagerVisible();
