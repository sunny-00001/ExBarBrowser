// 破坏性命令的人工确认：判据与收口实现。
//
// 【为什么独立成文件】CommandBus 所在的 commands.cpp 早已超 500 行（欠账棘轮
// 记在 data/file-length-baseline.json）。破坏性确认这一块自带一张豁免表
// 与一整套判据说明，塞在 commands.cpp 里只会让那个文件继续涨 ——
// 而"欠账只能变少不能变多"是项目规则（AGENTS.md §3）。
//
// 【这一层负责什么】
//   · 判定"这条命令执行前要不要问用户"（NeedsConfirm）。
//   · 人工调用时真的问一次，并把"用户取消了"写进失败原因与审计。
//
// 【它不负责什么】
//   · 不弹框：CommandBus 不持有窗口引用，"问用户"由 App 注入的回调实现
//     （见 App::InstallConfirmCallback）。这与 CanExecute 同一口径 ——
//     CommandDef 是纯数据，判定由调用方做。
//   · 不管机器侧：AI 走危险双门（aiAllowDanger + approve，见 ai_api.cpp）。
//     机器调用再弹模态框会把管道调用卡到超时（症状是"实例像死了"）。
#include "commands.h"

#include "common.h"

namespace {

// 【破坏性命令的"日常豁免"表】
//
// 这张表回答的是："这条命令被标成 destructive，但它丢的东西用户随时能拿回来，
// 或者它根本不是破坏性操作 —— 弹确认框只会打断日常使用。"
//
// 判据是**物理后果 + 显式白名单**，不是"能不能撤销"：
//  · destructive 且不在下面白名单 → 一定问；
//  · undoable 只说明"执行后我们能撤"，不等于"丢了也无所谓"。
//    删书签、清历史、导入覆盖书签即使进了撤销栈，用户仍要事先看到
//    "这一下会影响什么"；可撤销只影响事后的补救，不免除事前的知情权。
//    以前 `if (undoable) return false;` 把这道门悄悄关掉了，属于安全问题。
//
// 【不在表里的就要问】删文件、永久删除、移入回收站、清历史、清搜索历史、
// 清会话组、导入覆盖书签、外部程序、网络驱动器、回滚仓库文件 —— 这些丢了
// 用户要找回来，或者后果在进程外（撤销栈管不到）。
//
// 【注意】枚举项必须写 CmdId::xxx —— enum class 的成员在类外不可见。
// （TabCloseLeft 这类名字在 app.h 里还有同名成员函数，不限定反而会绑到函数上。）
const CmdId kNoConfirmDestructive[] = {
    CmdId::TabCloseOther, CmdId::TabCloseLeft, CmdId::TabCloseRight,
    CmdId::CloseLeftOne, CmdId::CloseRightOne, CmdId::TabCloseDuplicates,
    CmdId::HotkeyManager, CmdId::AiStop, CmdId::PathCopyLink,
    CmdId::DevBuild, CmdId::DevTest,
};

}  // namespace

bool CommandBus::NeedsConfirm(const CommandDef& def) const {
    if (!def.effects.destructive) return false;
    // undoable 不是豁免：书签/历史的删除都能撤，但执行前仍要问一次。
    for (CmdId id : kNoConfirmDestructive)
        if (id == def.id) return false;
    return true;
}

bool CommandBus::ConfirmIfDestructive(const CommandDef& def, const Args& args, Actor actor) {
    // 不需要问的（只读、显式日常豁免）直接放行。可撤销但 destructive 的命令
    // 不再走这条路 —— 能撤也要先问。
    if (!NeedsConfirm(def)) return true;
    // 机器侧不弹框：危险双门（aiAllowDanger + approve）已经管过。
    if (actor.kind != ActorKind::Human) return true;
    // 本次 Dispatch 内已经问过并通过了 → 不重复问。
    if (callConfirmed_) return true;

    // 【未注入回调 = 拒绝】宁可不做，也不能不确认就做破坏性操作。
    const bool ok = confirmFn_ ? confirmFn_(def) : false;
    if (!ok) {
        last_ = def.name;
        lastBlockReason_ = confirmFn_ ? L"用户取消了破坏性操作"
                                      : L"破坏性命令需要人工确认，但确认通道未就绪";
        lastFailKind_ = FailKind::CanceledByUser;
        // 审计里带上**真实参数**：被取消的那次调用到底想删什么/清什么，
        // 事后要能查出来（只记命令名的话，审计就少了最关键的那一维）。
        Audit(def, args, actor, false, L"canceled",
              FailKindCode(FailKind::CanceledByUser), lastBlockReason_, 0);
        LogDbg(L"[cmd] 破坏性命令未获确认：%s", def.name);
        return false;
    }
    callConfirmed_ = true;
    return true;
}
