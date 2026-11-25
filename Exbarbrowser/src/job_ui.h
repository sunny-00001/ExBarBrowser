// 长任务的进度窗口（人工）与静默等待（机器）。
//
// 【为什么需要它】长任务搬去工作线程之后，"界面不冻结"这件事就落在这里：
// UI 线程不能傻等（等了就是冻结），也不能直接返回（命令必须给结果）。
// 唯一的办法是**在 UI 线程上跑一个嵌套消息循环** —— 消息照常派发，
// 窗口照常重绘，而我们在循环里轮询作业状态。
//
// 【两条路径】
//   · WaitWithWindow —— 人工路径：弹一个带进度条与"取消"按钮的窗口。
//     用户随时能按"取消"，作业随即置取消位，数据层尽快收工。
//   · WaitQuiet      —— 机器路径：不弹窗（没人盯着屏幕），但照样泵消息，
//     于是界面不会僵死。结果照样回给调用方。
//
// 【为什么用 modal::Run 而不是自己写 GetMessage 循环】modal::Run 是本工程
// 唯一的模态消息泵（见 src/modal.h）。它内部会把模态层数 +1，
// 于是 AI 请求泵在 Depth() > 0 时**不派发** —— 这正好挡住"命令 handler
// 重入地跑在另一个 handler 的栈上"（P0 修复要防的那一类）。
#pragma once
#include <string>

#include "common.h"
#include "theme.h"

namespace jobui {

// 有进度窗口地等作业跑完。
// 返回 true = 用户中途点了取消（作业已被置取消位，数据层尽快收工）。
bool WaitWithWindow(HWND owner, const Theme& theme, const std::wstring& title,
                     const std::wstring& jobId);

// 静默等作业跑完（机器路径）：不弹窗，只泵消息让界面保持响应。
// 返回 true = 作业被取消；false = 正常结束或**超时放弃**（作业仍在后台跑，
// 但本次调用不再等它 —— 调用方应当重新发起一次并给更长的预算）。
bool WaitQuiet(const std::wstring& jobId, unsigned timeoutMs);

}  // namespace jobui
