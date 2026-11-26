#include "commands.h"

#include "str.h"

#include "audit_log.h"
#include "cmd_naming.h"
#include "common.h"
#include "config.h"
#include "text_file.h"

#include <algorithm>

// FailKind → errorCode（唯一映射处，见 commands.h 里 FailKind 的说明）。
//
// 【为什么必须只有这一处】errorCode 是给机器分支用的：
// 同一类别在两处各写一个字符串，改了一处忘了另一处，客户端就会掉进 default 分支。
// 枚举在这里翻成码，映射就只有一个真源。
//
// 【为什么必须在匿名 namespace 之外】commands.h 对外声明了它 ——
// 声明是外部链接，定义就必须是外部链接。落在匿名 namespace 里的话，
// 定义只有内部链接，别的 TU 链接不上（症状是 undefined symbol，
// 而头文件与实现都"看着对"）。
const wchar_t* FailKindCode(FailKind k) {
    switch (k) {
        case FailKind::PreconditionUnmet: return L"precondition_unmet";
        case FailKind::LockConflict:     return L"lock_conflict";
        case FailKind::CanceledByUser:   return L"canceled_by_user";
        case FailKind::NotSupported:     return L"not_supported";
        case FailKind::TimedOut:         return L"timed_out";
        case FailKind::None:             break;
    }
    return L"";   // None 不是一种错误
}

// ---------------------------------------------------------------- 派发总线
//
// 【为什么表不在这里】命令表本体与查表入口在 commands_table.cpp（数据），
// 这个文件只管「能不能执行、怎么路由、返回什么」（行为）。
bool SplitCommandSpec(const std::wstring& spec, std::wstring* name, std::wstring* arg) {
    size_t colon = spec.find(L':');
    if (name) *name = colon == std::wstring::npos ? spec : spec.substr(0, colon);
    if (arg) *arg = colon == std::wstring::npos ? std::wstring() : spec.substr(colon + 1);
    return !spec.empty();
}

// ---------------------------------------------------------------- 可否执行

namespace {

StateProbe g_probe;

}  // namespace

void SetStateProbe(StateProbe fn) { g_probe = std::move(fn); }

bool CanExecute(const CommandDef& def, std::wstring* reason, int targetTab) {
    auto deny = [reason](Req r) {
        if (reason) *reason = ReqReason(r);
        return false;
    };

    if (def.requires_.Empty()) {
        if (reason) reason->clear();
        return true;
    }

    // 未注册探测 = 不知道状态 = 不能声称可执行（失败关闭）。
    // 返回一个明确原因而不是空串，否则 UI 会显示"灰按钮但没有解释"。
    if (!g_probe) return deny(Req::StateUnavailable);

    const CommandState st = g_probe();
    for (Req r : def.requires_.items) {
        if (r == Req::None) continue;
        switch (r) {
        case Req::HasHistoryBack:    if (st.historyBack <= 0) return deny(r); break;
        case Req::HasHistoryForward: if (st.historyForward <= 0) return deny(r); break;
        case Req::HasSelection:      if (!st.hasSelection) return deny(r); break;
        case Req::HasTabs:           if (st.tabCount <= 0) return deny(r); break;
        case Req::HasClipboard:      if (!st.hasClipboard) return deny(r); break;
        case Req::CanGoUp:           if (!st.canGoUp) return deny(r); break;
        // 目标标签：右键菜单传了 index 就按它判断；缺省仍是活动标签（快捷键/面板）。
        // 不这么改的话，右键第一个标签「左移」会被"活动标签已在最左"误拒。
        case Req::CanMoveTabLeft: {
            const int at = targetTab >= 0 ? targetTab : st.activeTabIndex;
            if (at <= 0 || at >= st.tabCount) return deny(r);
            break;
        }
        case Req::CanMoveTabRight: {
            const int at = targetTab >= 0 ? targetTab : st.activeTabIndex;
            if (at < 0 || at >= st.tabCount - 1) return deny(r);
            break;
        }
        case Req::HasClosedTabs:     if (!st.hasClosedTabs) return deny(r); break;
        case Req::HasMultipleTabs:   if (st.tabCount < 2) return deny(r); break;
        case Req::HasSelectionSingle: if (st.selectionCount != 1) return deny(r); break;
        case Req::StateUnavailable:  return deny(r);   // 走到这里说明 probe 刚被清空
        case Req::None:              break;
        }
    }
    if (reason) reason->clear();
    return true;
}

CommandBus& CommandBus::Get() {
    static CommandBus bus;
    return bus;
}

void CommandBus::Register(CmdId id, Handler fn) {
    // 与 RegisterWithResult 互斥检查：同一命令在两个通道都注册时，
    // Dispatch 会让带结果的胜出，这里的注册就被静默忽略。
    // 那不是崩溃，而是"我明明注册了却没跑"—— 所以当场警告。
    for (const auto& kv : resultHandlers_) {
        if (kv.first == id) {
            const CommandDef* d = FindCommand(id);
            LogDbg(L"[cmd] 冲突：%s 已用 RegisterWithResult 注册，"
                   L"这次的 Register 会被忽略", d ? d->name : L"?");
            break;
        }
    }
    for (auto& kv : handlers_) {
        if (kv.first == id) {
            kv.second = std::move(fn);
            return;
        }
    }
    handlers_.emplace_back(id, std::move(fn));
}

void CommandBus::RegisterByName(const wchar_t* name, Handler fn) {
    if (const CommandDef* d = FindCommand(name)) Register(d->id, std::move(fn));
}

void CommandBus::RegisterWithResult(CmdId id, ResultHandler fn) {
    // 与 Register 互斥检查：同一命令在两个通道都注册时，Dispatch 会让带结果的
    // 胜出。Register 会检查"反向"并警告，这里检查**正向**（先 Register 后
    // RegisterWithResult 时，先注册的普通 handler 被静默遮蔽）——
    // 两个方向都要提醒，否则会出现"我注册了 handler 却没跑"且日志全空的排查灾难。
    for (const auto& kv : handlers_) {
        if (kv.first == id) {
            const CommandDef* d = FindCommand(id);
            LogDbg(L"[cmd] 冲突：%s 已用 Register 注册，"
                   L"它的 handler 会被 RegisterWithResult 遮蔽", d ? d->name : L"?");
            break;
        }
    }
    for (auto& kv : resultHandlers_) {
        if (kv.first == id) {
            kv.second = std::move(fn);
            return;
        }
    }
    resultHandlers_.emplace_back(id, std::move(fn));
}

bool CommandBus::HasResult(CmdId id) const {
    for (const auto& kv : resultHandlers_)
        if (kv.first == id) return true;
    return false;
}

bool CommandBus::HasResult(const std::wstring& name) const {
    const CommandDef* d = FindCommand(name);
    return d && HasResult(d->id);
}

bool CommandBus::Invoke(CmdId id, const Args& args, Actor actor) {
    return Dispatch(id, args, nullptr, actor);
}

bool CommandBus::InvokeResult(CmdId id, const Args& args, json::Value* result,
                             Actor actor) {
    return Dispatch(id, args, result, actor);
}

namespace {

// 【破坏性命令的"日常豁免"表】与 NeedsConfirm 的实现已移出本文件，
// 见 src/commands_confirm.cpp —— commands.cpp 早已超 500 行（欠账棘轮），
// 往里塞只会让它继续涨。

}  // namespace

bool CommandBus::Dispatch(CmdId id, const Args& args, json::Value* result, Actor actor) {
    // 记下本次来源（handler 经 fromMachine() 读），离开函数时还原 ——
    // 命令可能嵌套调用（面板追问后调另一条），还原保证内层退出后外层仍看到自己的来源。
    struct ActorScope {
        Actor* slot;
        Actor prev;
        ~ActorScope() { *slot = prev; }
    } actorScope{&actor_, actor_};
    actor_ = actor;
    // 本次调用的"已确认"标记同样要还原：命令可能嵌套调用，
    // 内层确认过不该让外层（还没问用户的那次）跟着跳过确认。
    struct ConfirmScope {
        bool* slot;
        bool prev;
        ~ConfirmScope() { *slot = prev; }
    } confirmScope{&callConfirmed_, callConfirmed_};
    const CommandDef* def = FindCommand(id);
    if (!def) return false;

    // 每次都先清空：否则上一次的拒绝原因会被下一次成功调用带出去，
    // 调用方读到"成功了但原因还是旧的"，比没有原因更误导。
    lastBlockReason_.clear();
    pendingReason_.clear();
    lastFailKind_ = FailKind::None;
    pendingKind_ = FailKind::None;
    lastWarning_.clear();
    pendingWarning_.clear();

    // 前置检查：不满足就不调 handler。以前这里没有这一层，结果是
    // "无历史时点后退"——handler 里静默 return false，界面毫无反馈。
    // 现在拒绝时给出明确原因（审计里也能看到），并把原因留给调用方：
    // 对外报"已经是顶层"远比笼统的"命令执行失败"有用。
    // 目标标签下标：右键菜单会传 index（作用于被右键的标签，可能不是活动标签）。
    // 前置检查据此判断"能不能左移/右移"，不能一律拿活动标签。
    const int targetTab = args.Has(L"index") ? args.Int(L"index", -1) : -1;
    std::wstring reason;
    if (!CanExecute(*def, &reason, targetTab)) {
        last_ = def->name;
        lastBlockReason_ = reason;
        // 前置条件不满足 —— 这就是 PreconditionUnmet 的定义，
        // 不需要 handler 再报一次（它根本没被调用）。
        lastFailKind_ = FailKind::PreconditionUnmet;
        Audit(*def, args, actor, false, L"blocked",
              FailKindCode(FailKind::PreconditionUnmet), reason, 0);
        LogDbg(L"[cmd] 前置不满足：%s（%s）", def->name, reason.c_str());
        if (result) *result = json::Value::MakeObject();
        return false;
    }

    // 破坏性命令的**唯一**人工确认收口处。
    //
    // 【为什么放在这里】以前确认框只写在 App::InvokeFromPalette 里 ——
    // 于是"命令面板"这一条路径有确认，而序列（q x）、弹层菜单（清除搜索历史）、
    // 快捷键都直接执行：同一个破坏性操作，人从不同入口点，确认强度差一半
    // （实测：序列 q x 直接把 19 条搜索历史清空，一道确认都没有）。
    // 收口到总线后，所有人工入口一律同一道门；机器侧由危险双门管，互不干扰。
    //
    // 判定与"被取消"的处理都在 src/commands_confirm.cpp（NeedsConfirm +
    // ConfirmIfDestructive）—— 本文件早已超 500 行，往里塞只会让它继续涨。
    if (!ConfirmIfDestructive(*def, args, actor)) {
        if (result) *result = json::Value::MakeObject();
        return false;
    }

    // 先找带返回值的 handler。
    //
    // 注意：同一命令如果两边都注册过，这里带结果的那个会胜出、另一个被忽略。
    // 这是刻意的（带结果的是"更强的实现"），但如果不提醒就会变成
    // "我注册了 handler 却没跑"这种极难查的问题 —— 所以注册时就报警，
    // 而不是等到运行时表现为"命令没反应"。
    for (auto& kv : resultHandlers_) {
        if (kv.first != id) continue;
        json::Value out = json::Value::MakeObject();
        // 耗时：只量 handler 本身，不含参数校验与审计落盘 ——
        // C6 要的是"这次调用花了多久"，而审计落盘自己也会花时间，
        // 把它算进去会得到一个随磁盘状态波动的数。
        const ULONGLONG t0 = GetTickCount64();
        bool ok = kv.second ? kv.second(args, &out) : false;
        const long long dur = static_cast<long long>(GetTickCount64() - t0);
        last_ = def->name;
        // handler 自己报的原因优先于笼统的"失败"：它知道是名字错还是值超范围。
        if (!ok && !pendingReason_.empty()) lastBlockReason_ = pendingReason_;
        if (!ok) lastFailKind_ = pendingKind_;
        // 告警与失败是**两条**通道：告警不改变 ok，但要跟着结果走。
        // 失败时那条告警没有意义（主操作都没做成），所以只在 ok 时带出去。
        if (ok) lastWarning_ = pendingWarning_;
        // 审计里的机器码与 AI 通道返回的 errorCode 是**同一个来源**
        // （FailKindCode）—— 同一件事不能有两个源头。
        // 成功记录里放告警：审计不能只记"ok"，否则"改成了但没写盘"这件事
        // 在任何地方都查不到。
        Audit(*def, args, actor, ok, ok ? L"ok" : L"failed",
              ok ? std::wstring() : FailKindCode(pendingKind_),
              ok ? pendingWarning_ : pendingReason_, dur);
        if (result) *result = std::move(out);
        return ok;
    }

    for (auto& kv : handlers_) {
        if (kv.first != id) continue;
        const ULONGLONG t0 = GetTickCount64();
        bool ok = kv.second ? kv.second(args) : false;
        const long long dur = static_cast<long long>(GetTickCount64() - t0);
        last_ = def->name;
        if (!ok && !pendingReason_.empty()) lastBlockReason_ = pendingReason_;
        if (!ok) lastFailKind_ = pendingKind_;
        if (ok) lastWarning_ = pendingWarning_;
        // 审计里的机器码与 AI 通道返回的 errorCode 是**同一个来源**
        // （FailKindCode）—— 同一件事不能有两个源头。
        Audit(*def, args, actor, ok, ok ? L"ok" : L"failed",
              ok ? std::wstring() : FailKindCode(pendingKind_),
              ok ? pendingWarning_ : pendingReason_, dur);
        // 无返回值命令：给空对象而非旧数据（不编造）。
        if (result) *result = json::Value::MakeObject();
        return ok;
    }
    Audit(*def, args, actor, false, L"not_implemented", L"not_implemented",
          std::wstring(), 0);
    LogDbg(L"[cmd] 未实现：%s（actor=%s）", def->name, actor.source ? actor.source : L"?");
    if (result) *result = json::Value::MakeObject();
    return false;
}

bool CommandBus::InvokeByNameResult(const std::wstring& name, const Args& args,
                                    json::Value* result, Actor actor) {
    const CommandDef* def = FindCommand(name);
    if (!def) {
        LogDbg(L"[cmd] 未知命令：%s（actor=%s）", name.c_str(), actor.source ? actor.source : L"?");
        if (result) *result = json::Value::MakeObject();
        return false;
    }
    return Dispatch(def->id, args, result, actor);
}

bool CommandBus::InvokeByName(const std::wstring& name, const Args& args, Actor actor) {
    const CommandDef* def = FindCommand(name);
    if (!def) {
        LogDbg(L"[cmd] 未知命令：%s（actor=%s）", name.c_str(), actor.source ? actor.source : L"?");
        return false;
    }
    return Dispatch(def->id, args, nullptr, actor);
}

bool CommandBus::InvokeLegacy(CmdId id, const std::wstring& arg, Actor actor) {
    const CommandDef* def = FindCommand(id);
    if (!def) return false;
    return Invoke(id, ArgsFromLegacyArg(*def, arg), actor);
}

bool CommandBus::InvokeByNameLegacy(const std::wstring& name, const std::wstring& arg,
                                    Actor actor) {
    // 支持 "ViewMode:3" 这种内联参数（keymap / 序列 / 鼠标规则同源）：
    // 显式传入的 arg 优先，没有才用规格里的。
    std::wstring cmdName, inlineArg;
    SplitCommandSpec(name, &cmdName, &inlineArg);
    const CommandDef* def = FindCommand(cmdName);
    if (!def) {
        LogDbg(L"[cmd] 未知命令：%s（actor=%s）", name.c_str(), actor.source ? actor.source : L"?");
        return false;
    }
    return Invoke(def->id, ArgsFromLegacyArg(*def, arg.empty() ? inlineArg : arg), actor);
}
