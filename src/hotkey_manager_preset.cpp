// 套用预设：把「命令 id → 键」翻译成覆盖表写入。
//
// 【为什么这一步不能放在界面里】它有三件容易做错的事，且都在数据面：
//   1. 命令 id 要反查当前 setting（内部键改过名，预设不该因此失效）；
//   2. 键位文本要**归一化后比较**（Format 输出 "Alt+←"，而人手写 "Alt+Left"，
//      同一组合键的两种写法；直接比字符串会认为"没变"而漏写，或认为"变了"而白写）；
//   3. 整批要走一次事务（否则 30 条 = 30 次写盘 + 30 轮重建）。
// 放在界面里，任何一条错都会表现成"套用了但只有部分生效"，且不可复现。
#include "hotkey_manager.h"

#include <map>

#include "chord.h"
#include "commands.h"
#include "config.h"
#include "key_binding.h"
#include "key_preset.h"
#include "str.h"

namespace {

// 键位文本归一化：解析成功就重新 Format 一次（"Alt+Left" → "Alt+←"），
// 解析失败就小写去空格后原样返回（这样至少不会因为大小写差异被当成"变了"）。
//
// 【为什么不直接比字符串】同一组合键有多个合法写法：Alt+Left / Alt+← /
// alt+left。预设是人手写的，键位表是程序写的，两边必然不同写法。不归一化
// 就会出现"套用后右栏写着已自定义、但显示的键和默认一模一样"—— 那是纯粹的噪音，
// 而且会把覆盖表撑大（每条命令都多一个"改了但没变"的覆盖项）。
std::wstring NormalizeKey(const std::wstring& key) {
    if (key.empty()) return std::wstring();
    if (key == kHotkeyDisabled) return std::wstring(kHotkeyDisabled);
    KeyChord c;
    if (chord::Parse(key, &c)) return chord::Format(c);
    return str::Lower(key);
}

}  // namespace

int HotkeyManager::ApplyPreset(const keypreset::Preset& preset) {
    if (!router_) return 0;

    // 命令 id → (setting, 当前生效键)。同一个命令可能对应多个 setting
    // （比如 view.mode 有 8 档），所以用 vector 收。
    //
    // 【只收组合键行】序列键的 setting 不在覆盖表的管辖内（它是 keymap.json
    // 的 sequences 段），往里写覆盖项没有任何效果 —— 那会造出"写了但没用"的记录。
    struct Slot {
        std::wstring setting;
        std::wstring currentKey;   // 归一化后的当前生效键
        bool hasOverride = false;
    };
    std::map<std::wstring, std::vector<Slot>> byCommand;
    for (const Row& r : rows_) {
        if (r.sequence) continue;
        Slot s;
        s.setting = r.src.setting;
        s.currentKey = NormalizeKey(r.unbound ? std::wstring(kHotkeyDisabled) : r.src.key);
        s.hasOverride = r.custom || r.unbound;
        byCommand[r.src.command].push_back(std::move(s));
    }

    int applied = 0;
    // 整批一次事务：BeginBatch 之后的写入只改内存，EndBatch 才落盘 + 重建一次。
    // 30 条预设逐条写 = 30 次写盘 + 30 轮 Reload（每轮都会重置序列状态机）。
    InputRouter::BatchScope batch(*router_);
    for (const auto& kv : preset.hotkeys) {
        const std::wstring& command = kv.first;
        const std::wstring want = NormalizeKey(kv.second);
        auto it = byCommand.find(command);
        if (it == byCommand.end()) {
            // 预设里写了本版本没有的命令。跳过而不是报错：预设文件可能来自
            // 更新的版本，或者某条命令被临时下线了。静默跳过 + 末尾汇总条数，
            // 用户至少知道"有几条没套上"。
            continue;
        }
        for (const Slot& s : it->second) {
            // 与当前生效键相同 → 不必写覆盖项。
            //
            // 【为什么要跳过】写了会多出一条"已自定义"记录，界面把这一行标成
            // 蓝色（自定义），而它其实与默认一模一样。用户接着点"还原此项默认"
            // 会发现"什么都没变"—— 那是自找的困惑。
            if (s.currentKey == want) continue;
            // 空串 = 显式解绑（写哨兵），非空 = 直接写键。
            if (!router_->SetKeyForCommand(command, kv.second)) continue;
            ++applied;
        }
    }
    // batch 析构 → EndBatch → 一次 Save + Reload。
    return applied;
}

// ---------------------------------------------------------------- 预设菜单
//
// 【为什么用系统菜单】方案只有 4-6 条、每条要显示"名字 + 一句话说明"，
// 系统菜单的 MF_STRING 一行放不下说明，但可以把说明做成状态栏提示 ——
// 弹层（popup_list）要额外维护"项 → 动作"映射，为一个 5 项的菜单不值得。
//
// 【为什么点完要重读行】套用改的是覆盖表，行模型是从覆盖表派生的。不重读
// 就会看到"套用了但列表还是旧键"—— 而 ApplyPreset 内部已经 Reload 过
// InputRouter，所以这里只需从 router 重新拉一遍。
void HotkeyManager::OnPresetMenu() {
    if (!hwnd_) return;
    const std::vector<keypreset::Preset> all =
        keypreset::LoadFromRoot(Config::Get().RootDir());
    if (all.empty()) {
        SetStatus(L"没有可用的键位方案", true);
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    // 菜单 id 从 1 起：0 是"没选"（TrackPopupMenu 返回 0）。
    for (size_t i = 0; i < all.size(); ++i) {
        // 名字后面带一句说明。系统菜单不换行，所以截到一句话的前半 ——
        // 完整说明仍在状态栏（悬停时显示）。
        std::wstring text = all[i].name;
        if (!all[i].desc.empty()) text += L"    " + all[i].desc;
        AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(i + 1), text.c_str());
    }

    POINT sp{};
    GetCursorPos(&sp);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, hwnd_,
                                   nullptr);
    DestroyMenu(menu);
    if (cmd <= 0 || cmd > static_cast<int>(all.size())) return;

    const keypreset::Preset& chosen = all[static_cast<size_t>(cmd - 1)];
    // "程序默认" = 清空全部覆盖。它不该走逐条写入：那会把每条默认键都写成
    // 一条覆盖项，界面全变成"已自定义"，而实际值与默认一模一样。
    if (chosen.id == L"default") {
        ResetAll();
        return;
    }
    const int n = ApplyPreset(chosen);
    // 套用后重读：行模型的键位文本、来源标记（自定义/默认）都变了。
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (n == 0) {
        SetStatus(L"方案「" + chosen.name + L"」与当前键位一致，无需改动");
    } else {
        SetStatus(L"已套用方案「" + chosen.name + L"」，改动 " + std::to_wstring(n) +
                  L" 条（Ctrl+Z 可撤销单条改动）");
    }
}
