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
            CommandBus::Get().Invoke(CmdId::BmkFocus, Args::Empty(), L"ai");
            g_restored = (CommandBus::Get().fromMachine() == before);
        }
        return true;
    });

    CHECK(bus.Invoke(CmdId::Preferences, Args::Empty(), L"ai"), "actor=ai 时命令被执行");
    CHECK(g_machine, "actor=ai 时 handler 看得出是机器调用");
    CHECK(!g_humanCall, "actor=ai 时 IsHumanCall 为假（不会走人侧分支弹窗）");

    CHECK(bus.Invoke(CmdId::Preferences, Args::Empty(), L"ui"), "actor=ui 时命令被执行");
    CHECK(!g_machine, "actor=ui 时是人的调用");
    CHECK(g_humanCall, "actor=ui 且空参数时 IsHumanCall 为真");

    CHECK(bus.Invoke(CmdId::Preferences, Args::Empty()), "不传 actor 时命令被执行");
    CHECK(!g_machine, "不传 actor 默认按人处理");

    g_nest = true;
    bus.Invoke(CmdId::Preferences, Args::Empty(), L"ui");
    CHECK(g_innerMachine, "嵌套调用里内层看到的是它自己的来源（ai）");
    CHECK(g_restored, "内层返回后外层来源被还原（仍是 ui）");

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
