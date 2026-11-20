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

}  // namespace chord
