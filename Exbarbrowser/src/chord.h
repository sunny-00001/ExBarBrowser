// 组合键：解析 / 格式化 / 与键盘消息比对 / 系统保留键。
//
// 文本口径与 mod 的 keymap.json 一致：
//   "Ctrl+Shift+T"  "Alt+←"  "F5"  "Shift+Left"  "Ctrl+Shift+1"  "Ctrl+Alt+Shift+C"
// 修饰键位域：Ctrl=1 Shift=2 Alt=4 Win=8（与 mod 的 MODB_* 同值）。
#pragma once
#include <string>

struct KeyChord {
    int mods = 0;
    unsigned vk = 0;

    bool valid() const { return vk != 0; }
    bool operator==(const KeyChord& o) const { return mods == o.mods && vk == o.vk; }
};

namespace chord {

constexpr int kCtrl = 1;
constexpr int kShift = 2;
constexpr int kAlt = 4;
constexpr int kWin = 8;

bool Parse(const std::wstring& text, KeyChord* out);
std::wstring Format(const KeyChord& c);
std::wstring FormatKey(unsigned vk);

// 当前键盘状态 -> KeyChord（不读 Win 键：应用内消息拿不到，Win 组合系统先吃掉）
KeyChord FromKey(unsigned vk);

// 键名 -> 虚拟键（支持 A-Z / 0-9 / F1-F24 / 方向 / 编辑键 / 符号）
unsigned VkFromName(const std::wstring& name);

// 系统保留键：快捷键管理器把它们标为"不可用"，也不写进默认表
bool IsReserved(const KeyChord& c);

// 焦点在文本编辑控件里时，哪些键应让给编辑框（复制/粘贴/全选/光标…）
bool ShouldYieldToEdit(unsigned vk, int mods);

// 虚拟键 → 字符（**确定性 US 布局映射**，不碰输入法）。
//   shift = 是否按住 Shift（按键消息未出队时 GetKeyboardState 可能还没刷新）
//
// 【为什么不走 ToUnicodeEx】中文输入法在中文态下会把**字母**吃进拼音组合：
// ToUnicodeEx 直接返回 0（组合中），而且会把**桌面级**输入法状态卡死 ——
// 之后所有进程都翻译不出字母（实测：q/u/e/r/y 全丢，e 还冒出 "5"）。
//
// 【为什么不走 WM_CHAR】WM_CHAR 是"输入法转好字之后"的结果：中文态下要等
// 用户提交组合才有，英文态才有。而我们要的是**键一按下去就有一个字符**
// —— 与原版 exbar.cpp 同款：低层键盘钩子把键吞掉（输入法根本看不到），
// 字符由钩子里现译。
//
// 【口径】按 US 布局固定映射，覆盖 Shift 上档符号（! ? ~ # @ $ % ^ & * ( )
// _ + { } | } " < >）—— 正好覆盖本项目的全部模式前缀。本机输入法基布局即 US，
// 换成 AZERTY 之类布局时这一列符号会与实际键位不符（已知取舍：换来的是
// 「不受输入法状态影响」的确定性）。
wchar_t CharOfVk(unsigned vk, bool shift);

}  // namespace chord
