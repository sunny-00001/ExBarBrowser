// 运行时键位表自测：确认能读 data/keymap.json（70 热键 + 38 序列）。
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -Isrc tests/keymap_test.cpp src/keymap.cpp src/json.cpp \
//     src/text_file.cpp -o /tmp/keymap_test.exe -static-libstdc++ -static-libgcc -lshell32
// 运行：从 tests/ 里跑（默认读 ../data/keymap.json），或把路径作为第一个参数传进来。
#include <cstdio>

#include "keymap.h"

int main(int argc, char** argv) {
    // 二进制没有宽字符命令行；用窄 argv 直接转（路径只含 ASCII）。
    std::wstring path = L"../data/keymap.json";
    if (argc > 1) path.assign(argv[1], argv[1] + strlen(argv[1]));
    Keymap km;
    bool ok = LoadKeymap(path, &km);
    std::printf("load=%d hotkeys=%zu sequences=%zu\n", ok ? 1 : 0, km.hotkeys.size(),
                km.sequences.size());

    int shown = 0;
    for (const auto& h : km.hotkeys) {
        if (h.key.empty()) continue;
        std::printf("  key  %-22ls %-16ls %ls\n", h.setting.c_str(), h.key.c_str(),
                    h.command.c_str());
        if (++shown >= 5) break;
    }
    shown = 0;
    for (const auto& s : km.sequences) {
        std::printf("  seq  %-8ls -> %ls\n", s.seq.c_str(), s.command.c_str());
        if (++shown >= 5) break;
    }

    if (!ok) return 2;
    if (km.hotkeys.size() != 70 || km.sequences.size() != 38) return 3;
    return 0;
}
