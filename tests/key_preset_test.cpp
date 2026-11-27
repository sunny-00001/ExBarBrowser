// 键位方案预设：解析 + 与默认值的差异判定（不建窗口，纯数据面）。
//
// 【为什么这几条值得单测】预设的全部风险都在"看起来套用了、其实没套对"：
//   · 键位文本两种写法（Alt+Left / Alt+←）被当成"变了" → 覆盖表里多出一堆
//     "改了但没变"的条目，界面全变"已自定义"，值与默认一模一样；
//   · 反过来被当成"没变" → 用户点了方案、什么都没发生；
//   · 预设引用的命令名在本版本不存在 → 静默跳过（正确），但若连计数都不对，
//     用户就无从知道"有几条没套上"。
// 这三种错在界面上都只表现为"好像没反应"，只能靠断言钉住。
#include <cstdio>
#include <string>
#include <vector>

#include "chord.h"
#include "key_preset.h"
#include "str.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

// 键位文本归一化（与 hotkey_manager_preset.cpp 的 NormalizeKey 同口径）。
// 单测里复制一份而不是调用生产实现：NormalizeKey 是 .cpp 匿名 namespace 里的
// 私有函数，链接不到。这组断言测的是"这两个写法本来就该是同一个键"这个事实
// （chord 库的契约），不保证生产实现没改坏——生产侧回归靠 ApplyPreset 的行为测试覆盖。
static std::wstring Norm(const std::wstring& key) {
    if (key.empty()) return std::wstring();
    KeyChord c;
    if (chord::Parse(key, &c)) return chord::Format(c);
    return str::Lower(key);
}

int main() {
    // ---- 1. 同一组合键的多种写法必须归一 ----
    //
    // 这是"套用了但显示没变"的唯一成因：预设写 Alt+Left，键位表写 Alt+←，
    // 字符串比较不相等 → 白写一条覆盖项。
    Check(Norm(L"Alt+Left") == Norm(L"Alt+←"), "Alt+Left 与 Alt+← 是同一个键");
    Check(Norm(L"alt+left") == Norm(L"Alt+Left"), "大小写不影响键位身份");
    Check(Norm(L"Ctrl+Shift+T") == Norm(L"shift+ctrl+t"), "修饰键顺序不影响键位身份");
    Check(Norm(L"F5") == Norm(L"f5"), "功能键大小写不影响身份");
    // 反向：不同的键不能被归一成同一个。
    Check(Norm(L"F5") != Norm(L"F6"), "F5 与 F6 不是同一个键");
    Check(Norm(L"Ctrl+C") != Norm(L"Ctrl+Shift+C"), "Ctrl+C 与 Ctrl+Shift+C 不是同一个键");

    // ---- 2. 空串与解绑哨兵是两种不同状态 ----
    Check(Norm(L"").empty(), "空串归一后仍是空（= 保持默认，不是解绑）");
    Check(Norm(L"-") == L"-", "解绑哨兵归一后仍是哨兵");

    // ---- 3. 预设文件必须读得出来，且"程序默认"排第一 ----
    //
    // 路径用相对路径指向工程根的只读预设文件（run_all.sh 的工作目录约定）。
    // 注意：这只测 keypreset::Load 的解析能力；运行时走的是 LoadFromRoot
    // （Config::RootDir() 拼绝对路径），两者读的是同一个文件。
    const std::vector<keypreset::Preset> all = keypreset::Load(L"data/keymap_presets.json");
    Check(!all.empty(), "预设文件至少读出一个方案");
    Check(all[0].id == L"default", "「程序默认」排在第一位（用户找「怎么改回去」不该翻列表）");

    // ---- 4. 每个方案都要有 id / name，且 id 唯一 ----
    for (size_t i = 0; i < all.size(); ++i) {
        Check(!all[i].id.empty(), "方案有 id（否则命令无法引用它）");
        Check(!all[i].name.empty(), "方案有显示名（否则菜单里是一行空白）");
        for (size_t j = i + 1; j < all.size(); ++j) {
            Check(all[i].id != all[j].id, "方案 id 不重复（重复则 Find 只认第一个）");
        }
    }

    // ---- 5. 预设里的每个键位都必须解析得出来 ----
    //
    // 【为什么这条最重要】写错一个键名（"Ctlr+C"）不会报错：SetKeyForCommand 收下
    // 字符串，chord::Parse 失败 → 那条命令**按不出来**，而界面上它的键位芯片
    // 显示着 "Ctlr+C"。用户会以为是程序坏了。
    for (const auto& p : all) {
        for (const auto& kv : p.hotkeys) {
            if (kv.second.empty()) continue;   // 空 = 显式解绑，合法
            if (kv.second == L"-") continue;   // 哨兵，合法
            KeyChord c;
            const bool ok = chord::Parse(kv.second, &c);
            if (!ok) {
                std::printf("FAIL  方案「%ls」里 %ls 的键位解析不了：%ls\n", p.name.c_str(),
                            kv.first.c_str(), kv.second.c_str());
                ++g_fail;
            }
        }
    }

    // ---- 6. 预设只写差异：不能把整个键位表抄一遍 ----
    //
    // 全量预设会随命令增长而过时（新增一条命令，二十个方案各漏一条）。
    // 这里给一个宽松上限：单个方案不超过 60 条 —— 超过说明有人在往里抄全表。
    for (const auto& p : all) {
        if (p.hotkeys.size() > 60) {
            std::printf("FAIL  方案「%ls」有 %zu 条，超过 60 —— 预设应只写与默认的差异\n",
                        p.name.c_str(), p.hotkeys.size());
            ++g_fail;
        }
    }

    // ---- 7. Find 的语义：找到返回指针，找不到返回 nullptr ----
    Check(keypreset::Find(all, all[0].id) != nullptr, "Find 能找回自己的第一个方案");
    Check(keypreset::Find(all, L"__no_such_preset__") == nullptr, "Find 对未知 id 返回空");
    Check(keypreset::Find(all, L"explorer") != nullptr, "explorer 方案存在（资源管理器风格）");

    // ---- 8. 兜底：文件读不到时不能返回空列表 ----
    //
    // 空列表会让界面画出一个空菜单，用户以为功能坏了。兜底给"程序默认"一项，
    // 等于把功能降级为"一键还原默认" —— 仍然可用。
    const std::vector<keypreset::Preset> fb =
        keypreset::Load(L"data/__definitely_missing_presets__.json");
    Check(!fb.empty(), "预设文件缺失时仍有兜底方案（菜单不该是空的）");
    Check(fb[0].id == L"default", "兜底方案就是「程序默认」");

    if (g_fail) {
        std::printf("\nkey_preset_test: %d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过（方案 %zu 个）\n", all.size());
    return 0;
}
