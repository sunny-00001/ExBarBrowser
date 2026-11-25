// 「有效键位」的唯一推导实现。见 key_binding.h。
#include "key_binding.h"

#include "cmd_naming.h"
#include "commands.h"   // SplitCommandSpec / FindCommand（未实现命令不入表）

#include <cwctype>

namespace keybinding {
namespace {

// 序列键的显示口径："t b" → "T B"。
//
// 【为什么不原样显示小写】小写是配置文件里的书写形式，不是用户看到的东西 ——
// 菜单右列与组合键（"Ctrl+Shift+T"）并排，全大写才读得出"这是一串按键"。
// 多字符 token（将来若有 "esc"）保持原样，只把单字符字母大写。
std::wstring SequenceText(const std::wstring& seq) {
    std::wstring out;
    for (const std::wstring& t : SplitSequence(seq)) {
        if (!out.empty()) out.push_back(L' ');
        if (t.size() == 1 && iswalpha(static_cast<wint_t>(t[0])))
            out.push_back(static_cast<wchar_t>(towupper(t[0])));
        else
            out += t;
    }
    return out;
}

// spec → 规范名。菜单写 `view.mode:3`、键位表写 `ViewMode:3`，两边只在规范名上相遇。
std::wstring CanonName(const std::wstring& name) {
    return cmdnaming::CanonicalName(name);
}

}  // namespace

bool ActingWithoutArgs(const CommandDef& def, const std::wstring& inlineArg) {
    // 带内联参数 = 这个键自带参数，它做的就是这一行的事（"ViewMode:3" / "view.sort:1"）。
    if (!inlineArg.empty()) return false;
    return HasRequiredArgs(def);
}

std::wstring ResolveKey(const std::wstring& setting, const std::wstring& keyInTable,
                        const Overrides& overrides) {
    for (const auto& kv : overrides)
        if (kv.first == setting) return kv.second;   // 覆盖优先（含"-"哨兵）
    if (!keyInTable.empty()) return keyInTable;
    return SystemKeyFor(setting);   // 表里没写 = 用系统原生键
}

std::vector<Binding> Effective(const Keymap& km, const Overrides& overrides) {
    std::vector<Binding> out;

    auto add = [&out](const KeyChord& c, const std::wstring& spec, const std::wstring& setting,
                      const std::wstring& scope) {
        if (!c.valid()) return;
        if (chord::IsReserved(c)) return;   // 系统先吃掉的键：显示了也轮不到本程序
        for (const auto& b : out)
            if (b.chord == c) return;       // 先到先得（同一组合键后一条永远按不出来）
        out.push_back({c, spec, setting, scope});
    };

    for (const auto& hk : km.hotkeys) {
        // 覆盖优先；哨兵 = 用户点了"清除"，是**显式解绑**，不能再回落默认键。
        std::wstring key = ResolveKey(hk.setting, hk.key, overrides);
        if (key == kHotkeyDisabled) continue;
        if (key.empty()) continue;

        std::wstring name, arg;
        SplitCommandSpec(hk.command, &name, &arg);
        const CommandDef* def = FindCommand(name);
        if (!def || !def->implemented) continue;   // 未实现：按不出来，别显示

        // 【必须挡住这一类】键位表里的键按下后走的是**空参**路径
        // （InputRouter → InvokeByNameLegacy(cmd, L"")），所以对"带必填参数"的命令
        // 来说这个键**做不出那一行的效果**：
        //   · clipboard.copy 空参 → 把空串写进剪贴板（静默毁掉用户上一条复制内容）；
        //   · fs.open / fs.properties / ext.terminal.at 空参 → 直接失败；
        //   · bmk.export / bmk.import 空参 → 缺 path/mode，失败。
        // 带内联参数（"ViewMode:3"）的绑定不受影响 —— 那些键自带参数，等价于这一行。
        if (keybinding::ActingWithoutArgs(*def, arg)) continue;

        KeyChord c;
        if (!chord::Parse(key, &c)) continue;
        add(c, hk.command, hk.setting, def->scopeUi);
    }
    return out;
}

std::wstring TextFor(const std::wstring& spec, const Keymap& km,
                     const std::vector<Binding>& chords, bool allowSequences) {
    if (spec.empty()) return std::wstring();

    std::wstring name, arg;
    SplitCommandSpec(spec, &name, &arg);
    const std::wstring want = CanonName(name);

    // 【入口处一次挡住】键位（组合键与序列都一样）按下后走空参路径，所以带必填
    // 参数的命令做不出这一行的效果 —— 这种行右端必须留空。放在入口而不是各分支里，
    // 是因为"组合键挡住、序列却漏"正是这类谎最常见的漏网方式。
    if (const CommandDef* def = FindCommand(name))
        if (keybinding::ActingWithoutArgs(*def, arg)) return std::wstring();

    // 1) 组合键优先：它是"一次按完"，比两段序列更值得提示。
    for (const auto& b : chords) {
        std::wstring bn, ba;
        SplitCommandSpec(b.spec, &bn, &ba);
        if (ba != arg) continue;                  // 带参命令必须参数也相同（view.mode:3 ≠ :4）
        if (CanonName(bn) != want) continue;
        return chord::Format(b.chord);
    }

    // 2) 没有组合键时退到序列键。序列键也是"真能触发"，显示它同样是真话；
    //    但序列受 seqEnabled 开关控制，关掉时不能显示（否则是按了没反应的谎）。
    //    （空参挡已在入口做过，这里不必重复。）
    if (allowSequences) {
        for (const auto& sq : km.sequences) {
            std::wstring sn, sa;
            SplitCommandSpec(sq.command, &sn, &sa);
            if (sa != arg) continue;
            if (CanonName(sn) != want) continue;
            return SequenceText(sq.seq);
        }
    }
    return std::wstring();
}

std::wstring TextFor(const std::wstring& spec, const Keymap& km, const Overrides& overrides,
                     bool allowSequences) {
    return TextFor(spec, km, Effective(km, overrides), allowSequences);
}

// 身份 → 规格。名字从命令表反查（FindCommand 双向都认），于是调用点只写枚举。
// 查不到命令 = 该 id 不存在于目录（实现缺陷），返回空串而不是猜一个名字。
static std::wstring SpecOf(CmdId id, const std::wstring& arg) {
    const CommandDef* def = FindCommand(id);
    if (!def) return std::wstring();
    std::wstring spec = def->name;
    if (!arg.empty()) spec += L":" + arg;
    return spec;
}

std::wstring TextForCmd(CmdId id, const std::wstring& arg, const Keymap& km,
                        const std::vector<Binding>& chords, bool allowSequences) {
    return TextFor(SpecOf(id, arg), km, chords, allowSequences);
}

std::wstring TextForCmd(CmdId id, const std::wstring& arg, const Keymap& km,
                        const Overrides& overrides, bool allowSequences) {
    return TextFor(SpecOf(id, arg), km, Effective(km, overrides), allowSequences);
}

}  // namespace keybinding