// 书签栏 OLE 拖放目标：接受 shell 项（含虚拟/命名空间项）拖入添加为书签。
// WM_DROPFILES 只带 CF_HDROP（且只有文件系统路径），虚拟项（此电脑/网络/回收站）
// 必须走 OLE IDropTarget + CFSTR_SHELLIDLIST。拖到文件夹按钮上时加入该文件夹内部。
#pragma once
#include <windows.h>

class BookmarkBar;

// 在书签栏窗口注册 OLE drop target（OleInitialize 已在 main 完成）。
void BmkDropAttach(HWND bar, BookmarkBar* owner);
// 注销并释放（窗口销毁前调用；幂等）。
void BmkDropDetach(HWND bar);
