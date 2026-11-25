// 「再点即收」状态的自测：弹层开着时同键的重入必须被挡下，关掉后必须能立刻重开。
//
// 这条断言的来源是一次真实缺陷：点地址栏分隔符，箭头变成「⌄」并弹出子目录列表；
// 再点一次，箭头变回「›」—— 但列表**又弹了出来**，用户根本收不掉。
//
// 第一版修法用 400 ms 时间窗判"连点"，实测无效，原因有两条（都写进 popup_toggle.h 了）：
//   · 用户是"看一眼列表 → 再点收起"，间隔 1~2 秒，任何毫秒级窗口都过期；
//   · 更致命的是 WM_CLOSE 是**投递**的，等那一下点击到达锚点时旧弹层已关干净，
//     "关闭后靠记录猜"的判据永远差一拍。
//
// 现在的修法分两层，本文件只测第二层（第一层是消息层的捕获，见 popup_list.cpp）：
//   ① 消息层：列表持有鼠标捕获，关掉弹层的那一下点击被列表吃掉 → 锚点收不到 → 不重入。
//   ② 状态层（本文件）：IsOpen(key) 如实反映"这个锚点的弹层此刻是否开着"，
//      供 Show* 兜底与面包屑箭头形态（› / ⌄）使用。
//
// 锁的不变式：
//   · 登记后 IsOpen 为真；注销后立即为假（"关掉再开"不能被吃掉）
//   · 注销只作用于自己那一个键（颜色二级菜单嵌套时不能误清外层）
//   · 空键一律不参与（命令面板等没有"同一个锚点"的场合）
//
// 编译（在 ExBarBrowser/ 下）：
//   "/c/Program Files/Windhawk/Compiler/bin/clang++.exe" -std=c++20 -O1 \
//     -target x86_64-w64-mingw32 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/popup_toggle_test.cpp src/popup_toggle.cpp -o /tmp/popup_toggle_test.exe \
//     -static -static-libstdc++ -static-libgcc
//   /tmp/popup_toggle_test.exe
#include <cstdio>
#include <string>

#include "popup_toggle.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    // 1. 开 → 开着 → 关 → 立刻能再开
    std::printf("开 / 关 / 再开\n");
    {
        popup::Reset();
        Check(!popup::IsOpen(L"nav.crumb.3"), "未打开时 IsOpen 为假");

        popup::NoteOpened(L"nav.crumb.3");
        Check(popup::IsOpen(L"nav.crumb.3"), "打开后 IsOpen 为真");
        Check(popup::IsOpen(L"nav.crumb.3"), "连续查询仍为真（查询无副作用，不是一次性消费）");

        popup::NoteClosed(L"nav.crumb.3");
        Check(!popup::IsOpen(L"nav.crumb.3"), "关闭后 IsOpen 为假");
        // 关键：关掉之后必须能立刻重开 —— 旧的时间窗版本在这里会"吃掉第一下点击"
        Check(!popup::IsOpen(L"nav.crumb.3"), "关闭后不会残留任何阻塞状态");
    }

    // 2. 不同锚点互不影响（下标必须进键）
    std::printf("\n不同锚点\n");
    {
        popup::Reset();
        popup::NoteOpened(L"nav.crumb.3");
        Check(popup::IsOpen(L"nav.crumb.3"), "第 3 段开着");
        Check(!popup::IsOpen(L"nav.crumb.5"), "第 5 段没开（下标进了键，不是同一个锚点）");
        Check(!popup::IsOpen(L"nav.drop"), "地址框下拉是另一个锚点");
    }

    // 3. 注销只清自己那一个：嵌套菜单（标签右键 → 颜色二级）不能互相误清
    std::printf("\n注销的作用域\n");
    {
        popup::Reset();
        popup::NoteOpened(L"tab.color.1");   // 内层登记（外层此时已被覆盖，见下）
        popup::NoteClosed(L"tab.menu.1");    // 外层注销一个不同的键
        Check(popup::IsOpen(L"tab.color.1"), "注销异键不影响当前记录");

        popup::NoteClosed(L"tab.color.1");
        Check(!popup::IsOpen(L"tab.color.1"), "注销自己那一键才清掉");
    }

    // 4. 后开的覆盖先开的（弹层是模态的，同时只有一个）
    std::printf("\n同时只有一个\n");
    {
        popup::Reset();
        popup::NoteOpened(L"tab.menu.1");
        popup::NoteOpened(L"tab.color.1");   // 二级菜单接管
        Check(!popup::IsOpen(L"tab.menu.1"), "被接管后外层不再是'开着'");
        Check(popup::IsOpen(L"tab.color.1"), "内层才是当前开着的");
        Check(popup::OpenKey() == L"tab.color.1", "OpenKey 回填当前那一个");
    }

    // 5. 空键不参与（一次性命令面板没有"同一个锚点"）
    std::printf("\n空键\n");
    {
        popup::Reset();
        popup::NoteOpened(nullptr);
        popup::NoteOpened(L"");
        Check(!popup::IsOpen(nullptr), "nullptr 恒为假");
        Check(!popup::IsOpen(L""), "空串恒为假");
        Check(popup::OpenKey().empty(), "空键不落记录");
    }

    // 6. 注销空键视为无条件清空（RunModal 收尾用；防止异常路径留下僵尸状态）
    std::printf("\n无条件清空\n");
    {
        popup::Reset();
        popup::NoteOpened(L"nav.view");
        popup::NoteClosed(nullptr);
        Check(!popup::IsOpen(L"nav.view"), "注销空键 = 无条件清空");
    }

    // 7. 失活判据：这是"每次点击都重新弹出"的真因，也是本文件最重要的一组。
    //
    //    弹层失活时必须区分两种情况（判据见 popup_toggle.h）：
    //      · 点的是本程序另一个窗口（同线程前景窗 + 我们持有捕获）
    //            → **不能关**。这一组 DOWN/UP 正被捕获吞着，提前关会让 UP
    //              落到锚点控件上，锚点再调一次 Show* → 菜单关了又开。
    //      · 点到别的程序 / Alt+Tab / 捕获已丢
    //            → 必须关。否则就是一个关不掉的菜单。
    std::printf("\n失活判据（关了又开的真因）\n");
    {
        // 满载条件：持捕获 + 前景窗同线程 = 点本程序自己的书签栏 → 不关
        Check(!popup::ShouldCloseOnDeactivate(/*holdsCapture=*/true, /*sameThread=*/true),
              "点本程序另一窗口（持捕获+同线程）→ 不关，等 UP 判定");

        // 点到别的程序：前景窗不同线程 → 关
        Check(popup::ShouldCloseOnDeactivate(/*holdsCapture=*/true, /*sameThread=*/false),
              "点到别的程序（异线程）→ 关");

        // 捕获已被抢走：这一次点击收不到 UP，留着就是关不掉的菜单 → 关
        Check(popup::ShouldCloseOnDeactivate(/*holdsCapture=*/false, /*sameThread=*/true),
              "捕获已丢 → 关（否则关不掉）");

        // 两个条件都不成立：更要关
        Check(popup::ShouldCloseOnDeactivate(/*holdsCapture=*/false, /*sameThread=*/false),
              "既无捕获又异线程 → 关");

        // 这一条防的是"改判据时把条件写反"：唯一不关的组合必须精确到 (true,true)。
        int notClose = 0;
        for (int cap = 0; cap < 2; ++cap)
            for (int same = 0; same < 2; ++same)
                if (!popup::ShouldCloseOnDeactivate(cap != 0, same != 0)) ++notClose;
        Check(notClose == 1, "四种组合里只有 (持捕获, 同线程) 不关");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
