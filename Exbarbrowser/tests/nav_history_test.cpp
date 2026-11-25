// 导航历史栈自测（nav::History）。
//
// 断言的都是**真实踩过的坑**，不是"函数能跑"：
//   · 清历史后 idx 必须收敛，否则后退按钮"能点但点了没反应"
//   · 从中间导航必须截断前进分支，否则"后退一次退错位置"
//   · 重定向（文档 → OneDrive\文档）只能就地收敛，不能压两条
//   · 导航失败必须完全回滚，不留死条目
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/nav_history_test.cpp src/nav_history.cpp -o /tmp/nav_history_test.exe
#include <cstdio>
#include <string>

#include "nav_history.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void CheckEq(const std::wstring& got, const std::wstring& want, const char* what) {
    const bool ok = got == want;
    std::printf("  %s  %s（得到 \"%ls\"）\n", ok ? "PASS" : "FAIL", what, got.c_str());
    if (!ok) ++g_fail;
}

// 造一个已压入若干条、游标停在末尾的栈
nav::History Make(std::initializer_list<const wchar_t*> paths) {
    nav::History h;
    for (auto p : paths) h.Request(p, true);
    return h;
}

}  // namespace

int main() {
    std::printf("空栈\n");
    {
        nav::History h;
        Check(h.empty(), "初始为空");
        Check(!h.canBack() && !h.canForward(), "前后退都不可用");
        CheckEq(h.here(), L"", "here() 为空串（不编造路径）");
        Check(h.idx() == -1, "idx = -1");
    }

    std::printf("\n压栈与游标\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c"});
        Check(h.size() == 3, "压了 3 条");
        CheckEq(h.here(), L"C:\\c", "当前在 C:\\c");
        Check(h.canBack() && !h.canForward(), "能后退、不能前进");
        Check(h.backCount() == 2, "可后退 2 条");
        Check(h.forwardCount() == 0, "可前进 0 条");
    }

    std::printf("\n从中间导航必须截断前进分支\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c"});
        h.Goto(0);                     // 退到 C:\a
        Check(h.canForward(), "退到中间后能前进");
        h.Request(L"C:\\x", true);     // 从中间跳到新目录
        Check(h.size() == 2, "前进分支被截断，只剩 [C:\\a, C:\\x]");
        CheckEq(h.here(), L"C:\\x", "当前在新目录");
        Check(!h.canForward(), "不能再前进（旧分支已丢）");
    }

    std::printf("\n原地导航同一路径不重复压栈\n");
    {
        auto h = Make({L"C:\\a"});
        const bool shouldGo = h.Request(L"C:\\a", true);
        Check(!shouldGo, "目标就是当前路径时返回 false（调用方可跳过导航）");
        Check(h.size() == 1, "栈没有增长");
    }

    std::printf("\n重定向就地收敛，不压第二条\n");
    {
        auto h = Make({L"C:\\Users"});
        h.Request(L"C:\\Users\\Documents", true);   // 请求"文档"
        // Shell 回报真实路径是 OneDrive 下的
        const std::wstring settled = h.Complete(L"C:\\Users\\me\\OneDrive\\Documents");
        Check(h.size() == 2, "仍然只有 2 条（没有为真实路径再压一条）");
        CheckEq(settled, L"C:\\Users\\me\\OneDrive\\Documents", "收敛到真实路径");
        CheckEq(h.here(), L"C:\\Users\\me\\OneDrive\\Documents", "当前路径是真实路径");
    }

    std::printf("\n视图内部导航（无 pending）正常压栈\n");
    {
        auto h = Make({L"C:\\a"});
        h.Complete(L"C:\\a");                 // 同一次导航的回报
        Check(h.size() == 1, "路径没变，不压栈");

        h.Complete(L"C:\\a\\sub");            // 双击目录（没经过 Request）
        Check(h.size() == 2, "压了新条");
        CheckEq(h.here(), L"C:\\a\\sub", "当前进入子目录");
    }

    std::printf("\n导航失败完整回滚\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b"});
        h.Request(L"C:\\gone", true);         // 压了一条不存在的目录
        Check(h.size() == 3, "乐观压栈成功");
        h.Fail();
        Check(h.size() == 2, "失败后弹掉那条");
        CheckEq(h.here(), L"C:\\b", "游标退回原处");
        Check(h.canBack(), "仍能后退");
    }

    std::printf("\n历史跳转失败：不删条目，游标退回跳转前\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c"});
        h.Goto(0);
        CheckEq(h.here(), L"C:\\a", "跳到第一条");
        h.Fail();                              // 那一跳失败（目标已被删掉等）
        Check(h.size() == 3, "条目一条不少（跳转不改栈结构）");
        // 退回跳转前的位置：留在 C:\a 会让界面显示一个打不开的目录。
        CheckEq(h.here(), L"C:\\c", "游标退回跳转前的 C:\\c");
    }

    std::printf("\n清历史：只剩当前一条\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c"});
        h.Goto(1);                             // 停在中间
        h.ClearKeepingHere();
        Check(h.size() == 1, "只剩 1 条");
        CheckEq(h.here(), L"C:\\b", "留下的是当前目录");
        Check(h.idx() == 0, "idx 归 0");
        Check(!h.canBack() && !h.canForward(), "前后退都不可用");
    }

    std::printf("\n清历史后再次导航（压栈基准要对）\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c"});
        h.Goto(1);
        h.ClearKeepingHere();                  // 剩 [C:\b], idx=0
        h.Request(L"C:\\new", true);
        Check(h.size() == 2, "历史重新增长到 2 条");
        Check(h.canBack(), "能后退回清空时停留的位置");
        CheckEq(h.at(0), L"C:\\b", "旧当前目录在前");
        CheckEq(h.at(1), L"C:\\new", "新目录在后");
    }

    std::printf("\nRestore 会夹紧越界游标\n");
    {
        nav::History h;
        h.Restore({L"C:\\a", L"C:\\b"}, 99);
        Check(h.idx() == 1, "越界游标夹到末尾");
        h.Restore({}, 5);
        Check(h.idx() == -1 && h.empty(), "空栈时 idx = -1");
    }

    std::printf("\n后退/前进列表顺序（离当前由近到远）\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c"});
        h.Goto(1);
        const auto back = h.backward();
        const auto fwd = h.forward();
        Check(back.size() == 1 && back[0] == L"C:\\a", "后退列表 = [C:\\a]");
        Check(fwd.size() == 1 && fwd[0] == L"C:\\c", "前进列表 = [C:\\c]");
    }

    std::printf("\n历史上限（Push / Restore 都裁剪）\n");
    {
        nav::History h;
        for (size_t i = 0; i < nav::History::kDefaultMaxEntries + 8; ++i)
            h.Request(L"C:\\p" + std::to_wstring(i), true);
        Check(h.size() == nav::History::kDefaultMaxEntries, "Push 后不超过上限");
        Check(h.idx() == static_cast<int>(nav::History::kDefaultMaxEntries) - 1, "游标同步指向末尾");
        CheckEq(h.here(), L"C:\\p" + std::to_wstring(nav::History::kDefaultMaxEntries + 7),
                "当前路径没有被裁掉（需要可比较的 wstring）");

        std::vector<std::wstring> big;
        for (size_t i = 0; i < nav::History::kDefaultMaxEntries + 16; ++i)
            big.push_back(L"C:\\r" + std::to_wstring(i));
        h.Restore(big, static_cast<int>(big.size()) - 1);
        Check(h.size() == nav::History::kDefaultMaxEntries, "Restore 旧配置也裁到上限");
        CheckEq(h.here(), big.back(), "Restore 后当前路径仍是最后一条");
        Check(h.canBack(), "裁剪后仍能后退");
    }

    std::printf("\n可配置深度（构造注入 / SetMaxEntries 立即裁剪）\n");
    {
        // 自定义深度：构造时注入，Push 按它裁剪而非默认 512。
        nav::History h(5);
        for (size_t i = 0; i < 12; ++i) h.Request(L"C:\\d" + std::to_wstring(i), true);
        Check(h.size() == 5, "自定义深度：Push 裁到注入的上限");
        CheckEq(h.here(), L"C:\\d11", "自定义深度：当前路径未被裁掉");

        // 运行中调小：SetMaxEntries 立即裁剪，游标左移后仍指向最后一条。
        h.SetMaxEntries(3);
        Check(h.size() == 3, "SetMaxEntries：立即裁到新上限");
        CheckEq(h.here(), L"C:\\d11", "SetMaxEntries：游标仍指向最后一条");
        Check(h.canBack(), "SetMaxEntries：裁剪后仍能后退");

        // 调大不丢历史；传 0 回落默认值（防手改配置写出 0 让历史永远空着）。
        h.SetMaxEntries(10);
        Check(h.maxEntries() == 10, "SetMaxEntries：调大生效");
        h.SetMaxEntries(0);
        Check(h.maxEntries() == nav::History::kDefaultMaxEntries, "SetMaxEntries(0) 回落默认值");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
