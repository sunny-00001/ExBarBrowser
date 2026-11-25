#include "chord.h"

#include <windows.h>

#include "str.h"

#include <cwctype>
#include <vector>

namespace chord {
namespace {

// Trim / Lower 走唯一实现（str.cpp）
using str::Lower;
using str::Trim;

}  // namespace

unsigned VkFromName(const std::wstring& raw) {
    const std::wstring name = Lower(Trim(raw));
    if (name.empty()) return 0;
    if (name.size() == 1) {
        wchar_t c = name[0];
        if (c >= L'a' && c <= L'z') return static_cast<unsigned>(towupper(c));
        if (c >= L'0' && c <= L'9') return static_cast<unsigned>(c);
        switch (c) {
        case L'+': return VK_OEM_PLUS;
        case L'-': return VK_OEM_MINUS;
        case L',': return VK_OEM_COMMA;
        case L'.': return VK_OEM_PERIOD;
        case L'/': return VK_OEM_2;
        case L';': return VK_OEM_1;
        case L'\'': return VK_OEM_7;
        case L'[': return VK_OEM_4;
        case L']': return VK_OEM_6;
        case L'\\': return VK_OEM_5;
        case L'`': return VK_OEM_3;
        default: break;
        }
    }
    struct Entry {
        const wchar_t* name;
        unsigned vk;
    };
    static const Entry kKeys[] = {
        {L"backspace", VK_BACK},   {L"tab", VK_TAB},
        {L"enter", VK_RETURN},     {L"return", VK_RETURN},
        {L"esc", VK_ESCAPE},       {L"escape", VK_ESCAPE},
        {L"space", VK_SPACE},      {L"insert", VK_INSERT},
        {L"ins", VK_INSERT},       {L"delete", VK_DELETE},
        {L"del", VK_DELETE},       {L"home", VK_HOME},
        {L"end", VK_END},          {L"pgup", VK_PRIOR},
        {L"pageup", VK_PRIOR},     {L"pgdn", VK_NEXT},
        {L"pagedown", VK_NEXT},    {L"left", VK_LEFT},
        {L"←", VK_LEFT},           {L"right", VK_RIGHT},
        {L"→", VK_RIGHT},          {L"up", VK_UP},
        {L"↑", VK_UP},             {L"down", VK_DOWN},
        {L"↓", VK_DOWN},           {L"capslock", VK_CAPITAL},
        {L"numlock", VK_NUMLOCK},  {L"scrolllock", VK_SCROLL},
        {L"pause", VK_PAUSE},      {L"printscreen", VK_SNAPSHOT},
        {L"prtsc", VK_SNAPSHOT},   {L"apps", VK_APPS},
        {L"menu", VK_APPS},        {L"num0", VK_NUMPAD0},
        {L"num1", VK_NUMPAD1},     {L"num2", VK_NUMPAD2},
        {L"num3", VK_NUMPAD3},     {L"num4", VK_NUMPAD4},
        {L"num5", VK_NUMPAD5},     {L"num6", VK_NUMPAD6},
        {L"num7", VK_NUMPAD7},     {L"num8", VK_NUMPAD8},
        {L"num9", VK_NUMPAD9},     {L"num*", VK_MULTIPLY},
        {L"num+", VK_ADD},         {L"num-", VK_SUBTRACT},
        {L"num.", VK_DECIMAL},     {L"num/", VK_DIVIDE},
    };
    for (const auto& e : kKeys)
        if (name == e.name) return e.vk;

    // F1..F24
    if (name.size() >= 2 && name[0] == L'f') {
        int n = _wtoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return static_cast<unsigned>(VK_F1 + n - 1);
    }
    return 0;
}

bool Parse(const std::wstring& text, KeyChord* out) {
    if (!out) return false;
    *out = KeyChord{};
    std::wstring s = Trim(text);
    if (s.empty()) return false;

    // 按 '+' 切分；末尾的 '+' 键写作 "Ctrl++" 时会出现空段，按 '+' 处理
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'+') {
            parts.push_back(cur);
            cur.clear();
            if (i + 1 == s.size()) cur = L"+";  // "Ctrl++"
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);

    unsigned vk = 0;
    for (auto& part : parts) {
        std::wstring p = Lower(Trim(part));
        if (p.empty()) continue;
        if (p == L"ctrl" || p == L"control") {
            out->mods |= kCtrl;
        } else if (p == L"shift") {
            out->mods |= kShift;
        } else if (p == L"alt" || p == L"menu") {
            out->mods |= kAlt;
        } else if (p == L"win" || p == L"windows" || p == L"super") {
            out->mods |= kWin;
        } else {
            unsigned v = VkFromName(p);
            if (!v) return false;
            vk = v;
        }
    }
    if (!vk) return false;
    out->vk = vk;
    return true;
}

std::wstring FormatKey(unsigned vk) {
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= '0' && vk <= '9') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_BACK: return L"Backspace";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_SPACE: return L"Space";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PgUp";
    case VK_NEXT: return L"PgDn";
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    case VK_OEM_PLUS: return L"+";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_COMMA: return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_1: return L";";
    case VK_OEM_2: return L"/";
    case VK_OEM_3: return L"`";
    case VK_OEM_4: return L"[";
    case VK_OEM_5: return L"\\";
    case VK_OEM_6: return L"]";
    case VK_OEM_7: return L"'";
    default: break;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
    wchar_t buf[32];
    swprintf(buf, 32, L"VK%02X", vk);
    return buf;
}

std::wstring Format(const KeyChord& c) {
    std::wstring s;
    if (c.mods & kCtrl) s += L"Ctrl+";
    if (c.mods & kShift) s += L"Shift+";
    if (c.mods & kAlt) s += L"Alt+";
    if (c.mods & kWin) s += L"Win+";
    s += FormatKey(c.vk);
    return s;
}

KeyChord FromKey(unsigned vk) {
    KeyChord c;
    c.vk = vk;
    // AltGr = 右 Alt + 左 Ctrl：这是"输入字符"，不是快捷键修饰键。
    if ((GetKeyState(VK_RMENU) & 0x8000) && (GetKeyState(VK_LCONTROL) & 0x8000)) return c;
    if (GetKeyState(VK_CONTROL) & 0x8000) c.mods |= kCtrl;
    if (GetKeyState(VK_SHIFT) & 0x8000) c.mods |= kShift;
    if (GetKeyState(VK_MENU) & 0x8000) c.mods |= kAlt;
    return c;
}

bool IsReserved(const KeyChord& c) {
    // 只列"系统/壳层一定先吃掉"的组合；应用内注册也抢不到，标出来避免误导
    struct R {
        int mods;
        unsigned vk;
    };
    static const R kReserved[] = {
        {kAlt, VK_F4}, {kAlt, VK_TAB}, {kCtrl, VK_ESCAPE}, {kCtrl | kAlt, VK_DELETE},
        {kWin, 'L'},   {kWin, 'D'},    {kWin, 'E'},        {kWin, 'R'},
        {kWin, 'S'},   {kWin, 'A'},    {kWin, 'I'},        {kWin, 'X'},
    };
    for (const auto& r : kReserved)
        if (c.mods == r.mods && c.vk == r.vk) return true;
    // Win 开头的组合一律交系统（应用内拿不到）
    if (c.mods & kWin) return true;
    return false;
}

bool ShouldYieldToEdit(unsigned vk, int mods) {
    const bool ctrl = (mods & kCtrl) != 0;
    const bool shift = (mods & kShift) != 0;
    const bool alt = (mods & kAlt) != 0;
    if (ctrl && !shift && !alt) {
        switch (vk) {
        case 'C':
        case 'V':
        case 'X':
        case 'Z':
        case 'Y':
        case 'A':
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
            return true;
        default:
            break;
        }
    }
    if (!ctrl && !alt) {
        switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
        case VK_BACK:
        case VK_DELETE:
        case VK_INSERT:
            return true;
        default:
            break;
        }
    }
    return false;
}

wchar_t CharOfVk(unsigned vk, bool shift) {
    // US 布局的确定性映射（见 chord.h 的说明）：
    // 不用 ToUnicodeEx —— 中文输入法会把字母吃进拼音组合（返回 0），还会把
    // 桌面级输入法状态卡死；也不用 WM_CHAR —— 那是"输入法转好字之后"的结果，
    // 中文态下要等用户提交组合才有。这里按物理键直接给字符：输入法看不到
    // 这些键（宿主在按键层就吞掉），所以也不会有拼音组合。
    if (vk >= 'A' && vk <= 'Z') {
        // CapsLock 与 Shift 互相反转（与系统一致）。
        const bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        const wchar_t base = static_cast<wchar_t>(vk);
        return (shift != caps) ? base : static_cast<wchar_t>(towlower(base));
    }
    if (vk >= '0' && vk <= '9') {
        if (!shift) return static_cast<wchar_t>(vk);
        static const wchar_t kShiftDigits[] = L")!@#$%^&*(";
        return kShiftDigits[vk - '0'];
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0));
    switch (vk) {
    case VK_SPACE: return L' ';
    case VK_OEM_1: return shift ? L':' : L';';
    case VK_OEM_PLUS: return shift ? L'+' : L'=';
    case VK_OEM_COMMA: return shift ? L'<' : L',';
    case VK_OEM_MINUS: return shift ? L'_' : L'-';
    case VK_OEM_PERIOD: return shift ? L'>' : L'.';
    case VK_OEM_2: return shift ? L'?' : L'/';
    case VK_OEM_3: return shift ? L'~' : L'`';
    case VK_OEM_4: return shift ? L'{' : L'[';
    case VK_OEM_5: return shift ? L'|' : L'\\';
    case VK_OEM_6: return shift ? L'}' : L']';
    case VK_OEM_7: return shift ? L'"' : L'\'';
    case VK_DECIMAL: return L'.';
    default: return 0;
    }
}

}  // namespace chord
