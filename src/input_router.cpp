#include "input_router.h"
#include "str.h"

#include "commands.h"
#include "config.h"
#include "mouse_rules.h"   // CurrentMods（输入端唯一实现）

#include <algorithm>
#include <cstdlib>
#include <map>

void InputRouter::Init(HWND main, const Theme& theme) {
    main_ = main;
    theme_ = &theme;
    hint_.Create(main, theme);
    Reload();
}

void InputRouter::Reload() {
    LoadKeymap(Config::Get().KeymapPath(), &keymap_);
    BuildBindings();

    MouseRules::Get().Load(Config::Get().MousePath());

    seq_.SetEntries(keymap_.sequences);
    seq_.SetUniqueAuto(Config::Get().s().seqUniqueAuto);
    seq_.SetSingleInstant(Config::Get().s().seqSingleInstant);
    seq_.Reset();
    hint_.Hide();
    BuildRows();
}

void InputRouter::BuildBindings() {
    chords_.clear();
    auto add = [this](const KeyChord& c, const std::wstring& spec, const std::wstring& setting,
                      const std::wstring& scope) {
        if (!c.valid()) return;
        if (chord::IsReserved(c)) return;
        for (const auto& b : chords_)
            if (b.chord == c) return;  // 先到先得（与 mod 的 orderDispatch 口径一致）
        chords_.push_back({c, spec, setting, scope});
    };

    const Settings& s = Config::Get().s();
    for (const auto& hk : keymap_.hotkeys) {
        std::wstring key = Config::Get().HasHotkeyOverride(hk.setting)
                               ? Config::Get().HotkeyOverride(hk.setting)
                               : hk.key;
        // 哨兵值 = 用户在管理器里点了"清除"：这是"显式解绑"，不能再退到系统默认键。
        if (key == kHotkeyDisabled) continue;
        if (key.empty()) key = SystemKeyFor(hk.setting);
        if (key.empty()) continue;
        // 命令规格可能带内联参数（"ViewMode:3"），查目录前先拆名字。
        std::wstring name, arg;
        SplitCommandSpec(hk.command, &name, &arg);
        const CommandDef* def = FindCommand(name);
        if (!def || !def->implemented) continue;
        KeyChord c;
        if (!chord::Parse(key, &c)) continue;
        add(c, hk.command, hk.setting, def->scopeUi);
    }

    // 应用内补充分配已于 2026-11-21 收编进 data/keymap.json：
    // F2 / Ctrl+L / Ctrl+Shift+1..8 现在和所有键位一样可见可配，且走同一份
    // 先到先得逻辑。以前这里的 kExtra 是第二套真相：用户改键会静默失效。
    (void)s;
}

void InputRouter::BuildRows() {
    rows_.clear();
    std::map<std::wstring, int> counts;

    for (const auto& hk : keymap_.hotkeys) {
        BindingRow row;
        row.setting = hk.setting;
        row.command = hk.command;
        std::wstring cmdName, cmdArg;
        SplitCommandSpec(hk.command, &cmdName, &cmdArg);
        const CommandDef* def = FindCommand(cmdName);
        row.title = def ? def->title : cmdName;
        if (def && !cmdArg.empty()) row.title += L" (" + cmdArg + L")";
        row.group = def ? def->group : L"?";
        row.scope = def ? def->scopeUi : L"全局";
        row.implemented = def && def->implemented;
        std::wstring key = Config::Get().HasHotkeyOverride(hk.setting)
                               ? Config::Get().HotkeyOverride(hk.setting)
                               : hk.key;
        if (key.empty()) {
            std::wstring sys = SystemKeyFor(hk.setting);
            if (!sys.empty()) key = sys;
        }
        row.key = key == kHotkeyDisabled ? std::wstring(L"（未绑定）") : key;
        KeyChord c;
        // 哨兵 "-" 会被 chord::Parse 当成 VK_OEM_MINUS 解析成功，于是所有"未绑定"
        // 行撞在同一个 counts["-"] 上 → 管理器把 2 条以上未绑定标成"冲突"。
        if (!key.empty() && key != kHotkeyDisabled && chord::Parse(key, &c)) {
            row.reserved = chord::IsReserved(c);
            ++counts[chord::Format(c)];
        }
        rows_.push_back(std::move(row));
    }

    for (const auto& sq : keymap_.sequences) {
        BindingRow row;
        row.setting = L"sequence:" + sq.seq;
        row.command = sq.command;
        const CommandDef* def = FindCommand(sq.command);
        row.title = def ? def->title : sq.command;
        row.group = def ? def->group : L"?";
        row.scope = def ? def->scopeUi : L"全局";
        row.implemented = def && def->implemented;
        row.key = sq.seq;
        row.isSequence = true;
        rows_.push_back(std::move(row));
    }

    for (auto& r : rows_) {
        if (r.isSequence || r.key.empty()) continue;
        KeyChord c;
        if (chord::Parse(r.key, &c) && counts[chord::Format(c)] > 1) r.conflict = true;
    }

    // ---- 冲突写日志（不是只标在管理器界面上）----
    //
    // 【为什么要有这一段】BuildBindings 的 add() 是"先到先得"：同一个 chord
    // 第二次出现就被**静默丢弃**（上面那个 `if (b.chord == c) return;`）。
    // 于是冲突的真实后果是"后一条命令永远按不出来"，
    // 而用户能看到的只有快捷键管理器里那一列"冲突"字样 ——
    // 不打开那个窗口，就完全不知道自己的键位为什么没反应。
    //
    // 日志是**只读证据**（AGENTS.md §7 的验证口径）：出问题时看一眼日志就知道
    // 是不是键位撞了，而不是去猜"是不是命令没实现"。
    //
    // 【为什么放在 BuildRows 之后】这里已经把三件事都算准了：
    // 用户覆盖（Config 的 override）、系统默认键兜底（SystemKeyFor）、
    // 以及未绑定哨兵（kHotkeyDisabled）的排除 —— 与判定"冲突"用的是同一份数据，
    // 不会出现"界面说冲突、日志说没冲突"的两套口径。
    //
    // 【为什么只报一次】Reload 会在配置变化时反复调用，逐次刷屏没意义；
    // 只在第一次见到某个组合时记一条。
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        if (it->second < 2) continue;
        std::wstring who;
        for (const auto& r : rows_) {
            if (r.isSequence || r.key.empty()) continue;
            KeyChord c;
            if (!chord::Parse(r.key, &c)) continue;
            if (chord::Format(c) != it->first) continue;
            if (!who.empty()) who += L" / ";
            who += r.setting + L"→" + r.command;
        }
        LogDbg(L"[keymap] 快捷键冲突：%ls 被绑了 %d 次（先到先得，后一条永远按不出来）：%ls",
               it->first.c_str(), it->second, who.c_str());
    }
}

int InputRouter::CurrentMods() const {
    // 转调输入端唯一实现（mouse_rules.h）。此前这里逐行复制了同一段
    // GetKeyState 三连 + AltGr 特判，校验器当时以"需按焦点抑制"为由放行 ——
    // 但实际读代码，两个版本一字不差，成员函数没有任何额外逻辑。
    // 保留成员形态只是为了不改调用点（238/247 行用 this->CurrentMods()）。
    return ::CurrentMods();
}

std::wstring InputRouter::TokenOf(WPARAM vk) {
    // 这里 vk 都是已判定过范围的虚拟键码（'A'..'Z' / '0'..'9' / VK_NUMPAD0..9），
    // 全部 < 128，转 wint_t（= wchar_t 宽度）是明确的收窄。
    if (vk >= 'A' && vk <= 'Z')
        return std::wstring(1, static_cast<wchar_t>(towlower(static_cast<wint_t>(vk))));
    if (vk >= '0' && vk <= '9') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return std::wstring(1, static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0)));
    return std::wstring();
}

bool InputRouter::FeedSequence(const std::wstring& token) {
    if (!Config::Get().s().seqEnabled) {  // 用户关掉序列后，字母键应原样交给视图（type-ahead）
        Cancel();
        return false;
    }
    std::wstring cmd;
    SequenceMachine::Result r = seq_.Feed(token, &cmd);
    LogDbg(L"[seq] token=%s prefix='%s' result=%d cmd=%s", token.c_str(), seq_.prefix().c_str(),
           static_cast<int>(r), cmd.c_str());
    switch (r) {
    case SequenceMachine::kMatched:
        hint_.Hide();
        KillTimer(main_, kSeqTimer);
        CommandBus::Get().InvokeByNameLegacy(cmd, L"", Actor::Seq());
        return true;
    case SequenceMachine::kPending:
        ShowSequenceHint();
        SetTimer(main_, kSeqTimer, static_cast<UINT>(Config::Get().s().seqTimeoutMs), nullptr);
        return true;
    case SequenceMachine::kAborted:
        hint_.Hide();
        KillTimer(main_, kSeqTimer);
        return true;
    default:
        return false;
    }
}

void InputRouter::ShowSequenceHint() {
    if (!Config::Get().s().seqShowHints) return;
    std::vector<std::pair<std::wstring, std::wstring>> items;
    for (const auto& h : seq_.Hints()) {
        const CommandDef* d = FindCommand(h.command);
        items.emplace_back(h.token, d ? d->title : h.command);
    }
    hint_.Show(seq_.prefix(), items);
}

namespace {

// 地址框获得焦点时仍然必须生效的少数"全局"组合。
// 其它组合（Ctrl+Q / Ctrl+Shift+N / Delete / F2 …）一律让给 EDIT：
// 否则在地址栏里按 Ctrl+A 会被"全选文件"截走，继续打字可能触发删除。
bool GlobalWhileEditing(const KeyChord& c) {
    const int ctrl = chord::kCtrl, shift = chord::kShift;
    if ((c.mods & (ctrl | shift | chord::kAlt)) == ctrl) {
        if (c.vk == VK_TAB || c.vk == 'T' || c.vk == 'W') return true;
    }
    if ((c.mods & (ctrl | shift | chord::kAlt)) == (ctrl | shift)) {
        if (c.vk == VK_TAB || c.vk == 'T') return true;
    }
    if (c.mods == 0 && c.vk == VK_F5) return true;
    return false;
}

}  // namespace

bool InputRouter::HandleKey(WPARAM vk, HWND focus, LPARAM keyInfo) {
    wchar_t focusClass[32] = L"";
    if (focus) GetClassNameW(focus, focusClass, 32);
    const bool inEdit = str::IEquals(focusClass, L"EDIT");
    const bool inTabStrip = str::IEquals(focusClass, kTabStripClass);
    const bool repeat = (keyInfo & (1LL << 30)) != 0;
    if (inEdit && seq_.pending()) Cancel();

    // 序列进行中：Esc 取消；带修饰键则取消后按普通组合键处理
    if (seq_.pending()) {
        if (vk == VK_ESCAPE) {
            Cancel();
            return true;
        }
        // 按住键产生的重复消息不能当作序列输入（否则 g 一按住就 gg 跳首项）
        if (repeat) {
            Cancel();
            return true;
        }
        if (CurrentMods() == 0) {
            std::wstring token = TokenOf(vk);
            if (!token.empty()) return FeedSequence(token);
            Cancel();
            return false;
        }
        Cancel();
    }

    const int mods = CurrentMods();
    KeyChord c = chord::FromKey(static_cast<unsigned>(vk));

    // 地址框里只放行"全局组合"，其余全交给 EDIT（含裸键、Ctrl+A/C/V、Delete…）
    if (inEdit && !GlobalWhileEditing(c)) return false;

    for (const auto& b : chords_) {
        if (!(b.chord == c)) continue;
        // "标签栏"作用域的裸键（Shift+← 等）只在焦点位于标签栏时生效，
        // 否则会抢走文件视图的 Shift 选择 / 地址框的文本选择。
        if (b.scope == L"标签栏" && !inTabStrip &&
            (mods & (chord::kCtrl | chord::kAlt)) == 0)
            continue;
        // 按住组合键产生的重复消息不再触发命令（Ctrl+W 连关标签、Ctrl+Shift+N 连建文件夹）。
        // 匹配之后才判重：早退到 DefWindowProc 会把键漏给视图，视图又做一遍默认行为。
        // Ctrl+Tab / Ctrl+Shift+Tab 例外（浏览器习惯：按住循环切换）。
        if (repeat) {
            const bool tabCycle = c.vk == VK_TAB && (mods & ~chord::kShift) == chord::kCtrl;
            if (!tabCycle) return true;
        }
        LogDbg(L"[key] %s -> %s", b.setting.c_str(), b.command.c_str());
        CommandBus::Get().InvokeByNameLegacy(b.command, L"", Actor::Key());
        return true;
    }

    // 编辑框里不启动序列键（否则输入 g / x / y 会被当成前缀）
    if (inEdit) return false;
    if (mods == 0 && !repeat) {
        std::wstring token = TokenOf(vk);
        if (!token.empty()) return FeedSequence(token);
    }
    return false;
}

bool InputRouter::HasRule(int target, int button, int trigger, int mods) const {
    return MouseRules::Get().Find(target, button, trigger, mods) != nullptr;
}

bool InputRouter::DispatchMouse(int target, int button, int trigger, int mods) {
    const MouseRule* r = MouseRules::Get().Find(target, button, trigger, mods);
    if (!r) return false;
    if (r->action == L"command")
        return CommandBus::Get().InvokeByNameLegacy(r->command, L"", Actor::Mouse());
    if (r->action == L"none") return true;  // 显式吞掉默认行为
    LogDbg(L"[mouse] 未支持的 action=%s（target=%s）", r->action.c_str(),
           MouseTargetName(target));
    return true;
}

bool InputRouter::OnMouseDown(int target, int button, int mods, POINT screen, bool expectUp) {
    // UIA 首次调用可能失败，首击目标会是 unknown；第二击成功时允许把 unknown 升级成
    // 有效目标。反向亦然：第二击失败时沿用首击目标，避免冷启动吃掉第一次双击。
    int effective = target;
    if (effective == MT_UNKNOWN && pending_.active && pending_.target != MT_UNKNOWN)
        effective = pending_.target;

    bool hasDouble = HasRule(effective, button, TR_DOUBLE, mods);
    bool hasHold = HasRule(effective, button, TR_HOLD, mods);
    bool hasSingle = HasRule(effective, button, TR_SINGLE, mods);
    // 冷启动 UIA 失败时首击只能是 unknown。此时不能猜 single/hold 的目标，
    // 但只要该按键配有双击规则，就留一个匿名 pending，让第二击把目标补上。
    bool speculative = false;
    if (!hasDouble && !hasHold && !hasSingle) {
        if (target == MT_UNKNOWN && MouseRules::Get().HasTriggerRules(button, TR_DOUBLE)) {
            speculative = true;
            hasDouble = true;
            LogDbg(L"[mouse] 首击目标 unknown，保留匿名 pending 等待双击升级");
        } else {
            return false;
        }
    }

    // 匿名 pending 还没命中规则，第一击必须照常交给 Shell（选中不能丢）。
    if (!speculative) clickClaimed_ = true;
    // WM_PARENTNOTIFY 路径没有配对的 WM_*BUTTONUP。若等抬起再清 claim，下一次
    // 组件点击会被残留 claim 吞掉（标签栏尤其明显），所以派发后即时释放。
    // 双击第二击可能来自另一条路径（第一击 PARENTNOTIFY、第二击 DBLCLK），
    // 因此还要看第一击是否本来就不期待 UP。
    const bool priorNoUp = pending_.active && !pending_.expectUp;
    auto releaseIfNoUp = [this, expectUp, priorNoUp] {
        if (!expectUp || priorNoUp) clickClaimed_ = false;
    };

    const ULONGLONG now = GetTickCount64();
    // GetDoubleClickTime 返回 UINT，系统允许的最大值是 5000ms（控制面板上限），
    // 转 int 安全；后面与 ULONGLONG 比较时会再转回无符号。
    const int dblMs = static_cast<int>(GetDoubleClickTime());

    // 与 Exbar.cpp 一致：双击必须落在系统双击矩形内，且刚触发过双击后的第三击
    // 不能立刻再算一次双击（防三击）。只比时间会把滚动列表后的两次点击误判。
    bool inRect =
        std::abs(screen.x - pending_.pt.x) < GetSystemMetrics(SM_CXDOUBLECLK) &&
        std::abs(screen.y - pending_.pt.y) < GetSystemMetrics(SM_CYDOUBLECLK);
    bool candidate = hasDouble && pending_.active &&
                     (pending_.target == effective || pending_.target == MT_UNKNOWN) &&
                     pending_.button == button && pending_.mods == mods && inRect &&
                     now - pending_.tick <= static_cast<ULONGLONG>(dblMs) &&
                     now - lastDoubleTick_ > static_cast<ULONGLONG>(dblMs);
    if (candidate) {
        KillTimer(main_, kSingleTimer);
        KillTimer(main_, kHoldTimer);
        pending_ = PendingClick{};
        clickClaimed_ = true;  // 第二击已命中，不能再放行 Shell 默认行为
        LogDbg(L"[mouse] double target=%s button=%d", MouseTargetName(effective), button);
        DispatchMouse(effective, button, TR_DOUBLE, mods);
        lastDoubleTick_ = now;
        releaseIfNoUp();
        return true;
    }

    pending_ = PendingClick{};
    pending_.active = true;
    // 第二击失败时沿用首击的有效目标；首击失败则由第二击补上。
    pending_.target = effective != MT_UNKNOWN ? effective : target;
    pending_.button = button;
    pending_.mods = mods;
    pending_.pt = screen;
    pending_.tick = now;
    pending_.down = true;
    pending_.expectUp = expectUp;
    downPt_ = screen;
    trackingMove_ = true;

    if (hasDouble) {
        // 单击延后到双击窗口结束，避免"先执行单击再执行双击"
        SetTimer(main_, kSingleTimer, static_cast<UINT>(dblMs), nullptr);
    }
    if (hasHold) {
        int hold = Config::Get().s().holdDelayMs;
        if (hold < dblMs + 50) hold = dblMs + 50;  // 长按阈值必须大于双击时间
        SetTimer(main_, kHoldTimer, static_cast<UINT>(hold), nullptr);
    }
    if (!hasDouble && hasSingle) {
        pending_ = PendingClick{};
        DispatchMouse(effective, button, TR_SINGLE, mods);
        releaseIfNoUp();
    }
    return true;
}

bool InputRouter::OnMouseUp(int target, int button, int mods) {
    // claim 只表示"这次抬起不该再跑组件默认行为"，不代表 pending 双击窗口已结束。
    // 第一次 DOWN 命中双击规则时也会 claim（用于吞掉 UP），但必须保留 pending，
    // 否则 DirectUI 没有 DBLCLK 时第二次 DOWN 无法完成双击匹配。
    const bool hadClaim = clickClaimed_;
    clickClaimed_ = false;
    if (!pending_.active || pending_.button != button) return hadClaim;
    // 同一次物理点击不该在 DOWN 和 UP 落到两个不同的已知目标上。
    // 真出现了（组件改了命中口径、或布局在按下后变了）就不能拿 DOWN 的
    // claim 去吞 UP 所在控件的默认行为 —— 那正是"点 + 没反应"的成因。
    // Shell DirectUI 的 UP 不判目标，传 MT_UNKNOWN，这里按沿用 pending 处理。
    if (target != MT_UNKNOWN && pending_.target != MT_UNKNOWN && target != pending_.target) {
        pending_ = PendingClick{};
        return false;
    }
    pending_.down = false;
    KillTimer(main_, kHoldTimer);
    if (pending_.holdFired) {
        pending_ = PendingClick{};
        return true;
    }
    // UP 路径同样可能命中不了 UIA；有有效目标就沿用，只有双击规则时还要保留
    // anonymous pending 跨过 UP，否则 Shell 的 UP 会把冷启动的第一击清掉。
    int upTarget = target != MT_UNKNOWN ? target : pending_.target;
    const bool mayUpgrade = pending_.target == MT_UNKNOWN &&
                            MouseRules::Get().HasTriggerRules(button, TR_DOUBLE);
    if (!mayUpgrade && !HasRule(upTarget, button, TR_DOUBLE, mods)) pending_ = PendingClick{};
    return true;
}

bool InputRouter::OnWheel(int target, int mods, int delta, bool horizontal) {
    int trigger;
    if (horizontal) {
        // WM_MOUSEHWHEEL：正 delta = 向右滚（Windows 文档），别和垂直方向类比
        trigger = delta > 0 ? TR_WHEEL_RIGHT : TR_WHEEL_LEFT;
    } else {
        trigger = delta > 0 ? TR_WHEEL_UP : TR_WHEEL_DOWN;
    }
    return DispatchMouse(target, MBTN_LEFT, trigger, mods);
}

void InputRouter::OnMouseMove(POINT screen) {
    if (!trackingMove_ || !pending_.down) return;
    int dx = screen.x - downPt_.x;
    int dy = screen.y - downPt_.y;
    if (abs(dx) > GetSystemMetrics(SM_CXDRAG) || abs(dy) > GetSystemMetrics(SM_CYDRAG)) {
        // 拖拽开始：长按不再触发，但单击/双击仍保留（松手时判定）
        KillTimer(main_, kHoldTimer);
        trackingMove_ = false;
    }
}

void InputRouter::OnTimer(UINT_PTR id) {
    if (id == kSingleTimer) {
        if (pending_.active && !pending_.holdFired) {
            PendingClick p = pending_;
            pending_ = PendingClick{};
            DispatchMouse(p.target, p.button, TR_SINGLE, p.mods);
            // 双击窗口已经过完，这次物理点击结束（DirectUI 可能根本没有 UP）。
            // 即使组件后来补发 UP，也不该再被这次残留的 claim 吞掉默认行为。
            clickClaimed_ = false;
        }
        return;
    }
    if (id == kHoldTimer) {
        if (pending_.active && pending_.down && !pending_.holdFired) {
            pending_.holdFired = true;
            DispatchMouse(pending_.target, pending_.button, TR_HOLD, pending_.mods);
            if (!pending_.expectUp) clickClaimed_ = false;
        }
        return;
    }
    if (id == kSeqTimer) {
        // 超时：若刚好处在一个"完整匹配但还有更长候选"的前缀，执行它；否则取消。
        std::wstring cmd;
        if (seq_.TakePendingExact(&cmd)) {
            hint_.Hide();
            CommandBus::Get().InvokeByNameLegacy(cmd, L"", Actor::Seq());
        } else {
            Cancel();
        }
    }
}

void InputRouter::Cancel() {
    seq_.Reset();
    hint_.Hide();
    if (main_) KillTimer(main_, kSeqTimer);
}

std::wstring InputRouter::StatusText() const {
    if (seq_.pending()) return L"序列 " + seq_.prefix() + L" …";
    return CommandBus::Get().lastCommand();
}

bool InputRouter::SetKeyForCommand(const std::wstring& command, const std::wstring& key) {
    for (const auto& r : rows_) {
        if (r.isSequence || !str::IEquals(r.command, command)) continue;
        if (key.empty()) {
            // "清除" = 显式解绑（写哨兵）。不能删覆盖项：删掉之后 BuildBindings 会
            // 回落到 hk.key / SystemKeyFor，用户看到的是"清不掉"。
            Config::Get().SetHotkeyOverride(r.setting, kHotkeyDisabled);
        } else {
            Config::Get().SetHotkeyOverride(r.setting, key);
        }
        Config::Get().Save();
        Reload();
        return true;
    }
    return false;
}

void InputRouter::ResetDefaults() {
    Config::Get().mutableSettings().hotkeyOverrides.clear();
    Config::Get().Save();
    Reload();
}
