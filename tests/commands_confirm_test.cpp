// 破坏性确认判据的回归：undoable 本身不能当"免问"理由 ——
// 只有显式写进白名单的命令才免问（当前按用户要求豁免的是单条 bmk.remove）。
// 这里只测纯判定，不弹框、不启动窗口；ConfirmIfDestructive 的回调行为
// 仍由 CommandBus 的调用方承担。
#include <cstdio>

#include "commands.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("单条书签删除免问，其余可撤销破坏性命令仍要确认\n");
    {
        const CommandDef* del = FindCommand(L"bmk.remove");
        Check(del != nullptr, "命令表里有 bmk.remove");
        if (del) {
            Check(del->effects.destructive, "bmk.remove 仍声明 destructive（没有偷偷改成无害）");
            Check(del->effects.undoable, "bmk.remove 声明 undoable（可恢复，是免问的前提）");
            Check(!CommandBus::Get().NeedsConfirm(*del), "单条删除在白名单里，不弹确认框");
        }
        const CommandDef* clear = FindCommand(L"bmk.clear");
        Check(clear != nullptr && clear->effects.destructive && clear->effects.undoable,
              "bmk.clear 同样是 destructive + undoable");
        if (clear) Check(CommandBus::Get().NeedsConfirm(*clear),
                         "清全部书签影响一大片，仍要确认");
    }

    std::printf("\n显式白名单仍然放行\n");
    {
        const CommandDef* build = FindCommand(L"dev.build");
        Check(build != nullptr && build->effects.destructive, "dev.build 是 destructive");
        if (build) Check(!CommandBus::Get().NeedsConfirm(*build), "dev.build 在白名单里，不重复问");
    }

    std::printf("\n非破坏性命令不触发确认\n");
    {
        const CommandDef* list = FindCommand(L"bmk.list");
        Check(list != nullptr && !list->effects.destructive, "bmk.list 不是 destructive");
        if (list) Check(!CommandBus::Get().NeedsConfirm(*list), "只读命令不需要确认");
    }

    std::printf("\n结果: %d 失败\n", g_fail);
    return g_fail;
}
