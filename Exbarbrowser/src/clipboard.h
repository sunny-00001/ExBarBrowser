// 剪贴板文本写入（唯一实现）。
//
// 收编理由：App::CopyText 与 BookmarkBar 的"复制路径"分支各写了一遍
// OpenClipboard/EmptyClipboard/GlobalAlloc/SetClipboardData/CloseClipboard，
// 其中一份漏了"SetClipboardData 失败要 GlobalFree"（每次复制漏一块内存）。
//
// 失败语义：返回 false 表示剪贴板被别的进程占着或分配失败 —— 调用方可以提示，
// 不要静默当作成功。
#pragma once
#include <string>

bool SetClipboardText(void* owner, const std::wstring& text);
