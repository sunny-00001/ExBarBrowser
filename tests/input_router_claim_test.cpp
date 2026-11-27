// InputRouter 的点击 claim 回归自测。
//
// 真实缺陷：标签栏 "+" 在 DOWN 时按空白(empty)上报（命中内置 double=NavUp 规则并
// claim），UP 时按 new_tab 上报。旧版 OnMouseUp 只要 pending 还活着就吞掉 UP，
// 于是单击 + 毫无反应；双击时最后一次 UP 又真的新建了标签 —— 用户看到的是
// "点击新建没反应，双击变成打开我的电脑"。
//
// 这里不建窗口，只验证路由层的可观察返回值：
//   · 不同目标之间的 claim 不能跨过去吞掉新控件的默认行为
//   · 同目标/未知目标（Shell DirectUI）仍按原契约吞掉
//
// 编译（在 ExBarBrowser/ 下，复用主构建的 obj/*.obj；排除 main.obj/app.res.obj）：
//   CLANG="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"
//   "$CLANG" -std=c++20 -O1 -g -target x86_64-w64-mingw32 \
//     -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
//     -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 -Isrc \
//     -include-pch obj/common.h.gch -c tests/input_router_claim_test.cpp \
//     -o obj/input_router_claim_test.obj
//   "$CLANG" -target x86_64-w64-mingw32 -o obj/input_router_claim_test.exe \
//     obj/input_router_claim_test.obj $(ls obj/*.obj | grep -v -E \
//     '(main|app.res|input_router_claim_test)\.obj$') \
//     -static-libstdc++ -static-libgcc -lole32 -loleaut32 -luuid -luser32 \
//     -lgdi32 -lshell32 -lshlwapi -lcomctl32 -ldwmapi -luxtheme \
//     -ladvapi32 -lpropsys -luiautomationcore
#include <cstdio>

#include "input_router.h"
#include "mouse_rules.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void Reset() {
    MouseRules::Get().LoadDefaults();
    KillTimer(nullptr, 0x5101);  // InputRouter::kSingleTimer
    KillTimer(nullptr, 0x5102);  // InputRouter::kHoldTimer
}

}  // namespace

int main() {
    std::printf("InputRouter 点击 claim 跨目标隔离\n");

    // 两套规则都只在 empty 上配了 double -> NavUp，new_tab 没有任何规则。
    // DOWN 落在 empty：规则接管，DOWN/UP 的 claim 立起来。
    {
        Reset();
        InputRouter r;
        const bool down = r.OnMouseDown(MT_EMPTY, MBTN_LEFT, 0, POINT{10, 10});
        const bool up = r.OnMouseUp(MT_NEW_TAB, MBTN_LEFT, 0);
        Check(down, "空白按下：内置双击规则接管");
        Check(!up, "抬起换到 new_tab：不能吞掉 + 的默认行为");
    }

    // 同目标抬起：规则仍接管，默认行为继续跳过。
    {
        Reset();
        InputRouter r;
        r.OnMouseDown(MT_EMPTY, MBTN_LEFT, 0, POINT{10, 10});
        Check(r.OnMouseUp(MT_EMPTY, MBTN_LEFT, 0), "同目标抬起：仍吞默认行为");
    }

    // Shell DirectUI 的 UP 不判目标，传 unknown，沿用 pending 而不是当成跨目标。
    {
        Reset();
        InputRouter r;
        r.OnMouseDown(MT_EMPTY, MBTN_LEFT, 0, POINT{10, 10});
        Check(r.OnMouseUp(MT_UNKNOWN, MBTN_LEFT, 0), "unknown 抬起：沿用 pending 目标");
    }

    // 【回归：残留 pending 不能吞掉下一次无规则点击】
    // 内置 MT_EMPTY+left+double=NavUp 命中后，UP 会刻意保留 pending 等第二击。
    // 若紧接着来一次无规则 DOWN（标签拖动）再在空白处 UP，旧版 OnMouseUp 会拿
    // 旧 pending 的 claim 把这次 UP 判成 claimed —— 拖拽提交被整个吞掉。
    {
        Reset();
        InputRouter r;
        r.OnMouseDown(MT_EMPTY, MBTN_LEFT, 0, POINT{10, 10});
        r.OnMouseUp(MT_EMPTY, MBTN_LEFT, 0);   // 同目标 UP：pending 保留
        const bool down2 = r.OnMouseDown(MT_TAB, MBTN_LEFT, 0, POINT{200, 10});
        const bool up2 = r.OnMouseUp(MT_EMPTY, MBTN_LEFT, 0);
        Check(!down2, "残留 pending 后无规则 DOWN：不接管");
        Check(!up2, "残留 pending 后无规则 UP：不能吞掉组件提交");
    }

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
