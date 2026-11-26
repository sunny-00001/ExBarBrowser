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

// 读剪贴板文本。没有文本内容（剪贴板里是图片/文件，或空）时返回空串。
//
// 【为什么和写放在同一个文件】读写共用同一套"全系统共享资源"的打开重试与
// 失败语义。分成两个文件之后，重试策略迟早各写一份 —— 一份重试 5 次、
// 一份不重试，表现为"复制偶尔失败、粘贴永远失败"。
std::wstring GetClipboardText(void* owner);
