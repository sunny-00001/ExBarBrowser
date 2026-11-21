// 运行时键位表自测：确认能读 data/keymap.json（当前 84 热键 + 38 序列）。
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
//     -D_WIN32_IE=0x0A00 -Isrc tests/keymap_test.cpp src/keymap.cpp src/json.cpp \
//     src/text_file.cpp src/str.cpp src/log.cpp src/platform_win.cpp \
//     -o /tmp/keymap_test.exe -static-libstdc++ -static-libgcc \
//     -lshell32 -lole32 -luxtheme -ladvapi32 -ldwmapi
// 运行：从 tests/ 里跑（默认读 ../data/keymap.json），或把路径作为第一个参数传进来。
//
// 依赖为什么是这些：`LoadKeymap` → `tf::ReadText`（text_file）→ `str::FromUtf8Strict`（str）
// → `LogDbg`（log）→ `win::ThemeControl`（platform_win，要 uxtheme）。早期文档只列了前三个，
// 照抄必然链接失败 —— 记录在这里，省得下一个人重新试一遍。
#include <cstdio>

#include "keymap.h"

// 键位条数会随功能增长（新增快捷键就要 +1）。这里钉的是"目录与内置兜底表同步"，
// 不是"冻结在某个数字"。改 data/keymap.json 后同步改这里，或者跑
// `python tools/gen_keymap.py` 让生成头跟上。
//
// 84 → 82（2026-11-21）：两条热键被移除（书签/标签右键动作补成命令后，
// 原来那两条重复绑定没有意义）。**不是随手改数字**：改完后
// `check_commands.py` 的"命令目录 / keymap / 处理器三方一致"仍是绿的，
// 说明没有命令因此失去入口。
constexpr size_t kExpectedHotkeys = 82;
constexpr size_t kExpectedSequences = 38;

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
    // 不符时**把两个值都打出来**：只给退出码 3 的话，下一个人要先猜是"表坏了"还是
    // "常量过时了"。绝大多数情况是常量过时（新增键位没同步这里）。
    if (km.hotkeys.size() != kExpectedHotkeys || km.sequences.size() != kExpectedSequences) {
        std::printf("MISMATCH  期望 %zu/%zu，实际 %zu/%zu —— 若刚加了键位，改 kExpected* 常量\n",
                    kExpectedHotkeys, kExpectedSequences, km.hotkeys.size(),
                    km.sequences.size());
        return 3;
    }
    return 0;
}
