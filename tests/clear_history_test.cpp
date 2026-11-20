// 清除历史的自测：清完之后必须"只剩当前目录一条"，且 idx 指向它。
//
// 这条断言来自一次真实缺陷风险：清历史若把栈清空但 idx 不收敛，
// 后退/前进按钮的可用态判断会出错 —— 表现为按钮能点但点了没反应，或者直接越界。
//
// 实现已收口到 nav::History（见 tests/nav_history_test.cpp 的主测试），
// 本文件只保留"清历史"这一个场景的细粒度断言，作为回归护栏。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/clear_history_test.cpp src/nav_history.cpp -o /tmp/clear_history_test.exe
#include <cstdio>
#include <string>

#include "nav_history.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

nav::History Make(std::initializer_list<const wchar_t*> paths) {
    nav::History h;
    for (auto p : paths) h.Request(p, true);
    return h;
}

}  // namespace

int main() {
    std::printf("清空前有 4 条历史、停在中间（可后退也能前进）\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c", L"C:\\d"});
        h.Goto(2);
        Check(h.canBack(), "清空前：能后退");
        Check(h.canForward(), "清空前：能前进");

        h.ClearKeepingHere();

        Check(h.size() == 1, "清空后只剩 1 条");
        Check(h.here() == L"C:\\c", "留下的是当前目录 C:\\c");
        Check(h.idx() == 0, "idx 归 0");
        Check(!h.canBack(), "清空后：不能后退");
        Check(!h.canForward(), "清空后：不能前进");
    }

    std::printf("\n清空前在末尾（只能后退）\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c"});
        h.ClearKeepingHere();
        Check(h.size() == 1 && h.here() == L"C:\\c" && h.idx() == 0, "只剩末尾那条，idx=0");
    }

    std::printf("\n清空前只有 1 条（本来就无可后退）\n");
    {
        auto h = Make({L"C:\\only"});
        h.ClearKeepingHere();
        Check(h.size() == 1 && h.idx() == 0, "保持 1 条，idx=0");
    }

    std::printf("\n空历史（新标签还没导航过）\n");
    {
        nav::History h;
        h.ClearKeepingHere();
        Check(h.empty(), "仍为空");
        Check(h.idx() == -1, "idx 保持 -1");
        Check(!h.canBack() && !h.canForward(), "前后退都不可用");
    }

    std::printf("\n清空后再次导航（压栈基准要正确）\n");
    {
        auto h = Make({L"C:\\a", L"C:\\b", L"C:\\c"});
        h.Goto(1);
        h.ClearKeepingHere();          // 剩 [C:\b], idx=0
        h.Request(L"C:\\new", true);
        Check(h.size() == 2, "历史重新增长到 2 条");
        Check(h.at(0) == L"C:\\b" && h.at(1) == L"C:\\new", "旧当前目录在前，新目录在后");
        Check(h.canBack(), "能后退回到清空时停留的位置");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
