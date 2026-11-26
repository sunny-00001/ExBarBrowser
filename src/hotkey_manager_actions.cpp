// 快捷键管理器：交互（点击 / 键盘 / 清除 / 还原）。
//
// 【一个动作只有一处实现】点击与键盘走同一个入口：点"编辑键位…"与按回车都调
// BeginCapture；点"清除"与按 Delete 都调 ClearSelected。各写一份的结局是
// "点得动、快捷键没反应"（或反过来），而那种缺陷没有任何检查会报。
//
// 【这一层只做分派】改键捕获的状态机在 hotkey_manager_capture.cpp，
// 右键菜单在 hotkey_manager_menu.cpp，撤销/剪贴板在 hotkey_manager_clip.cpp。
// 留在这里的是"点到哪儿 → 调哪个动作"的判定本身。
#include "hotkey_manager.h"

#include <algorithm>

#include "chord.h"
#include "str.h"

std::wstring HotkeyManager::HeldModsText() const {
    // 只读修饰键的**当前按住状态**用于回显。最终写下去的键仍由 chord::FromKey
    // 统一解析（AltGr 特判等规则在那一处），这里不另立一套。
    std::wstring s;
    if (GetKeyState(VK_CONTROL) & 0x8000) s += L"Ctrl+";
    if (GetKeyState(VK_SHIFT) & 0x8000) s += L"Shift+";
    if (GetKeyState(VK_MENU) & 0x8000) s += L"Alt+";
    return s;
}

void HotkeyManager::SetStatus(const std::wstring& msg, bool error) {
    status_ = msg;
    statusError_ = error;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- 鼠标

void HotkeyManager::OnClick(int x, int y) {
    const POINT pt{x, y};

    // 【顺序很要紧】先判"点到哪"，再决定要不要取消捕获/录制。
    // 反过来写（先无条件 CancelCapture）会出现两个错：点「取消改键」会立刻
    // 重新进入捕获，点录制按钮会先停再开 —— 两处都是"按了没反应"的观感。

    // 0) 工具栏（通栏，最优先：它压在三栏之上）
    const int tool = HitTool(x, y);
    if (tool != kToolNone) {
        OnTool(tool);
        return;
    }

    // 0a) 录制按钮与搜索框（都在工具栏里，但不是"动作按钮"）。
    //
    // 搜索框的点击不能在这里吞掉：它必须落到那个真正的 EDIT 上，否则
    // 用户点搜索框不会出现插入光标。所以这里只处理录制按钮，搜索框放行。
    if (PtInRect(&recordBtn_, pt)) {
        ToggleRecording();
        return;
    }
    if (PtInRect(&searchField_, pt)) return;   // 交给 EDIT 自己处理

    // 0b) 表头：点列标题排序。表头不是"行"，所以在判行之前处理。
    const int sortCol = HitSortHeader(x, y);
    if (sortCol >= 0) {
        if (recording_) StopRecording();
        if (capturing_) CancelCapture();
        SetSort(static_cast<SortKey>(sortCol));
        return;
    }

    // 2) 右栏动作
    switch (HitDetail(x, y)) {
    case 1:
        if (capturing_) {
            CancelCapture();
        } else {
            BeginCapture();
        }
        return;
    case 2:
        if (capturing_) CancelCapture();
        ClearSelected();
        return;
    case 3:
        if (capturing_) CancelCapture();
        ResetSelected();
        return;
    case 4:
        if (capturing_) CancelCapture();
        ResetAll();
        return;
    default:
        break;
    }

    // 3) 左栏筛选项（前两组是表切换：组合键 / 序列键）
    const int gi = HitGroup(x, y);
    if (gi >= 0 && gi < static_cast<int>(groups_.size())) {
        if (recording_) StopRecording();
        if (capturing_) CancelCapture();
        SetGroup(groups_[static_cast<size_t>(gi)].id);   // 内部同步 tableType_
        return;
    }

    // 4) 行。点键位芯片 = 直接改这一行的键（与 VS Code 点 Keybinding 单元格同款），
    //    点行的别处只是选中。Ctrl/Shift 走多选。
    const int vi = HitRow(x, y);
    if (vi >= 0) {
        if (recording_) StopRecording();
        if (capturing_) CancelCapture();

        const std::wstring setting = rows_[static_cast<size_t>(view_[static_cast<size_t>(vi)])].src.setting;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (shift && anchorView_ >= 0) {
            // Shift 连选：从锚点到当前行整段加入多选（不清空已有的 ——
            // 那才是列表控件的常规语义；清空的话第二次 Shift 点击会丢掉第一段）。
            const int a = std::min(anchorView_, vi);
            const int b = std::max(anchorView_, vi);
            for (int k = a; k <= b && k < static_cast<int>(view_.size()); ++k) {
                const std::wstring s = rows_[static_cast<size_t>(view_[static_cast<size_t>(k)])].src.setting;
                if (!IsSelectedSetting(s)) multiSel_.push_back(s);
            }
            selSetting_ = setting;
            SelectViewIndex(vi);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (ctrl) {
            // Ctrl 点击 = 切换这一行的多选状态（不移动光标）。
            bool removed = false;
            for (size_t k = 0; k < multiSel_.size(); ++k) {
                if (multiSel_[k] == setting) {
                    multiSel_.erase(multiSel_.begin() + static_cast<long>(k));
                    removed = true;
                    break;
                }
            }
            if (!removed) multiSel_.push_back(setting);
            anchorView_ = vi;
            SelectViewIndex(vi);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        // 普通点击：清掉多选，回到单选。
        multiSel_.clear();
        SelectViewIndex(vi);
        const RowLayout& rl = rowLayout_[static_cast<size_t>(vi)];
        if (PtInRect(&rl.chip, pt)) {
            BeginCapture();
        } else if (!status_.empty()) {
            status_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    // 5) 空白处：只收掉瞬态，不清选择。
    if (recording_) StopRecording();
    if (capturing_) CancelCapture();
}

void HotkeyManager::OnDoubleClick(int x, int y) {
    const int vi = HitRow(x, y);
    if (vi < 0) return;
    SelectViewIndex(vi);
    BeginCapture();
}

// ---------------------------------------------------------------- 键盘

void HotkeyManager::OnKeyDown(UINT vk) {
    if (capturing_) {
        if (vk == VK_ESCAPE) {
            CancelCapture();
            return;
        }
        if (vk == VK_DELETE || vk == VK_BACK) {
            // 捕获态下按 Delete = 清空这一条（VS Code 同款），比"先 Esc 再点清除"快。
            CancelCapture();
            ClearSelected();
            return;
        }
        // Esc/修饰键之外，任何键都算一个完整组合键。
        const KeyChord c = chord::FromKey(vk);
        CommitCapture(c);
        return;
    }

    if (recording_) {
        if (vk == VK_ESCAPE) {
            StopRecording();
            return;
        }
        const KeyChord c = chord::FromKey(vk);
        if (!c.valid()) return;
        // 录到的组合键直接当搜索词：与手打 "ctrl+shift" 走同一条匹配路径，
        // 于是"按出来能筛到"与"打出来能筛到"不可能不一致。
        query_ = chord::Format(c);
        SyncSearchBox();
        Refilter();
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    switch (vk) {
    case VK_UP:
        if (!view_.empty()) {
            const int vi = SelectedViewIndex();
            const int next = vi <= 0 ? 0 : vi - 1;
            if (!(GetKeyState(VK_SHIFT) & 0x8000)) multiSel_.clear();
            SelectViewIndex(next);
        }
        return;
    case VK_DOWN:
        if (!view_.empty()) {
            const int vi = SelectedViewIndex();
            const int next = vi < 0 ? 0 : std::min(static_cast<int>(view_.size()) - 1, vi + 1);
            if (!(GetKeyState(VK_SHIFT) & 0x8000)) multiSel_.clear();
            SelectViewIndex(next);
        }
        return;
    case VK_HOME:
        if (!view_.empty()) SelectViewIndex(0);
        return;
    case VK_END:
        if (!view_.empty()) SelectViewIndex(static_cast<int>(view_.size()) - 1);
        return;
    case VK_PRIOR:
        ScrollBy(120);
        return;
    case VK_NEXT:
        ScrollBy(-120);
        return;
    case VK_RETURN:
        BeginCapture();
        return;
    case VK_SPACE:
        // 空格 = 改键（VS Code 的 Enter 语义 + 资源管理器的"空格预览"位置，
        // 这里选改键：面板里空格没有别的用途，而改键是最常用的动作）。
        BeginCapture();
        return;
    case VK_DELETE:
        ClearSelected();
        return;
    case VK_F2:
        ResetSelected();
        return;
    case VK_F3:
    case 'F': {
        // Ctrl+F = 回到搜索框（浏览器/编辑器的通用手势）。
        if (vk == 'F' && !(GetKeyState(VK_CONTROL) & 0x8000)) break;
        if (searchEdit_) SetFocus(searchEdit_);
        return;
    }
    case 'A':
        // Ctrl+A = 全选可见行。
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            SelectAllVisible();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        break;
    case 'Z':
        // Ctrl+Z = 撤销；Ctrl+Shift+Z = 重做（与 VS Code / Photoshop 同手势）。
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (GetKeyState(VK_SHIFT) & 0x8000) RedoLast();
            else UndoLast();
            return;
        }
        break;
    case 'Y':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            RedoLast();   // Ctrl+Y 是 Windows 上的重做，一并支持
            return;
        }
        break;
    case 'C':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            CopySelectedIds();
            return;
        }
        break;
    case VK_ESCAPE:
        // Esc 的语义分三层：捕获中 → 取消捕获；有搜索词 → 清搜索；
        // 都没有 → 关窗。一层到底会让"想清搜索"变成"窗口没了"。
        if (!query_.empty()) {
            query_.clear();
            SyncSearchBox();
            Refilter();
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (!multiSel_.empty()) {
            ClearMultiSelection();
            return;
        }
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    default:
        break;
    }
}

void HotkeyManager::OnTool(int id) {
    switch (id) {
    case kToolEdit: BeginCapture(); return;
    case kToolClear: ClearSelected(); return;
    case kToolReset: ResetSelected(); return;
    case kToolUndo: UndoLast(); return;
    case kToolRedo: RedoLast(); return;
    case kToolCopyId: CopySelectedIds(); return;
    case kToolExport: ExportToClipboard(); return;
    case kToolImport: ImportFromClipboard(); return;
    default: break;
    }
}

void HotkeyManager::OnTimer(UINT_PTR id) {
    if (id != kTimerReset) return;
    KillTimer(hwnd_, kTimerReset);
    // 两步确认的窗口期过了：按钮回到"还原此项默认"，避免它长时间停在一个
    // 危险的状态上（用户半小时后回来点一下，以为只是还原一项）。
    if (resetArmed_) {
        resetArmed_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------- 录制

void HotkeyManager::ClearSelected() {
    const Row* row = Selected();
    if (!row) {
        SetStatus(L"先在列表里选一条命令", true);
        return;
    }
    if (row->sequence) {
        SetStatus(L"序列键不能在这里清除（改 data/keymap.json）", true);
        return;
    }
    if (row->unbound) {
        SetStatus(L"这一条已经解绑了", true);
        return;
    }
    const std::wstring command = row->src.command;
    const std::wstring setting = row->src.setting;
    const std::wstring oldKey = row->custom ? row->src.key : std::wstring();
    if (!router_->SetKeyForCommand(command, std::wstring())) {
        SetStatus(L"清除失败：命令不可绑键", true);
        return;
    }
    PushUndo({setting, oldKey, kHotkeyDisabled});
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    // 说清"清除"到底是什么：写的是解绑哨兵，不是删掉覆盖项 ——
    // 所以它不会自己回落到默认键（那正是"清不掉"的老问题）。
    SetStatus(L"已解绑：" + command + L"（「还原」可恢复默认键，Ctrl+Z 可撤销）");
}

void HotkeyManager::ResetSelected() {
    const Row* row = Selected();
    if (!row) {
        SetStatus(L"先在列表里选一条命令", true);
        return;
    }
    if (row->sequence) {
        // 序列键没有"覆盖项"可删：它的默认值就在 keymap.json 的 sequences 里。
        SetStatus(L"序列键没有自定义覆盖可还原", true);
        return;
    }
    if (!row->custom && !row->unbound) {
        // 本来就是默认值。说清楚，而不是让用户按了没反应、以为功能坏了。
        SetStatus(L"这一条本来就是默认键位", true);
        return;
    }
    const std::wstring command = row->src.command;
    const std::wstring setting = row->src.setting;
    const std::wstring oldKey = row->custom ? row->src.key : kHotkeyDisabled;
    if (!router_->ResetKeyForCommand(command)) {
        SetStatus(L"还原失败：命令不可绑键", true);
        return;
    }
    // 还原也是"改动"：撤销要能回到还原之前那个自定义键。
    PushUndo({setting, oldKey, std::wstring()});
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    SetStatus(L"已还原默认：" + command + L"（Ctrl+Z 可撤销）");
}

void HotkeyManager::ResetAll() {
    // 两步确认，不用 MessageBox：系统消息框在深色主题下是一块刺眼的浅色窗，
    // 而且它是**模态**的 —— 为了一次确认把整个面板卡住不值得。
    // 第一步把按钮文案改掉，3 秒内没有第二步就自动退回（OnTimer）。
    if (!resetArmed_) {
        resetArmed_ = true;
        SetTimer(hwnd_, kTimerReset, 3000, nullptr);
        SetStatus(L"再点一次「还原全部」= 清空所有自定义键位（3 秒后取消）", true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    KillTimer(hwnd_, kTimerReset);
    resetArmed_ = false;
    router_->ResetDefaults();
    undo_.clear();   // 全量还原之后旧撤销步已无意义（它们指向的覆盖项都没了）
    redo_.clear();
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    SetStatus(L"已还原全部默认键位");
}
