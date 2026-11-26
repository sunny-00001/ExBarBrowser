// 键位表的**行投影**：把 keymap + 用户覆盖推导成快捷键管理器能直接显示的一行行。
//
// 【为什么单独成文件】这段是"数据面"，与同目录 input_router.cpp 的"输入面"
// （HandleKey / 鼠标规则 / 序列状态机）是两种语义：前者只被管理器读、不参与
// 任何按键判定；后者是热路径、每次按键都会跑。混在一起时，改个显示口径
// 要动一个每帧都在跑的翻译单元，读的人也得翻到文件末尾才发现还有另一套语义。
// 顺带把 input_router.cpp 从 526 行压回 431 行（AGENTS.md §3 的 500 行上限）。
//
// 【冲突为什么在这里算而不是在界面里】"同一组合键被绑了几次"必须与
// keybinding::Effective 的"先到先得"同口径 —— 那份推导只在输入端。管理器
// 自己再数一遍，迟早出现"界面说冲突、实际能按"或反过来。
#include "input_router.h"

#include "commands.h"
#include "key_binding.h"
#include "str.h"

#include <map>

void InputRouter::BuildRows() {
    rows_.clear();
    std::map<std::wstring, int> counts;

    // 覆盖表预先摊平成 map：逐行扫 vector 是 O(n²)（112 行 × 覆盖数），
    // 而且"有没有覆盖"与"覆盖成什么"要分别回答（哨兵 vs 键位）。
    std::map<std::wstring, std::wstring> overrides;
    for (const auto& kv : cfg_->HotkeyOverrides()) overrides[kv.first] = kv.second;

    for (const auto& hk : keymap_.hotkeys) {
        BindingRow row;
        row.setting = hk.setting;
        row.command = hk.command;
        std::wstring cmdName, cmdArg;
        SplitCommandSpec(hk.command, &cmdName, &cmdArg);
        const CommandDef* def = FindCommand(cmdName);
        row.title = def ? def->title : cmdName;
        if (def && !cmdArg.empty()) row.title += L" (" + cmdArg + L")";
        row.group = def ? def->group : L"?";
        row.scope = def ? def->scopeUi : L"全局";
        row.implemented = def && def->implemented;
        row.nativeKey = SystemKeyFor(hk.setting);
        // 生效键文本同样走唯一解析（覆盖 → 表内 → 系统原生键；"-" = 显式解绑）。
        // 这里以前是就地复制的一份同规则代码 —— 两份的结局是"管理器说绑了 X、
        // 实际生效的是 Y"。
        const std::wstring key = keybinding::ResolveKey(
            hk.setting, hk.key, cfg_->HotkeyOverrides());
        // 来源：只有覆盖表里真有这一项、且不是哨兵，才算"我改过"。
        auto ov = overrides.find(hk.setting);
        row.unbound = key == kHotkeyDisabled;
        row.custom = ov != overrides.end() && !row.unbound;
        row.key = row.unbound ? std::wstring(L"（未绑定）") : key;
        KeyChord c;
        // 哨兵 "-" 会被 chord::Parse 当成 VK_OEM_MINUS 解析成功，于是所有"未绑定"
        // 行撞在同一个 counts["-"] 上 → 管理器把 2 条以上未绑定标成"冲突"。
        if (!key.empty() && key != kHotkeyDisabled && chord::Parse(key, &c)) {
            row.reserved = chord::IsReserved(c);
            ++counts[chord::Format(c)];
        }
        rows_.push_back(std::move(row));
    }

    for (const auto& sq : keymap_.sequences) {
        BindingRow row;
        row.setting = L"sequence:" + sq.seq;
        row.command = sq.command;
        const CommandDef* def = FindCommand(sq.command);
        row.title = def ? def->title : sq.command;
        row.group = def ? def->group : L"?";
        row.scope = def ? def->scopeUi : L"全局";
        row.implemented = def && def->implemented;
        row.key = sq.seq;
        row.isSequence = true;
        rows_.push_back(std::move(row));
    }

    for (auto& r : rows_) {
        if (r.isSequence || r.key.empty()) continue;
        KeyChord c;
        if (chord::Parse(r.key, &c) && counts[chord::Format(c)] > 1) r.conflict = true;
    }

    // ---- 冲突写日志（不是只标在管理器界面上）----
    //
    // 【为什么要有这一段】BuildBindings 的 add() 是"先到先得"：同一个 chord
    // 第二次出现就被**静默丢弃**（上面那个 `if (b.chord == c) return;`）。
    // 于是冲突的真实后果是"后一条命令永远按不出来"，
    // 而用户能看到的只有快捷键管理器里那一列"冲突"字样 ——
    // 不打开那个窗口，就完全不知道自己的键位为什么没反应。
    //
    // 日志是**只读证据**（AGENTS.md §7 的验证口径）：出问题时看一眼日志就知道
    // 是不是键位撞了，而不是去猜"是不是命令没实现"。
    //
    // 【为什么放在 BuildRows 之后】这里已经把三件事都算准了：
    // 用户覆盖（Config 的 override）、系统默认键兜底（SystemKeyFor）、
    // 以及未绑定哨兵（kHotkeyDisabled）的排除 —— 与判定"冲突"用的是同一份数据，
    // 不会出现"界面说冲突、日志说没冲突"的两套口径。
    //
    // 【为什么只报一次】Reload 会在配置变化时反复调用，逐次刷屏没意义；
    // 只在第一次见到某个组合时记一条。
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        if (it->second < 2) continue;
        std::wstring who;
        for (const auto& r : rows_) {
            if (r.isSequence || r.key.empty()) continue;
            KeyChord c;
            if (!chord::Parse(r.key, &c)) continue;
            if (chord::Format(c) != it->first) continue;
            if (!who.empty()) who += L" / ";
            who += r.setting + L"→" + r.command;
        }
        LogDbg(L"[keymap] 快捷键冲突：%ls 被绑了 %d 次（先到先得，后一条永远按不出来）：%ls",
               it->first.c_str(), it->second, who.c_str());
    }
}
