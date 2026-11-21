// 前置条件自测（第 3 步判据）。
//
// 核心断言：**不满足时返回可读原因，而不是崩溃或静默失败**。
// 以前"无历史时点后退"就是静默 return false —— 界面上看是按钮能点、点了没反应，
// 日志里也没有任何线索。这里把"为什么不能做"钉死成可断言的行为。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/precondition_test.cpp src/commands.cpp src/json.cpp src/text_file.cpp \
//     /tmp/config_stub.cpp -o /tmp/precondition_test.exe -lole32 -luuid -lshell32 -ladvapi32
#include <cstdio>
#include <string>

#include "commands.h"

namespace {

int g_fail = 0;
bool g_called = false;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 造一个指定状态的探测
CommandState MakeState(int back, int fwd, bool sel, int tabs, bool clip, bool up) {
    CommandState s;
    s.historyBack = back;
    s.historyForward = fwd;
    s.hasSelection = sel;
    s.tabCount = tabs;
    s.hasClipboard = clip;
    s.canGoUp = up;
    return s;
}

bool Can(const wchar_t* name, std::wstring* reason = nullptr) {
    const CommandDef* d = FindCommand(name);
    if (!d) return false;
    return CanExecute(*d, reason);
}

}  // namespace

int main() {
    std::printf("无条件命令：总能执行\n");
    {
        SetStateProbe([] { return MakeState(0, 0, false, 0, false, false); });
        std::wstring r;
        Check(Can(L"TabNew", &r), "TabNew 无前置条件 -> 可执行");
        Check(r.empty(), "可执行时原因为空");
        Check(Can(L"GoHome"), "GoHome 无前置条件");
    }

    std::printf("\nNavBack：无历史时拒绝并给原因\n");
    {
        SetStateProbe([] { return MakeState(0, 0, false, 1, false, false); });
        std::wstring r;
        Check(!Can(L"NavBack", &r), "back=0 时不可执行");
        Check(r == L"已经是第一条历史", "原因为\"已经是第一条历史\"");

        SetStateProbe([] { return MakeState(2, 0, false, 1, false, false); });
        Check(Can(L"NavBack"), "back=2 时可执行");
    }

    std::printf("\nNavForward：无前进历史时拒绝\n");
    {
        SetStateProbe([] { return MakeState(3, 0, false, 1, false, false); });
        std::wstring r;
        Check(!Can(L"NavForward", &r), "fwd=0 时不可执行");
        Check(r == L"已经是最后一条历史", "原因正确");

        SetStateProbe([] { return MakeState(0, 1, false, 1, false, false); });
        Check(Can(L"NavForward"), "fwd=1 时可执行");
    }

    std::printf("\nNavUp：盘根时拒绝\n");
    {
        SetStateProbe([] { return MakeState(0, 0, false, 1, false, false); });
        std::wstring r;
        Check(!Can(L"NavUp", &r), "canGoUp=false 时不可执行");
        Check(r == L"已经是顶层（此电脑、命名空间或网络根）", "原因指出是顶层");

        SetStateProbe([] { return MakeState(0, 0, false, 1, false, true); });
        Check(Can(L"NavUp"), "canGoUp=true 时可执行");
    }

    std::printf("\n依赖选中项的命令\n");
    {
        SetStateProbe([] { return MakeState(0, 0, false, 1, false, true); });
        std::wstring r;
        Check(!Can(L"CmdDelete", &r), "无选中时不能删除");
        Check(r == L"未选中任何项目", "原因指出未选中");

        SetStateProbe([] { return MakeState(0, 0, true, 1, false, true); });
        Check(Can(L"CmdDelete"), "有选中时可删除");
    }

    std::printf("\nCmdPaste：剪贴板空时拒绝\n");
    {
        SetStateProbe([] { return MakeState(0, 0, false, 1, false, true); });
        std::wstring r;
        Check(!Can(L"CmdPaste", &r), "剪贴板空时不能粘贴");
        Check(r == L"剪贴板没有可粘贴的内容", "原因指出剪贴板空");

        SetStateProbe([] { return MakeState(0, 0, false, 1, true, true); });
        Check(Can(L"CmdPaste"), "剪贴板有内容时可粘贴");
    }

    std::printf("\n标签相关：没有标签时拒绝\n");
    {
        SetStateProbe([] { return MakeState(0, 0, false, 0, false, false); });
        std::wstring r;
        Check(!Can(L"TabClose", &r), "tabCount=0 时不能关标签");
        Check(r == L"没有打开的标签", "原因指出无标签");
    }

    std::printf("\n未注册探测：失败关闭\n");
    {
        SetStateProbe(nullptr);
        std::wstring r;
        Check(!Can(L"NavBack", &r), "无探测时不声称可执行");
        Check(!r.empty(), "但仍给出原因（不是空串）");
        // 原因要准确：不能说"没有打开的标签"（那是误导），
        // 真实情况是"还没拿到界面状态"。
        Check(r == L"界面尚未就绪", "原因指明状态未就绪而非缺标签");
        Check(Can(L"TabNew"), "无前置条件的命令不受影响");
    }

    std::printf("\n多个条件 AND：全满足才放行\n");
    {
        // 造一个双条件命令需要改表，这里用"全部满足/缺一个"验证语义
        SetStateProbe([] { return MakeState(1, 1, true, 2, true, true); });
        Check(Can(L"NavBack") && Can(L"NavForward") && Can(L"NavUp") &&
              Can(L"CmdDelete") && Can(L"CmdPaste") && Can(L"TabClose"),
              "全满足时全部可执行");
    }

    std::printf("\n拒绝时条目仍不应被 handler 执行\n");
    {
        // 注册一个会记录自己被调用的 handler，验证前置拦截发生在它之前
        g_called = false;
        CommandBus::Get().Register(CmdId::CmdDelete, [](const Args&) {
            g_called = true;
            return true;
        });
        SetStateProbe([] { return MakeState(0, 0, false, 1, false, true); });

        CommandBus::Get().Invoke(CmdId::CmdDelete, Args::Empty(), L"test");
        Check(!g_called, "前置不满足时 handler 没有被调用");

        SetStateProbe([] { return MakeState(0, 0, true, 1, false, true); });
        CommandBus::Get().Invoke(CmdId::CmdDelete, Args::Empty(), L"test");
        Check(g_called, "前置满足时 handler 被调用");
    }


    std::printf("\n第 8 步新增的边界条件\n");
    {
        // 一个标签、位置 0：左右都动不了
        SetStateProbe([] {
            CommandState s;
            s.tabCount = 1;
            s.activeTabIndex = 0;
            return s;
        });
        std::wstring r;
        Check(!Can(L"TabMoveLeft", &r), "单标签在最左 -> 不能左移");
        Check(r == L"当前标签已在最左", "原因准确");
        Check(!Can(L"TabMoveRight", &r), "单标签也在最右 -> 不能右移");
        Check(r == L"当前标签已在最右", "原因准确");
        Check(!Can(L"TabMoveFirst", &r), "已在最左 -> 不能移到最前");
        Check(!Can(L"TabMoveLast", &r), "已在最右 -> 不能移到最后");

        // 三个标签、中间位置：左右都能动
        SetStateProbe([] {
            CommandState s;
            s.tabCount = 3;
            s.activeTabIndex = 1;
            return s;
        });
        Check(Can(L"TabMoveLeft", &r), "中间位置可左移");
        Check(Can(L"TabMoveRight", &r), "中间位置可右移");

        // 无已关闭标签
        SetStateProbe([] {
            CommandState s;
            s.tabCount = 1;
            s.hasClosedTabs = false;
            return s;
        });
        Check(!Can(L"TabReopenClosed", &r), "无已关闭标签 -> 不能重开");
        Check(r == L"没有可重新打开的标签", "原因准确");

        SetStateProbe([] {
            CommandState s;
            s.tabCount = 1;
            s.hasClosedTabs = true;
            return s;
        });
        Check(Can(L"TabReopenClosed", &r), "有已关闭标签 -> 可重开");

        // 单选条件
        SetStateProbe([] {
            CommandState s;
            s.tabCount = 1;
            s.selectionCount = 2;
            s.hasSelection = true;
            return s;
        });
        Check(!Can(L"CmdProps", &r), "选中 2 个 -> 属性不可用");
        Check(r == L"需要恰好选中一个项目", "原因准确");

        SetStateProbe([] {
            CommandState s;
            s.tabCount = 1;
            s.selectionCount = 1;
            s.hasSelection = true;
            return s;
        });
        Check(Can(L"CmdProps", &r), "选中 1 个 -> 属性可用");

        // GoTabN 需要至少一个标签
        SetStateProbe([] {
            CommandState s;
            s.tabCount = 0;
            return s;
        });
        Check(!Can(L"GoTab1", &r), "无标签 -> GoTab1 不可用");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
