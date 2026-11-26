// 快捷键管理器：交互（点击 / 键盘 / 改键 / 清除 / 还原）。
//
// 【一个动作只有一处实现】点击与键盘走同一个入口：点"编辑键位…"与按回车都调
// BeginCapture；点"清除"与按 Delete 都调 ClearSelected。各写一份的结局是
// "点得动、快捷键没反应"（或反过来），而那种缺陷没有任何检查会报。
//
// 【写下去之前先说清楚后果】改键会直接落到 settings.json 并立刻 Reload。
// 所以冲突键要**先问一句**（再按一次确认）、还原全部要**两步确认**——
// 都是为了让"不可逆/难察觉"的动作有一个明确的停顿点。
#include "hotkey_manager.h"

#include <algorithm>

#include "chord.h"
#include "str.h"

namespace {

// 查找同键的另一条命令（不同 setting、不是序列、不是未绑定）。
// 返回它的标题；空 = 没冲突。
std::wstring PeerOf(const std::vector<BindingRow>& rows, const std::wstring& selfSetting,
                    const KeyChord& c) {
    const std::wstring want = chord::Format(c);
    for (const auto& r : rows) {
        if (r.isSequence || r.unbound) continue;
        if (r.setting == selfSetting) continue;
        KeyChord other;
        if (!chord::Parse(r.key, &other)) continue;
        if (chord::Format(other) == want) return r.title;
    }
    return std::wstring();
}

}  // namespace

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

    // 1) 录制按钮
    if (PtInRect(&recordBtn_, pt)) {
        ToggleRecording();
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

    // 3) 左栏筛选项
    const int gi = HitGroup(x, y);
    if (gi >= 0 && gi < static_cast<int>(groups_.size())) {
        if (recording_) StopRecording();
        if (capturing_) CancelCapture();
        const std::wstring id = groups_[static_cast<size_t>(gi)].id;
        if (id != group_) {
            group_ = id;
            status_.clear();
            statusError_ = false;
            Refilter();
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    // 4) 行。点键位芯片 = 直接改这一行的键（与 VS Code 点 Keybinding 单元格同款），
    //    点行的别处只是选中。
    const int vi = HitRow(x, y);
    if (vi >= 0) {
        if (recording_) StopRecording();
        if (capturing_) CancelCapture();
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
            SelectViewIndex(vi <= 0 ? 0 : vi - 1);
        }
        return;
    case VK_DOWN:
        if (!view_.empty()) {
            const int vi = SelectedViewIndex();
            SelectViewIndex(vi < 0 ? 0 : std::min(static_cast<int>(view_.size()) - 1, vi + 1));
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
    case VK_ESCAPE:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    default:
        break;
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

void HotkeyManager::ToggleRecording() {
    if (recording_) {
        StopRecording();
        return;
    }
    CancelCapture();
    recording_ = true;
    // 进录制就清空搜索词：录到的是"新的筛选条件"，接着上一轮的文字只会
    // 让结果为空，看起来像"按了没反应"。
    query_.clear();
    SyncSearchBox();
    Refilter();
    Layout();
    SetFocus(hwnd_);   // 键盘必须落在本窗口，否则录不到键
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HotkeyManager::StopRecording() {
    if (!recording_) return;
    recording_ = false;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- 改键

void HotkeyManager::BeginCapture() {
    const Row* row = Selected();
    if (!row) {
        SetStatus(L"先在列表里选一条命令", true);
        return;
    }
    if (capturing_ && captureSetting_ == row->src.setting) return;
    if (row->sequence) {
        // 序列键要改状态机（多键 + 超时 + 唯一匹配），不是"换个字符串"。
        // 说清去处，不让人在这里反复试。
        SetStatus(L"序列键请改 data/keymap.json 的 sequences（需要多键状态机）", true);
        return;
    }
    if (!row->src.implemented) {
        SetStatus(L"该命令在当前版本未实现，改键不会生效", true);
        return;
    }
    if (recording_) StopRecording();
    capturing_ = true;
    captureSetting_ = row->src.setting;
    conflictPending_ = false;
    status_.clear();
    statusError_ = false;
    SetFocus(hwnd_);   // 键盘必须落在本窗口，否则捕获不到
    const int vi = SelectedViewIndex();
    if (vi >= 0) EnsureVisible(vi);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HotkeyManager::CancelCapture() {
    if (!capturing_) return;
    capturing_ = false;
    captureSetting_.clear();
    conflictPending_ = false;
    pendingPeer_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HotkeyManager::CommitCapture(const KeyChord& c) {
    if (!capturing_) return;
    const std::wstring key = chord::Format(c);
    const std::wstring setting = captureSetting_;

    if (chord::IsReserved(c)) {
        // 系统先吃掉的键，注册了也轮不到本程序。这里**不写**，否则用户会得到
        // 一条"看起来绑上了、按下去毫无反应"的记录 —— 那是本项目最不能容忍的谎。
        MessageBeep(MB_ICONWARNING);
        SetStatus(L"该组合被系统占用（" + key + L"），换一个", true);
        conflictPending_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 已经在等确认、且按的还是同一个键 → 就这么绑。
    if (conflictPending_ && pendingChord_ == c) {
        conflictPending_ = false;
    } else {
        const std::wstring peer = PeerOf(router_->Rows(), setting, c);
        if (!peer.empty()) {
            // 冲突键先拦住一次。本程序的规则是"先到先得"：绑上去之后**另一条**
            // 会永久按不出来，而界面上的"冲突"两个字远不足以让人意识到这件事。
            conflictPending_ = true;
            pendingChord_ = c;
            pendingPeer_ = peer;
            MessageBeep(MB_ICONWARNING);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    std::wstring command;
    for (const Row& r : rows_) {
        if (r.src.setting == setting) {
            command = r.src.command;
            break;
        }
    }
    capturing_ = false;
    captureSetting_.clear();
    pendingPeer_.clear();
    if (command.empty()) return;

    if (!router_->SetKeyForCommand(command, key)) {
        SetStatus(L"改键失败：命令不可绑键", true);
        return;
    }
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    SetStatus(L"已绑定：" + command + L" → " + key);
}

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
    if (!router_->SetKeyForCommand(command, std::wstring())) {
        SetStatus(L"清除失败：命令不可绑键", true);
        return;
    }
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    // 说清"清除"到底是什么：写的是解绑哨兵，不是删掉覆盖项 ——
    // 所以它不会自己回落到默认键（那正是"清不掉"的老问题）。
    SetStatus(L"已解绑：" + command + L"（可用「还原此项默认」恢复默认键）");
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
    if (!router_->ResetKeyForCommand(command)) {
        SetStatus(L"还原失败：命令不可绑键", true);
        return;
    }
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    SetStatus(L"已还原默认：" + command);
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
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    SetStatus(L"已还原全部默认键位");
}
