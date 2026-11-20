// 命令目录自测：keymap 里出现的每条命令都必须在目录里存在。
//
// 这条断言来自一次真实事故：keymap 绑了 GoTab1..GoTab9、TabMergeAll、CopyToFolder…，
// 但目录里没有这些 id，FindCommand 返回空 → BuildBindings 直接跳过 →
// 用户按 g1 没反应、快捷键管理器里也看不到，属于"静默丢键"。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -Isrc tests/commands_test.cpp src/commands.cpp src/keymap.cpp \
//     src/json.cpp src/text_file.cpp -o /tmp/commands_test.exe \
//     -static-libstdc++ -static-libgcc -lole32 -luuid -lshell32 -ladvapi32
#include <cstdio>
#include <string>

#include "commands.h"
#include "keymap.h"

int main() {
    Keymap km;
    const bool loaded = LoadKeymap(L"data/keymap.json", &km);
    std::printf("keymap load=%d hotkeys=%zu sequences=%zu\n", loaded ? 1 : 0, km.hotkeys.size(),
                km.sequences.size());

    int missing = 0;
    auto check = [&missing](const std::wstring& spec, const wchar_t* kind) {
        std::wstring name, arg;
        SplitCommandSpec(spec, &name, &arg);
        if (!FindCommand(name)) {
            std::printf("  MISSING  %ls  %ls\n", kind, spec.c_str());
            ++missing;
        }
    };
    for (const auto& hk : km.hotkeys) check(hk.command, L"hotkey");
    for (const auto& sq : km.sequences) check(sq.command, L"sequence");

    // 内联参数必须被拆开（8 档视图模式的根因）
    std::wstring n, a;
    SplitCommandSpec(L"ViewMode:3", &n, &a);
    const bool splitOk = (n == L"ViewMode" && a == L"3");
    if (!splitOk) std::printf("  SplitCommandSpec 失败：%ls / %ls\n", n.c_str(), a.c_str());

    int implemented = 0, total = 0;
    for (const auto& d : CommandTable()) {
        ++total;
        if (d.implemented) ++implemented;
    }
    std::printf("table total=%d implemented=%d missingFromKeymap=%d split=%d\n", total, implemented,
                missing, splitOk ? 1 : 0);

    if (!loaded) return 2;
    if (missing) return 3;
    if (!splitOk) return 4;
    return 0;
}
