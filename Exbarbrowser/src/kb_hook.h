// 低层键盘钩子：把**真实**的按键事件投递给主线程。
//
// 【为什么需要它】中文输入法在系统层把字母/数字键吃成 WM_KEYDOWN(VK_PROCESSKEY)，
// 应用根本拿不到真实 vk —— 于是「按 g 弹序列候选」在任何中文输入法下都不可能发生
// （实测：宿主日志里连一行都没有，弹窗不出现）。
// 原版 exbar.cpp 的解法就是 WH_KEYBOARD_LL：钩子跑在输入法**之前**，
// 拿到真实 vk，把键吞掉（输入法看不见 → 不会弹拼音候选框），再投给 UI 线程。
//
// 【与原版的差异】原版把整个序列状态机都搬进了钩子线程；本项目只取它最必要的一层：
// 钩子只做「取真实 vk + 吞键 + 投递」，序列状态机仍在 UI 线程（InputRouter）。
// 这样钩子回调里没有任何业务逻辑，不可能因超时被系统静默摘掉
// （LowLevelHooksTimeout：低层钩子回调超时会被系统直接移除，
//   症状是「所有热键突然全失效」——原版为此专门注释过）。
#pragma once

#include <windows.h>

namespace kbhook {

// 修饰键位域（与 chord::kCtrl/kShift/kAlt 同值，见 chord.h）。
constexpr int kCtrl = 1;
constexpr int kShift = 2;
constexpr int kAlt = 4;

// 一次按键事件（由钩子线程投递到主线程）。
struct KeyEvent {
    unsigned vk = 0;      // 真实虚拟键码
    int mods = 0;         // 事件流推导的修饰键（不是 GetKeyState：异步投递时状态可能已变）
    bool repeat = false;  // 自动重复（同一次按下的第 2 个及以后的 key-down）
};

// 主线程消息循环里收到本消息时，lParam 是 KeyEvent*（堆对象，接收方负责 delete）。
constexpr UINT kKeyMsg = WM_APP + 0x2A;

// 安装钩子。投递目标线程 = targetThreadId；mainWnd = 主窗口（用于判定焦点归属）。
// 成功返回 true（失败时应用退回到 WM_KEYDOWN 路径，行为与装钩子前一致）。
bool Start(DWORD targetThreadId, HWND mainWnd);

// 额外的"文本输入区"宿主（当前是 WebView2 AI 面板）。焦点落在它或它的
// 子窗口里时，钩子必须放行按键 —— WebView2 的输入框不是原生 EDIT 类，
// 只判 EDIT 会把聊天框里的打字当成序列键前缀吞掉。传 nullptr 清除。
void SetTextEntryRoot(HWND root);

// 停钩子（卸载钩子 + 结束钩子线程）。可重复调用。
void Stop();

// 钩子是否在运行。主线程据此决定「字符键走钩子路径」还是「走 WM_KEYDOWN 路径」。
bool Active();

}  // namespace kbhook
