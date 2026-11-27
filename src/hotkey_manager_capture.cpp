// 快捷键管理器：改键捕获与按键录制（一台小状态机）。
//
// 【为什么单独一个文件】捕获有三个互相牵制的状态：正在捕获（等一个完整组合键）、
// 冲突待确认（同一个键要按第二次）、录制（按键被译成筛选词而不是组合键）。
// 它们的转移条件散在 OnClick / OnKeyDown / WM_KILLFOCUS 三处，但**状态本身**
// 只在这里改。放在 actions.cpp 里时，"点了一下为什么变成在改键"要跨 200 行
// 才看得明白。
//
// 【写下去之前先说清后果】改键直接落 settings.json 并立刻 Reload。所以冲突键
// 要**先问一句**（再按一次确认）—— 本程序的键位是"先到先得"，绑上去之后另一条
// 会永久按不出来，界面上的"冲突"两个字远不足以让人意识到这件事。
#include "hotkey_manager.h"

#include "chord.h"
#include "str.h"

namespace {

// 查找同键的另一条命令（不同 setting、不是序列、不是未绑定）。
// 返回它的标题；空 = 没冲突。
//
// 【为什么要判"可共存"】派发路径里唯一的作用域过滤是"标签栏作用域的裸键只在
// 焦点位于标签栏时生效"。所以同键但恰有一条是标签栏裸键时，两者**可以共存**——
// 此时不该拦用户（拦了就是"明明能绑，程序却说要确认"）。判据必须与
// InputRouter::BuildRows 的 conflictHard 同口径，否则录制时的提示与列表里的
// 状态会各说各话。
std::wstring PeerOf(const std::vector<BindingRow>& rows, const std::wstring& selfSetting,
                    const KeyChord& c) {
    const std::wstring want = chord::Format(c);
    const bool selfTabBare = (c.mods == 0);   // 待绑的键本身是不是裸键
    std::wstring selfScope;
    for (const auto& r : rows) {
        if (r.setting == selfSetting) {
            selfScope = r.scope;
            break;
        }
    }
    for (const auto& r : rows) {
        if (r.isSequence || r.unbound) continue;
        if (r.setting == selfSetting) continue;
        KeyChord other;
        if (!chord::Parse(r.key, &other)) continue;
        if (chord::Format(other) != want) continue;
        // 恰有一条是"标签栏裸键" → 可共存，不算冲突，不拦。
        const bool otherTabBare = (other.mods == 0 && r.scope == L"标签栏");
        const bool selfTabBareHere = selfTabBare && selfScope == L"标签栏";
        if (otherTabBare != selfTabBareHere) continue;
        return r.title;
    }
    return std::wstring();
}

}  // namespace

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
    std::wstring oldKey;
    for (const Row& r : rows_) {
        if (r.src.setting == setting) {
            command = r.src.command;
            // 撤销要记"改动前是什么"：原本有覆盖项就记它，没有就记空
            // （空 = 还原时删覆盖项、回落到默认）。记成"当前显示值"是错的 ——
            // 那会把默认键位固化成一条覆盖项。
            oldKey = r.custom ? r.src.key : std::wstring();
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
    PushUndo({setting, oldKey, key});
    RebuildRows();
    RebuildGroups();
    Refilter();
    Layout();
    SetStatus(L"已绑定：" + command + L" → " + key + L"（Ctrl+Z 可撤销）");
}
