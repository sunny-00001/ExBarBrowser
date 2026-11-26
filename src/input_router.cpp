#include "input_router.h"
#include "str.h"

#include "commands.h"
#include "mouse_rules.h"   // CurrentMods（输入端唯一实现）—— 配置经 InputConfig 注入，不直接 include config.h（R15）

#include <algorithm>
#include <cstdlib>
#include <map>

void InputRouter::Init(HWND main, const Theme& theme, InputConfig& cfg) {
    main_ = main;
    theme_ = &theme;
    cfg_ = &cfg;
    hint_.Create(main, theme);
    Reload();
}

void InputRouter::Reload() {
    LoadKeymap(cfg_->KeymapPath(), &keymap_);
    BuildBindings();

    MouseRules::Get().Load(cfg_->MousePath());

    seq_.SetEntries(keymap_.sequences);
    seq_.SetUniqueAuto(cfg_->SeqUniqueAuto());
    seq_.SetSingleInstant(cfg_->SeqSingleInstant());
    seq_.Reset();
    hint_.Hide();
    BuildRows();
}

void InputRouter::BuildBindings() {
    // 推导**本体**在 keybinding::Effective（唯一实现）：菜单行右端的快捷键文案
    // 也读它，于是"显示的键"与"按得出来的键"同源。这里只负责喂当前配置。
    chords_ = keybinding::Effective(keymap_, cfg_->HotkeyOverrides());

    // 应用内补充分配已于 2026-11-21 收编进 data/keymap.json：
    // F2 / Ctrl+L / Ctrl+Shift+1..8 现在和所有键位一样可见可配，且走同一份
    // 先到先得逻辑。以前这里的 kExtra 是第二套真相：用户改键会静默失效。
}

std::wstring InputRouter::ShortcutText(const std::wstring& spec) const {
    // 序列键的显示受 seqEnabled 控制：关掉之后序列根本不触发，
    // 这时还显示 "T B" 就是"按了没反应"的谎。
    return keybinding::TextFor(spec, keymap_, chords_, cfg_->SeqEnabled());
}

std::wstring InputRouter::ShortcutText(CmdId id, const std::wstring& arg) const {
    // 与上面那条**同一个**推导（keybinding::TextForCmd 内部就是反查名字再调 TextFor）。
    // 两条入口共用一份实现，于是"按名字查"与"按身份查"永远同结果。
    return keybinding::TextForCmd(id, arg, keymap_, chords_, cfg_->SeqEnabled());
}

int InputRouter::CurrentMods() const {
    // 转调输入端唯一实现（mouse_rules.h）。此前这里逐行复制了同一段
    // GetKeyState 三连 + AltGr 特判，校验器当时以"需按焦点抑制"为由放行 ——
    // 但实际读代码，两个版本一字不差，成员函数没有任何额外逻辑。
    // 保留成员形态只是为了不改调用点（238/247 行用 this->CurrentMods()）。
    return ::CurrentMods();
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

bool InputRouter::InTextEntry(HWND focus) const {
    return textEntryRoot_ && focus &&
           (focus == textEntryRoot_ || IsChild(textEntryRoot_, focus));
}

bool InputRouter::HandleKey(WPARAM vk, HWND focus, LPARAM keyInfo, int modsOverride) {
    // 修饰键来源：低层钩子路径给的是事件流推导值（更准）；普通 WM_KEYDOWN 路径
    // 只能用 GetKeyState（此时按键消息可能尚未出队，Shift 位偶尔还没刷新）。
    const int mods = (modsOverride >= 0) ? modsOverride : CurrentMods();

    // WebView2 的聊天输入框不是 EDIT 类，之前打 g / x 会被当成序列前缀。
    // 焦点进面板后先把可能残留的候选弹窗收掉，再按"文本输入区"放行所有裸键。
    const bool inTextEntry = InTextEntry(focus);
    if (inTextEntry && overlay_.visible()) Cancel();

    // 弹窗开着时：所有键都归它（序列 + 弹层作用域见 input_router_overlay.cpp）。
    if (overlay_.visible()) return HandleOverlayKey(vk, keyInfo, mods);

    wchar_t focusClass[32] = L"";
    if (focus) GetClassNameW(focus, focusClass, 32);
    const bool inEdit = inTextEntry || str::IEquals(focusClass, L"EDIT");
    const bool inTabStrip = str::IEquals(focusClass, kTabStripClass);
    const bool repeat = (keyInfo & (1LL << 30)) != 0;
    if (inEdit && seq_.pending()) Cancel();

    // 模式前缀键（与 mod 一致）：弹窗外按 / ! # @ … 直接唤起对应模式。
    // 只在无 Ctrl/Alt（允许 Shift，符号前缀要按 Shift）、非编辑框、非重复时生效，
    // 避免抢走地址栏/搜索框的 typing。
    if (!inEdit && !repeat && !seq_.pending() && modeOpener_) {
        const int m0 = mods;
        if ((m0 & (chord::kCtrl | chord::kAlt)) == 0) {
            // 修饰键状态以**本线程实测**为准（与参考 mod 的 SeqCharFromVk 同款）：
            // GetKeyboardState 的 Shift 位在按键消息尚未出队时可能还没刷新，
            // 直接按 CurrentMods() 补上，符号键（? ! ~）才认得出。
            const wchar_t c = chord::CharOfVk(static_cast<unsigned>(vk),
                                              (m0 & chord::kShift) != 0);
            if (c != 0) {
                for (const auto& m : SequenceOverlay::Modes()) {
                    if (c == m.prefix) {
                        modeOpener_(m.name);
                        return true;
                    }
                }
            }
        }
    }

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
        if (mods == 0) {
            std::wstring token = TokenOf(vk);
            if (!token.empty()) return FeedSequence(token);
            Cancel();
            return false;
        }
        Cancel();
    }

    KeyChord c = chord::FromKey(static_cast<unsigned>(vk));
    // 钩子路径：修饰键以**事件流**为准。FromKey 读的是 GetKeyState ——
    // 按键消息是异步投递的，等 UI 线程处理时物理按键可能已经松开，
    // 那样会读到"Shift 已放开"的错状态（于是 Shift+1 这类组合被判成裸键）。
    if (modsOverride >= 0) c.mods = mods;

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
        // keybinding::Binding 的字段是 spec（命令规格，可带内联参数 :3），
        // 不是 command —— 后者是 Keymap/HotkeyRow 那套结构的字段名。
        LogDbg(L"[key] %s -> %s", b.setting.c_str(), b.spec.c_str());
        CommandBus::Get().InvokeByNameLegacy(b.spec, L"", Actor::Key());
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
        int hold = cfg_->HoldDelayMs();
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
        // 钉住态：超时**既不关窗也不自动执行**（原版 HandleSequenceTimeout 分支）。
        // 否则用户只是停下来想一下，命令就被抢跑且弹层凭空消失，与「常驻控制台」
        // 的预期完全相反。改为续期，等用户显式 Esc / 点击别处。
        if (overlay_.pinned()) {
            SetTimer(main_, kSeqTimer, static_cast<UINT>(cfg_->SeqTimeoutMs()),
                     nullptr);
            return;
        }
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
    overlay_.Close();   // 序列键候选弹窗与序列状态同生命周期
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
            cfg_->SetHotkeyOverride(r.setting, kHotkeyDisabled);
        } else {
            cfg_->SetHotkeyOverride(r.setting, key);
        }
        cfg_->Save();
        Reload();
        return true;
    }
    return false;
}

void InputRouter::ResetDefaults() {
    cfg_->ClearAllHotkeyOverrides();
    cfg_->Save();
    Reload();
}

bool InputRouter::ResetKeyForCommand(const std::wstring& command) {
    for (const auto& r : rows_) {
        if (r.isSequence || !str::IEquals(r.command, command)) continue;
        cfg_->ClearHotkeyOverride(r.setting);
        cfg_->Save();
        Reload();
        return true;
    }
    return false;
}
