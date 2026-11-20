// 命名空间别名自测（第 6 步判据）。
//
// 核心断言：**旧名与点分名必须解析到同一条命令**。
// 只要这条成立，命名空间化就不改变任何既有行为（keymap / 序列 / 鼠标规则
// 里写的仍然是旧名），同时外部调用方可以开始用 nav.back 这类稳定名字。
//
// 第二个断言：点分名格式合规且唯一 —— 否则会出现"两个命令抢一个名字"，
// 那种 bug 表现为"调用 A 却执行了 B"，极难排查。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/naming_test.cpp src/commands.cpp src/cmd_naming.cpp src/json.cpp \
//     src/text_file.cpp /tmp/config_stub.cpp \
//     -o /tmp/naming_test.exe -lole32 -luuid -lshell32 -ladvapi32
#include "common.h"

#include <cstdio>
#include <map>
#include <string>

#include "cmd_naming.h"
#include "commands.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool SameDef(const wchar_t* a, const wchar_t* b, std::wstring* detail = nullptr) {
    const CommandDef* da = FindCommand(a);
    const CommandDef* db = FindCommand(b);
    if (!da || !db) {
        if (detail) *detail = std::wstring(L"有一方找不到：") + a + L" / " + b;
        return false;
    }
    if (da->id != db->id) {
        if (detail) *detail = std::wstring(L"id 不同：") + da->name + L" vs " + db->name;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    std::printf("旧名仍可用（零破坏）\n");
    {
        Check(FindCommand(L"NavBack") != nullptr, "NavBack 找得到");
        Check(FindCommand(L"CmdDelete") != nullptr, "CmdDelete 找得到");
        Check(FindCommand(L"FsRename") != nullptr, "FsRename 找得到");
    }

    std::printf("\n点分名可用\n");
    {
        Check(FindCommand(L"nav.back") != nullptr, "nav.back 找得到");
        Check(FindCommand(L"fs.rename") != nullptr, "fs.rename 找得到");
        Check(FindCommand(L"tab.close.others") != nullptr, "三段名也认");
        Check(FindCommand(L"no.such.thing") == nullptr, "未知点分名返回空");
        // 大小写不敏感是**有意**的：与 Windows 路径/命令的习惯一致，
        // 让人手输 "Navback" 也能用。所以这里断言"能命中"，而不是"拒绝"。
        Check(FindCommand(L"navback") != nullptr, "大小写不敏感（有意为之）");
        Check(FindCommand(L"NAV.BACK") != nullptr, "点分名也大小写不敏感");
    }

    std::printf("\n双向解析到同一条命令（关键判据）\n");
    {
        struct Pair { const wchar_t* legacy; const wchar_t* dotted; };
        const Pair pairs[] = {
            {L"NavBack", L"nav.back"},
            {L"NavRefresh", L"nav.refresh"},
            {L"TabClose", L"tab.close"},
            {L"CmdDelete", L"file.delete"},
            {L"FsDelete", L"fs.delete"},
            {L"OpListDir", L"tool.list"},
            {L"ViewMode", L"view.mode"},
            {L"BmkToggleBar", L"bmk.toggle"},
        };
        for (const auto& p : pairs) {
            std::wstring d;
            const bool ok = SameDef(p.legacy, p.dotted, &d);
            if (ok) {
                Check(true, "一致");
            } else {
                std::printf("  FAIL  %ls\n", d.c_str());
                ++g_fail;
            }
        }
    }

    std::printf("\nCanonicalName / AliasOf / LegacyOf\n");
    {
        Check(cmdnaming::CanonicalName(L"NavBack") == L"nav.back", "旧名 -> 点分名");
        Check(cmdnaming::CanonicalName(L"nav.back") == L"nav.back", "点分名原样返回");
        Check(cmdnaming::AliasOf(L"NavBack") == L"nav.back", "AliasOf 命中");
        Check(cmdnaming::LegacyOf(L"nav.back") == L"NavBack", "LegacyOf 命中");
        Check(cmdnaming::AliasOf(L"NoSuchCmd").empty(), "AlasOf 未命中返回空");
        Check(cmdnaming::LegacyOf(L"no.such").empty(), "LegacyOf 未命中返回空");
        // 未列在表里的名字：CanonicalName 原样返回（不编造）
        Check(cmdnaming::CanonicalName(L"SomeFutureCmd") == L"SomeFutureCmd",
              "无别名时原样返回");
    }

    std::printf("\nLooksDotted\n");
    {
        Check(cmdnaming::LooksDotted(L"nav.back"), "含点 -> true");
        Check(!cmdnaming::LooksDotted(L"NavBack"), "无点 -> false");
        Check(cmdnaming::LooksDotted(L"a.b.c"), "多段 -> true");
    }

    std::printf("\n全表一致性：每条命令都能按两种名字找到\n");
    {
        int checked = 0, bad = 0;
        std::map<std::wstring, std::wstring> seen;
        for (const auto& d : CommandTable()) {
            const std::wstring dotted = cmdnaming::CanonicalName(d.name);
            if (dotted == d.name) {
                // d.name 已经是点分名 = 该命令已迁移到"点分名作主名"的目标形态
                // （docs/NAMING.md §4.1）。这不是"缺别名"，而是迁移完成的标志。
                // 仍要求它可被解析，且旧名（若有）也能解析回同一 id。
                if (!cmdnaming::LooksDotted(d.name)) {
                    ++bad;
                    std::printf("      无别名且主名非点分：%ls\n", d.name);
                    continue;
                }
                if (!FindCommand(d.name)) {
                    ++bad;
                    std::printf("      点分主名解析不到自己：%ls\n", d.name);
                }
                const std::wstring legacy = cmdnaming::LegacyOf(d.name);
                if (!legacy.empty()) {
                    const CommandDef* byLegacy = FindCommand(legacy);
                    if (!byLegacy || byLegacy->id != d.id) {
                        ++bad;
                        std::printf("      旧名 %ls 解析不到 %ls\n", legacy.c_str(), d.name);
                    }
                }
                if (seen.count(dotted)) {
                    ++bad;
                    std::printf("      重复：%ls 与 %ls 都用 %ls\n", seen[dotted].c_str(),
                                d.name, dotted.c_str());
                }
                seen[dotted] = d.name;
                ++checked;
                continue;
            }
            // 点分名唯一
            auto it = seen.find(dotted);
            if (it != seen.end() && it->second != d.name) {
                ++bad;
                std::printf("      重复：%ls 与 %ls 都用 %ls\n", it->second.c_str(),
                            d.name, dotted.c_str());
            }
            seen[dotted] = d.name;

            // 两种名字解析到同一 id
            const CommandDef* byLegacy = FindCommand(d.name);
            const CommandDef* byDotted = FindCommand(dotted);
            if (!byLegacy || !byDotted || byLegacy->id != byDotted->id) ++bad;
            ++checked;
        }
        Check(checked >= 131, "至少检查了 131 条");
        Check(bad == 0, "全部条目双向解析一致、点分名唯一");
    }

    std::printf("\n点分名格式\n");
    {
        int bad = 0;
        for (const auto& d : CommandTable()) {
            const std::wstring dotted = cmdnaming::AliasOf(d.name);
            if (dotted.empty()) continue;
            // 只允许小写字母、数字、点；且至少两段
            bool okfmt = dotted.find(L'.') != std::wstring::npos;
            for (wchar_t ch : dotted) {
                if (!((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'.')) {
                    okfmt = false;
                    break;
                }
            }
            if (!okfmt) {
                ++bad;
                std::printf("      不合规：%ls -> %ls\n", d.name, dotted.c_str());
            }
        }
        Check(bad == 0, "所有点分名只含小写字母/数字/点，且至少两段");
    }

    std::printf("\n与 group 字段的对应关系（域前缀不应凭空发明）\n");
    {
        // 域前缀到 group 的映射（个别例外在命名表里有注释说明）
        std::map<std::wstring, std::wstring> domainToGroup = {
            {L"tab", L"Tab"},       {L"go", L"Go"},         {L"nav", L"Nav"},
            {L"path", L"Path"},     {L"file", L"File"},     {L"fs", L"File"},
            {L"view", L"View"},     {L"win", L"Window"},    {L"ext", L"External"},
            {L"bmk", L"Bookmark"},  {L"session", L"Session"},
            {L"tool", L"Tool"},     {L"search", L"Search"},
        };
        int bad = 0;
        for (const auto& d : CommandTable()) {
            const std::wstring dotted = cmdnaming::AliasOf(d.name);
            if (dotted.empty()) continue;
            const std::wstring domain = dotted.substr(0, dotted.find(L'.'));
            auto it = domainToGroup.find(domain);
            if (it == domainToGroup.end()) {
                ++bad;
                std::printf("      未知域：%ls -> %ls\n", d.name, dotted.c_str());
            } else if (it->second != d.group) {
                ++bad;
                std::printf("      域与 group 不符：%ls -> %ls（group=%ls）\n",
                            d.name, dotted.c_str(), d.group);
            }
        }
        Check(bad == 0, "每个点分名的域前缀都与命令的 group 一致");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
