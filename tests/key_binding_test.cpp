// 「有效键位」推导与菜单键位匹配的自测。
//
// 为什么这条测试重要：菜单行右端显示的快捷键必须与"按下去真能触发"的键**同源**。
// 一旦派生出第二套逻辑，就会出现"菜单写着 Ctrl+Shift+T、按下去没反应"——
// 这种错永远不会自己暴露（界面看起来完全正常），只能靠断言把它钉住。
//
// 本测试覆盖三条会被第二套逻辑搞错的判据：
//   1. 用户覆盖优先，且"-"哨兵 = 显式解绑（不能回落默认键）；
//   2. 系统保留键 / 未实现命令 / 冲突（先到先得）不进生效表；
//   3. 带参命令按"名字 + 参数"匹配（view.mode:3 不能命中 :4 的键）。
//
// 编译（在 ExBarBrowser/ 下，或直接 bash tests/run_all.sh key_binding）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
//     -D_WIN32_IE=0x0A00 -Isrc tests/key_binding_test.cpp src/key_binding.cpp \
//     src/keymap.cpp src/chord.cpp src/str.cpp src/json.cpp src/text_file.cpp \
//     src/log.cpp src/platform_win.cpp src/cmd_naming.cpp src/commands.cpp \
//     src/commands_table*.cpp ... -o /tmp/key_binding_test.exe -static-libstdc++ -static-libgcc
#include <cstdio>

#include "commands.h"      // FindCommand / HasRequiredArgs（对真表断言）
#include "key_binding.h"
#include "keymap.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 造一条热键（setting / command / key）。
HotkeyBinding Hk(const wchar_t* setting, const wchar_t* command, const wchar_t* key) {
    return HotkeyBinding{setting, command, key};
}

Keymap Makemap(std::vector<HotkeyBinding> hks) {
    Keymap km;
    km.hotkeys = std::move(hks);
    return km;
}

bool HasKey(const std::vector<keybinding::Binding>& v, const wchar_t* spec) {
    for (const auto& b : v)
        if (b.spec == spec) return true;
    return false;
}

}  // namespace

int main() {
    std::printf("有效键位推导\n");

    // ---- 1. 覆盖优先 / 哨兵解绑 / 系统原生键回落 ----
    {
        Keymap km = Makemap({Hk(L"hotkeyTabClose", L"tab.close", L"Ctrl+W"),
                             Hk(L"hotkeyNavBack", L"nav.back", L"")});
        // nav.back 在表里是空键，系统原生键表给 Alt+←
        const auto eff = keybinding::Effective(km, {});
        Check(HasKey(eff, L"tab.close"), "表内键生效");
        Check(HasKey(eff, L"nav.back"), "空键回落到系统原生键（Alt+←）");
        const std::wstring backKey =
            keybinding::TextFor(L"nav.back", km, eff, /*allowSequences=*/true);
        Check(backKey == L"Alt+←", "显示的键就是系统原生键");

        // 覆盖成别的键
        const std::vector<std::pair<std::wstring, std::wstring>> ov{
            {L"hotkeyTabClose", L"Ctrl+Shift+Q"}};
        Check(keybinding::TextFor(L"tab.close", km, ov, true) == L"Ctrl+Shift+Q",
              "覆盖优先于表内键");

        // 哨兵 = 显式解绑，不能回落
        const std::vector<std::pair<std::wstring, std::wstring>> clear{
            {L"hotkeyTabClose", L"-"}};
        Check(keybinding::TextFor(L"tab.close", km, clear, true).empty(),
              "哨兵\"-\" = 显式解绑，不回落默认键");
        Check(keybinding::ResolveKey(L"hotkeyTabClose", L"Ctrl+W", clear) == L"-",
              "ResolveKey 把哨兵原样交出来（管理器据此显示\"未绑定\"）");
    }

    // ---- 2. 系统保留键 / 未实现命令 / 先到先得 ----
    {
        Keymap km = Makemap({Hk(L"a", L"tab.close", L"Alt+F4"),      // 系统保留
                             Hk(L"b", L"tab.new", L"Ctrl+Alt+Shift+F9"),
                             Hk(L"c", L"tab.clone", L"Ctrl+Alt+Shift+F9"),  // 撞上面那条
                             Hk(L"d", L"NoSuchCommand", L"Ctrl+Alt+F10"),   // 名字查不到
                             Hk(L"e", L"tab.reopen", L"Win+E")});           // Win 组合
        const auto eff = keybinding::Effective(km, {});
        Check(!HasKey(eff, L"tab.close"), "系统保留键（Alt+F4）不入生效表");
        Check(HasKey(eff, L"tab.new"), "先到先得：先出现的留下");
        Check(!HasKey(eff, L"tab.clone"), "先到先得：撞键的后一条被丢弃");
        Check(!HasKey(eff, L"NoSuchCommand"), "命令名查不到 → 不入表（显示了也执行不了）");
        Check(!HasKey(eff, L"tab.reopen"), "Win 组合一律交系统，不入表");
    }

    // ---- 3. 带参命令：名字 + 参数都要对上 ----
    {
        Keymap km = Makemap({Hk(L"v3", L"ViewMode:3", L"Ctrl+Shift+4"),
                             Hk(L"v5", L"ViewMode:5", L"Ctrl+Shift+6")});
        const auto eff = keybinding::Effective(km, {});
        // 菜单行写的是规范点分名 + 参数；键位表里是 legacy 名 —— 必须能对上
        Check(keybinding::TextFor(L"view.mode:3", km, eff, true) == L"Ctrl+Shift+4",
              "view.mode:3 命中 Ctrl+Shift+4（规范名 ↔ legacy 名归一）");
        Check(keybinding::TextFor(L"view.mode:5", km, eff, true) == L"Ctrl+Shift+6",
              "同名不同参数各自命中自己那一键");
        Check(keybinding::TextFor(L"view.mode:0", km, eff, true).empty(),
              "没有绑定的档位（:0）返回空 —— 菜单那行右端留空");
        Check(keybinding::TextFor(L"view.mode", km, eff, true).empty(),
              "不带参数的 view.mode 不命中任何带参绑定（参数是判据的一部分）");
    }

    // ---- 3b. 标签色板：8 档各自按内联参数匹配，空参仍被挡 ----
    {
        Keymap km = Makemap({Hk(L"c3", L"TabSetColor:3", L"Ctrl+Shift+Alt+F12")});
        const auto eff = keybinding::Effective(km, {});
        Check(keybinding::TextFor(L"tab.color:3", km, eff, true) == L"Ctrl+Shift+Alt+F12",
              "tab.color:3 命中自己的键（规范名 ↔ legacy 名归一）");
        Check(keybinding::TextForCmd(CmdId::TabSetColor, L"3", km, eff, true) ==
                  L"Ctrl+Shift+Alt+F12",
              "按 CmdId + 档位参数也命中同一条（菜单走这条路）");
        Check(keybinding::TextFor(L"tab.color:4", km, eff, true).empty(),
              "同一命令的其它档位不误命中");
        Check(keybinding::TextFor(L"tab.color", km, eff, true).empty(),
              "空参 tab.color 仍被必填参数挡住（按下去不知道改哪一档）");
    }

    // ---- 4. 序列键回退 + seqEnabled ----
    {
        Keymap km = Makemap({Hk(L"t", L"tab.new", L"")});   // 无组合键
        SequenceBinding sq;
        sq.seq = L"t n";
        sq.tokens = SplitSequence(sq.seq);
        sq.command = L"TabNew";
        km.sequences.push_back(sq);

        // 组合键为空、也没有系统原生键 → 退回序列键
        const auto eff = keybinding::Effective(km, {});
        Check(keybinding::TextFor(L"tab.new", km, eff, true) == L"T N",
              "没有组合键时退回序列键，且显示成大写（菜单右列与 Ctrl+X 并排）");
        Check(keybinding::TextFor(L"tab.new", km, eff, false).empty(),
              "seqEnabled 关掉时不显示序列（否则是按了没反应的谎）");
    }

    // ---- 4b. 带必填参数的命令：键/序列一律不许显示 ----
    //
    // 【这条是本功能最重要的回归】键位按下走的是**空参**路径
    // （InvokeByNameLegacy(cmd, L"")）。对带 required 的命令来说，那个键做的事
    // 不是菜单那一行的事 —— 最坏的一例 clipboard.copy 会把**空串**写进剪贴板，
    // 静默毁掉用户上一条复制内容。所以这类行右端必须留空。
    //
    // 用真实命令表（clipboard.copy 有 required:["text"]、tab.close 无 required），
    // 不造假 CommandDef —— 判据必须对**真表**成立。
    {
        const CommandDef* copy = FindCommand(L"clipboard.copy");
        const CommandDef* close = FindCommand(L"tab.close");
        Check(copy != nullptr && close != nullptr, "命令表里能找到用于对照的命令");
        if (copy && close) {
            Check(HasRequiredArgs(*copy), "clipboard.copy 被判定为「有必填参数」");
            Check(!HasRequiredArgs(*close), "tab.close 被判定为「无必填参数」");
            Check(keybinding::ActingWithoutArgs(*copy, L""),
                  "空参绑定 clipboard.copy → 判定为「做不出这一行」");
            Check(!keybinding::ActingWithoutArgs(*close, L""),
                  "空参绑定 tab.close → 允许（键就是这一行的动作）");
            Check(!keybinding::ActingWithoutArgs(*copy, L"x"),
                  "带内联参数时不再挡（键自带参数，等价于这一行）");

            // 端到端：给 clipboard.copy 绑一个组合键和一个序列，两者都必须显示为空。
            Keymap km = Makemap({Hk(L"copyHot", L"clipboard.copy", L"Ctrl+Alt+Shift+F11")});
            SequenceBinding sq;
            sq.seq = L"p c";
            sq.tokens = SplitSequence(sq.seq);
            sq.command = L"clipboard.copy";
            km.sequences.push_back(sq);

            Check(keybinding::TextFor(L"clipboard.copy", km, keybinding::Overrides{}, true).empty(),
                  "带必填参数的命令：组合键不显示（按了会写空剪贴板）");
            Check(keybinding::TextFor(L"clipboard.copy", km, keybinding::Overrides{}, false).empty(),
                  "带必填参数的命令：序列也不显示");
        }
    }

    // ---- 5. 空规格 / 空键位表不能崩 ----
    {
        Keymap empty;
        const keybinding::Overrides noOv;
        Check(keybinding::TextFor(L"", empty, noOv, true).empty(), "空规格返回空串");
        Check(keybinding::TextFor(L"tab.close", empty, noOv, true).empty(),
              "空键位表里任何命令都没有键（不猜）");
        Check(keybinding::Effective(empty, noOv).empty(), "空键位表推不出绑定");
    }

    // ---- 6. 「清除」与「还原此项默认」是两种不同的写，不能互相冒充 ----
    //
    // 快捷键管理器有两颗按钮，看起来都是"把键弄掉"，实际写的是两样东西：
    //   清除        → 覆盖表写哨兵 "-"（显式解绑，**不**回落默认键）
    //   还原此项默认 → 覆盖表里**删掉**这一项（回落到 keymap.json / 系统原生键）
    // 如果实现里把"还原"写成"写空串"，BuildBindings 见到空键会跳过 ——
    // 那条命令反而按不出来了，与"恢复默认"正好相反。而界面上完全看不出区别：
    // 两处都只是"键位列变了一下"。所以这条判据必须钉在纯函数上。
    {
        Keymap km = Makemap({Hk(L"hotkeyTabClose", L"tab.close", L"Ctrl+W")});

        // 清除：显式解绑
        const keybinding::Overrides cleared = {{L"hotkeyTabClose", kHotkeyDisabled}};
        Check(keybinding::ResolveKey(L"hotkeyTabClose", L"Ctrl+W", cleared) == kHotkeyDisabled,
              "清除 = 覆盖表里的哨兵，解析结果就是哨兵（不回落默认键）");
        Check(!HasKey(keybinding::Effective(km, cleared), L"tab.close"),
              "清除后这条命令不进生效表（按不出来，这是「清除」的语义）");

        // 还原此项：删掉覆盖项
        const keybinding::Overrides none;
        Check(keybinding::ResolveKey(L"hotkeyTabClose", L"Ctrl+W", none) == L"Ctrl+W",
              "还原此项 = 没有覆盖项，解析回落到键位表的默认值");
        Check(HasKey(keybinding::Effective(km, none), L"tab.close"),
              "还原此项后这条命令重新进生效表（默认键又按得出来）");

        // 反过来钉住那个错：如果"还原"被写成写空串，Effective 会静默丢掉这条。
        const keybinding::Overrides emptied = {{L"hotkeyTabClose", L""}};
        Check(!HasKey(keybinding::Effective(km, emptied), L"tab.close"),
              "把「还原」写成「覆盖成空串」会丢掉这条命令 —— 所以两者不能混用");
    }

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}