// 命令总线的调用来源（actor）自测。
//
// 守的是：**handler 能不能分辨"人点的"和"机器调的"**。
//
// 为什么需要：双入口命令（人有界面路径、机器有参数路径）以前只看
// `args.IsEmpty()` —— 对无参数命令是错的，agent 传来的也是 `{}`，于是走"人侧"
// 分支弹模态窗口，而 AI 侧在等一个永远不会来的应答（实测 session.list 返回
// {"ok":false,"error":"timeout"}）。修法是在 Dispatch 期间记录 actor；
// 这里按住三件事：
//   1. actor=ai 时 handler 看得出是机器，actor=ui 时看得出是人；
//   2. IsHumanCall() 的组合语义（空参数 **且** 不是机器）；
//   3. 嵌套调用退出后，**外层仍看到自己的来源**（否则面板追问后调另一条命令，
//      返回时外层就被误判成机器了）。
//
// 【2026-11-21 新增】第 4 件事才是真正修掉的那个 bug：
// **所有人类来源都必须算"人"**。
// 以前 actor 是字符串，判定只认 `actor != L"ui"` —— 于是「系统菜单点的」
// 「快捷键触发的」「拖进来的」「命令面板选的」统统被判成**机器**。
// 后果不是"标签不好看"，而是行为反了：
//   · 用户按 Ctrl+Z 撤书签 → 被"AI 的撤销只撤到机器那一步"拦住 → 撤销看起来坏了；
//   · 用户按 Ctrl+D 收藏已有书签 → 走"机器分支"→ 弹不出"已经收藏过了"的提示；
//   · 审计里键盘操作被归成机器 → 人工改写率算错。
// 修法：actor 变成 Actor{kind, source}，判定只看 ActorKind；
// source 只是人类来源的细分标签（进审计与日志），**不参与**判定。
//
// 编译（在 ExBarBrowser/ 下）：见 tests/run_all.sh
#include <cstdio>
#include <string>

#include "commands.h"

static int fails = 0;
#define CHECK(cond, msg)                  \
    do {                                  \
        if (cond) {                       \
            printf("ok   %s\n", msg);     \
        } else {                          \
            printf("FAIL %s\n", msg);     \
            ++fails;                      \
        }                                 \
    } while (0)

// 探针状态
static bool g_machine = false;      // 外层看到的"是否机器"
static bool g_humanCall = false;    // 外层 IsHumanCall 的结果
static bool g_innerMachine = false; // 内层看到的"是否机器"
static bool g_restored = false;     // 内层返回后外层来源是否还原
static bool g_nest = false;         // 是否做嵌套那一轮

int main() {
    CommandBus& bus = CommandBus::Get();

    // 内层探针：BmkFocus 是无前置条件的无参命令
    bus.Register(CmdId::BmkFocus, [](const Args&) {
        g_innerMachine = CommandBus::Get().fromMachine();
        return true;
    });
    // 外层探针：Preferences 同样无前置条件、无参数
    bus.Register(CmdId::Preferences, [](const Args& args) {
        g_machine = CommandBus::Get().fromMachine();
        g_humanCall = IsHumanCall(args);
        if (g_nest) {
            const bool before = CommandBus::Get().fromMachine();
            CommandBus::Get().Invoke(CmdId::BmkFocus, Args::Empty(), Actor::Ai());
            g_restored = (CommandBus::Get().fromMachine() == before);
        }
        return true;
    });

    CHECK(bus.Invoke(CmdId::Preferences, Args::Empty(), Actor::Ai()), "actor=ai 时命令被执行");
    CHECK(g_machine, "actor=ai 时 handler 看得出是机器调用");
    CHECK(!g_humanCall, "actor=ai 时 IsHumanCall 为假（不会走人侧分支弹窗）");

    CHECK(bus.Invoke(CmdId::Preferences, Args::Empty(), Actor::Ui()), "actor=ui 时命令被执行");
    CHECK(!g_machine, "actor=ui 时是人的调用");
    CHECK(g_humanCall, "actor=ui 且空参数时 IsHumanCall 为真");

    CHECK(bus.Invoke(CmdId::Preferences, Args::Empty()), "不传 actor 时命令被执行");
    CHECK(!g_machine, "不传 actor 默认按人处理");

    g_nest = true;
    bus.Invoke(CmdId::Preferences, Args::Empty(), Actor::Ui());
    CHECK(g_innerMachine, "嵌套调用里内层看到的是它自己的来源（ai）");
    CHECK(g_restored, "内层返回后外层来源被还原（仍是 ui）");
    g_nest = false;

    // ---- 全部人类来源都必须算"人"（2026-11-21 的真修复）----
    //
    // 这组是回归护栏：以前这些来源全是机器，于是 Ctrl+Z 撤书签被拦、
    // Ctrl+D 重复收藏不提示。任何一个被判成机器，这条测试就红。
    struct HumanCase {
        Actor actor;
        const char* name;
    };
    const HumanCase humans[] = {
        {Actor::Ui(), "ui"},
        {Actor::Key(), "key"},
        {Actor::Mouse(), "mouse"},
        {Actor::SysMenu(), "sysmenu"},
        {Actor::Palette(), "palette"},
        {Actor::Seq(), "seq"},
        {Actor::Drop(), "drop"},
        {Actor::Settings(), "ui.settings"},
    };
    for (const HumanCase& hc : humans) {
        bus.Invoke(CmdId::Preferences, Args::Empty(), hc.actor);
        CHECK(!g_machine, hc.name);
        CHECK(g_humanCall, hc.name);
    }

    // 机器来源只有一种：AI 通道。
    bus.Invoke(CmdId::Preferences, Args::Empty(), Actor::Ai());
    CHECK(g_machine, "只有 Actor::Ai() 算机器");

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
