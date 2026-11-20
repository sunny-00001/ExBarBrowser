// 键位表：读 data/keymap.json（默认键的唯一真相；内容与 ExBar mod 的
// data/defaults/keymap.json 保持一致，但**是两份独立副本**，改 mod 不会自动同步过来）。
// 配置文件缺失/损坏时退回 src/keymap.gen.h 的生成默认表（由 data/keymap.json 生成）。
//
// 字段口径与 mod 完全一致：
//   hotkeys[]   { setting, command, key }   key 为空 = 该命令没有默认组合键（用系统原生键）
//   sequences[] { seq, command }            seq 形如 "g g" / "x l a"
#pragma once
#include <string>
#include <vector>

struct HotkeyBinding {
    std::wstring setting;  // 配置键，如 hotkeyNavBack（覆盖按此键存）
    std::wstring command;  // 命令名，与 commands.h 的 CommandDef::name 对齐
    std::wstring key;      // "Ctrl+Shift+T"；空 = 未绑定
};

struct SequenceBinding {
    std::wstring seq;                 // 原始文本 "g g"
    std::vector<std::wstring> tokens;  // {"g","g"}
    std::wstring command;
};

struct Keymap {
    std::vector<HotkeyBinding> hotkeys;
    std::vector<SequenceBinding> sequences;
};

// 读文件；失败返回 false 且 out 置为内置默认表
bool LoadKeymap(const std::wstring& path, Keymap* out);
const Keymap& DefaultKeymap();

// mod 的系统原生键参考表（管理器里显示"实际生效键"，不写回配置）
std::wstring SystemKeyFor(const std::wstring& setting);

// 把 "g g" 切成 {"g","g"}；统一小写（字母）
std::vector<std::wstring> SplitSequence(const std::wstring& seq);
