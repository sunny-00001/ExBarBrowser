// 快捷键管理器：撤销/重做 与 剪贴板（复制 id / 导出 / 导入）。
//
// 【为什么从 actions.cpp 分出来】那个文件原本是"点击与键盘的动作"，加进撤销栈
// 与 JSON 导入导出之后长到 800 行（AGENTS.md §3 的上限是 500）。而这三件事
// 与"点击/按键"是两种语义：
//   · actions.cpp    —— 人的动作（点、按、改键、清除）
//   · 本文件          —— **数据的搬运**（撤销栈的推入/回放、剪贴板的序列化）
// 混在一起时，改一个"导出格式"要动那个每帧都在跑命中判定的翻译单元。
//
// 【撤销为什么必须存在】改键直接落盘（settings.json）。一次误点就覆盖掉用户
// 用了半年的键位，而"还原此项默认"只对**有覆盖项**的行有效 —— 覆盖掉一个
// 原本是默认的键之后，想回到"刚才那样"没有任何路径。撤销栈补的就是这条路。
#include "hotkey_manager.h"

#include "clipboard.h"
#include "json.h"
#include "str.h"

// ---------------------------------------------------------------- 撤销 / 重做

void HotkeyManager::PushUndo(const UndoStep& step) {
    undo_.push_back(step);
    if (undo_.size() > kUndoLimit) undo_.erase(undo_.begin());
    // 新的改动让"重做"失效：重做栈记的是"被撤销的那些改动"，
    // 一旦往前走了一步，那条时间线就不存在了。
    redo_.clear();
}

void HotkeyManager::ApplyUndoStep(const UndoStep& step, bool forward) {
    // forward=true → 应用 newKey（重做）；false → 回到 oldKey（撤销）。
    // oldKey/newKey 为空 = 该侧"没有覆盖项"，要删而不是写成空串。
    const std::wstring& target = forward ? step.newKey : step.oldKey;
    std::wstring command;
    for (const Row& r : rows_) {
        if (r.src.setting == step.setting) {
            command = r.src.command;
            break;
        }
    }
    if (command.empty()) return;
    if (target.empty()) {
        router_->ResetKeyForCommand(command);   // 删覆盖项 → 回落到默认
    } else if (target == kHotkeyDisabled) {
        router_->SetKeyForCommand(command, std::wstring());   // 空串 = 写解绑哨兵
    } else {
        router_->SetKeyForCommand(command, target);
    }
}

void HotkeyManager::UndoLast() {
    if (undo_.empty()) {
        SetStatus(L"没有可撤销的改动", true);
        return;
    }
    const UndoStep step = undo_.back();
    undo_.pop_back();
    ApplyUndoStep(step, false);
    redo_.push_back(step);
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    SetStatus(L"已撤销：回到 " +
              (step.oldKey.empty() ? std::wstring(L"默认键位")
                                   : (step.oldKey == kHotkeyDisabled ? std::wstring(L"已解绑")
                                                                    : step.oldKey)));
}

void HotkeyManager::RedoLast() {
    if (redo_.empty()) {
        SetStatus(L"没有可重做的改动", true);
        return;
    }
    const UndoStep step = redo_.back();
    redo_.pop_back();
    ApplyUndoStep(step, true);
    undo_.push_back(step);
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    SetStatus(L"已重做：" +
              (step.newKey.empty() ? std::wstring(L"默认键位")
                                   : (step.newKey == kHotkeyDisabled ? std::wstring(L"已解绑")
                                                                     : step.newKey)));
}

// ---------------------------------------------------------------- 剪贴板

void HotkeyManager::CopySelectedIds() {
    const std::vector<std::wstring> settings = TargetSettings();
    if (settings.empty()) {
        SetStatus(L"先选一条命令", true);
        return;
    }
    std::wstring text;
    for (const std::wstring& s : settings) {
        for (const Row& r : rows_) {
            if (r.src.setting != s) continue;
            if (!text.empty()) text += L"\r\n";
            text += r.src.command;
            break;
        }
    }
    if (!SetClipboardText(hwnd_, text)) {
        SetStatus(L"复制失败：剪贴板被占用", true);
        return;
    }
    SetStatus(settings.size() == 1 ? (L"已复制命令 id：" + text)
                                   : (L"已复制 " + std::to_wstring(settings.size()) + L" 条命令 id"));
}

void HotkeyManager::ExportToClipboard() {
    // 导出格式与 settings.json 的 hotkeys 段同构：{"setting": "key", ...}。
    //
    // 【为什么不导出自定义格式】导入方（可能是另一台机器、也可能是 AI）
    // 只要认识 settings.json 就够了。自定义格式会多一层"还得写个转换器"。
    json::Value root = json::Value::MakeObject();
    int n = 0;
    for (const Row& r : rows_) {
        if (!r.custom && !r.unbound) continue;
        // unbound 导出成哨兵 "-"，与配置里的存法一致 —— 否则导入后
        // "已解绑"会变成"没有这一项"，语义反了。
        root.Set(r.src.setting, json::Value::MakeStr(r.unbound ? kHotkeyDisabled : r.src.key));
        ++n;
    }
    if (n == 0) {
        SetStatus(L"没有自定义键位可导出", true);
        return;
    }
    const std::wstring text = json::Stringify(root, 2);
    if (!SetClipboardText(hwnd_, text)) {
        SetStatus(L"导出失败：剪贴板被占用", true);
        return;
    }
    SetStatus(L"已导出 " + std::to_wstring(n) + L" 条自定义键位到剪贴板（JSON）");
}

void HotkeyManager::ImportFromClipboard() {
    const std::wstring text = GetClipboardText(hwnd_);
    if (str::Trim(text).empty()) {
        SetStatus(L"剪贴板里没有文本可导入", true);
        return;
    }
    json::Value root;
    std::wstring err;
    if (!json::Parse(text, root, &err) || !root.IsObject()) {
        SetStatus(L"导入失败：剪贴板内容不是 JSON 对象" +
                      (err.empty() ? std::wstring() : (L"（" + err + L"）")),
                  true);
        return;
    }
    // 逐条写。未知 setting 跳过并计数 —— 静默丢弃最糟：
    // 用户会以为"导进去了"，实际什么都没发生。
    //
    // 【整批包在 BatchScope 里】导入 20 条若逐条 Save + Reload，就是 20 次写盘
    // 加 20 轮"重读 keymap + 重建 281 条绑定 + 重置序列状态机"。最后那条尤其
    // 难受：中间每一轮都会清掉用户正在输入的序列。批次内只改内存，出作用域
    // 时统一落盘 + 重建一次。
    int applied = 0;
    int skipped = 0;
    {
        InputRouter::BatchScope batch(*router_);
        for (const auto& kv : root.obj) {
            if (!kv.second.IsString() || kv.second.str.empty()) {
                ++skipped;
                continue;
            }
            std::wstring command;
            std::wstring oldKey;
            bool hadOverride = false;
            for (const Row& r : rows_) {
                if (r.src.setting != kv.first) continue;
                command = r.src.command;
                hadOverride = r.custom || r.unbound;
                oldKey = r.custom ? r.src.key : (r.unbound ? std::wstring(kHotkeyDisabled)
                                                           : std::wstring());
                break;
            }
            if (command.empty()) {
                ++skipped;
                continue;
            }
            const bool ok = (kv.second.str == kHotkeyDisabled)
                                ? router_->SetKeyForCommand(command, std::wstring())
                                : router_->SetKeyForCommand(command, kv.second.str);
            if (!ok) {
                ++skipped;
                continue;
            }
            PushUndo({kv.first, hadOverride ? oldKey : std::wstring(), kv.second.str});
            ++applied;
        }
    }   // BatchScope 析构 → 落盘 + Reload 一次
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    if (applied == 0) {
        SetStatus(L"导入失败：没有一条能应用（键位表里没有这些 setting）", true);
    } else if (skipped) {
        SetStatus(L"已导入 " + std::to_wstring(applied) + L" 条，跳过 " +
                  std::to_wstring(skipped) + L" 条（未知 setting 或格式不对）");
    } else {
        SetStatus(L"已导入 " + std::to_wstring(applied) + L" 条键位");
    }
}
