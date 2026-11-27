// 键位方案预设：把「某一派用户的习惯键位」做成可一键套用的数据。
//
// 【为什么需要它】迁移成本几乎全在键位上。从 Total Commander / Opus / 资源管理器
// 过来的人，第一件事是找 F5 是不是刷新、Delete 是不是删除；让他逐条改 112 条
// 不现实，而"你自己改吧"等于告诉他"这软件不适合你"。预设把这件事变成一次点击。
//
// 【为什么预设只存差异】见 data/keymap_presets.json 的 _comment：全量预设会随
// 命令增长而过时，二十个文件各漏一条就是"套用后那条命令没键"。只写差异则
// 天然跟着默认值走 —— 而且"这一派跟默认有什么不同"本来就是方案的定义。
//
// 【为什么按命令 id 匹配而不是 setting 名】命令 id 是对外契约（命令面板、AI、
// 菜单都用它），setting 名是配置文件内部键（历史上改过一次）。预设写 id，
// 套用时反查当前 setting —— 于是预设不会因为内部改名而静默失效。
#pragma once

#include <string>
#include <vector>

namespace keypreset {

// 一个预设方案。
struct Preset {
    std::wstring id;      // 稳定标识（落盘/命令参数用它，不改名）
    std::wstring name;    // 显示名
    std::wstring desc;    // 一句话说明（"适合从 XX 迁过来的人"）
    // 命令 id → 键位文本。空串 = 显式解绑（这一派不用这个键）。
    // 只含**与默认值不同**的条目。
    std::vector<std::pair<std::wstring, std::wstring>> hotkeys;
    int matched = 0;      // 套用时：真正写进去几条（加载后由 Apply 填）
};

// 从指定 profile 根目录下的 data/keymap_presets.json 读全部方案。
// 调用方传 Config::Get().RootDir()（默认=exe 目录）—— 不要用相对 cwd，
// 从快捷方式/命令行以别的"起始目录"启动时相对路径会指错地方。
std::vector<Preset> Load(const std::wstring& path);
std::vector<Preset> LoadFromRoot(const std::wstring& root);

// 预设文件不存在时用它（只有"程序默认"一项，等于把功能降级为"还原默认"）。
std::vector<Preset> BuiltinFallback();

// 按 id 找方案。找不到返回 nullptr。
const Preset* Find(const std::vector<Preset>& all, const std::wstring& id);

}  // namespace keypreset
