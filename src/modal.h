// 模态消息泵（唯一实现）。
//
// 【为什么必须有这一个文件】模态对话框的循环以前在 prompt.cpp（两个函数各一份）、
// report.cpp、popup_list.cpp、app.cpp 里各写一遍，每份都是同样的
// `while (!done && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) { ... }` + 尾部
// `if (gm == 0) PostQuitMessage(...)`。四处重复的后果不是"代码难看"，而是
// **每新增一个对话框都要重新踩一遍同样的两个坑**：
//   1. GetMessageW 返回 -1 是错误、返回 0 是 WM_QUIT —— 写成 `while (GetMessageW(...))`
//      把 -1 当成"真"继续跑，把 0 当成"假"吞掉，宿主就永远退不出去；
//   2. 循环里对按键做的特殊处理（回车/ESC/Tab）必须 `continue`，绝不能
//      既处理又 Dispatch —— 否则同一次按键被处理两遍（回车 = 提交两次）。
// 两件事都是"写对了看不出，写错了很久以后才发现"的类型，值得收成一处。
//
// 键盘过滤用回调而不是枚举：调用方知道自己有哪些控件、什么键该拦，
// 枚举出来的形式永远少一种（书签重命名不需要 Tab、报告窗口需要 Ctrl+A）。
#pragma once
#include <windows.h>

#include <functional>

namespace modal {

// 泵消息直到 done() 为真。onKey 返回 true 表示"这条消息已处理，不要再派发"。
// 返回 false 表示循环因 WM_QUIT 结束（已代为 PostQuitMessage，调用方直接收摊即可）。
//
// 注意：done() 由窗口过程置位（WM_COMMAND/WM_CLOSE），所以**不要**在 onKey 里
// 直接 DestroyWindow —— 销毁推迟到调用方退出循环后做，避免循环里访问已销毁的窗口。
bool Run(const std::function<bool()>& done,
         const std::function<bool(MSG&)>& onKey = nullptr);

}  // namespace modal
