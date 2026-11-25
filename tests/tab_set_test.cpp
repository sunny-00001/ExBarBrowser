// 标签模型自测（core::TabSet 的改名/颜色/去重）。
//
// 断言的都踩过的坑：
//   · 改名存 alias 而不是 title —— 否则下次导航被 Shell 名冲掉
//   · 颜色下标越界必须夹回 0 —— 否则绘制走空色板（标签颜色莫名消失）
//   · 重复判定大小写不敏感 —— Windows 路径语义，且 Shell 回调会给不同大小写
//   · DuplicatesOf 不能把自己算进去 —— 否则"关闭重复"会先关掉本尊
//
// 编译（在 ExBarBrowser/ 下）：
//   g++ -std=c++20 -O1 -DUNICODE -D_UNICODE -Isrc \
//     tests/tab_set_test.cpp src/tab_set.cpp src/str.cpp -o /tmp/tab_set_test.exe
#include <cstdio>
#include <string>

#include "tab_set.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    using core::TabSet;
    using core::TabRef;

    std::printf("TabSet 改名 / 颜色 / 去重\n");

    TabSet ts;
    const int a = ts.Add(L"C:\\Windows", L"Windows");
    const int b = ts.Add(L"C:\\Users", L"Users");
    const int c = ts.Add(L"c:\\windows", L"Windows");  // 同路径，仅大小写不同

    // ---- 显示名三级优先 ----
    Check(core::DisplayTitle(*ts.Find(a)) == L"Windows", "未改名时用 Shell 名");

    ts.SetAlias(a, L"系统目录");
    Check(ts.Find(a)->title == L"Windows", "改名不动 title（否则会被导航冲掉）");
    Check(core::DisplayTitle(*ts.Find(a)) == L"系统目录", "改名后显示名优先用 alias");

    // 模拟一次导航回调：Shell 重写 title
    ts.SetTitle(a, L"Windows (新)");
    Check(core::DisplayTitle(*ts.Find(a)) == L"系统目录", "导航后改名仍然有效");

    // 清空 alias = 回到跟随
    ts.SetAlias(a, L"");
    Check(core::DisplayTitle(*ts.Find(a)) == L"Windows (新)", "清空 alias 回到 Shell 名");

    // ---- 颜色 ----
    ts.SetColor(b, 5);
    Check(ts.Find(b)->color == 5, "颜色下标写入");
    ts.SetColor(b, 99);
    Check(ts.Find(b)->color == 0, "越界下标夹回 0（不是保留 99）");
    ts.SetColor(b, -3);
    Check(ts.Find(b)->color == 0, "负数下标也夹回 0");

    // ---- 重复 ----
    Check(core::DisplayTitle(*ts.Find(c)) == L"Windows", "重复标签的显示名独立");
    std::vector<int> d = ts.DuplicatesOf(a);
    Check(d.size() == 1 && d[0] == c, "DuplicatesOf 命中同路径（大小写不敏感），且不含自己");

    ts.SetColor(a, 2);
    d = ts.DuplicatesOf(b);
    Check(d.empty(), "无重复的标签返回空");

    // 空路径不算重复：新建失败的标签路径是空的，两条空路径不应互相关掉。
    {
        core::TabSet e;
        e.Add(L"", L"");
        e.Add(L"", L"");
        Check(!e.HasDuplicates(), "两条空路径不算重复");
        Check(e.DuplicatesOf(e.IdAt(0)).empty(), "空路径的 DuplicatesOf 为空");
    }

    Check(ts.HasDuplicates(), "集合里有重复");
    ts.Remove(c);
    Check(!ts.HasDuplicates(), "关掉重复后不再有重复");

    // ---- 删除后颜色/改名不串位 ----
    const int d2 = ts.Add(L"D:\\Data", L"Data");
    ts.SetColor(d2, 7);
    ts.Remove(a);
    Check(ts.Find(d2)->color == 7, "删别的标签不影响本标签颜色");
    Check(ts.Count() == 2, "删除后计数正确");

    // ---- 关闭历史：按索引取走（菜单列多条 / tab.reopen.index）----
    {
        core::TabSet h;
        h.PushClosed(L"A");
        h.PushClosed(L"B");
        h.PushClosed(L"C");   // 栈：C B A（front = 最近关闭）
        std::wstring got;
        Check(h.PopClosedAt(1, &got) && got == L"B", "PopClosedAt(1) 取到 B");
        Check(h.closed().size() == 2 && h.closed()[0] == L"C" && h.closed()[1] == L"A",
              "取走中间一条后顺序不乱");
        got.clear();
        Check(!h.PopClosedAt(9, &got) && got.empty(), "越界不取走任何条目");
        Check(h.closed().size() == 2, "越界调用不改变栈大小");
        Check(h.PopClosedAt(0, &got) && got == L"C", "PopClosedAt(0) = 最近关闭");
    }

    // ---- 关闭历史上限可调（配置项 closedTabLimit）----
    //
    // 上限以前是代码里的裸 20：调大要重编译（"Ctrl+Shift+T 能退几步"是使用习惯
    // 问题，不是性能问题）。现在由外面推进来 —— 这里钉住三件事：
    //   · 默认仍是 20（旧行为不变，老用户不会突然发现历史变短）；
    //   · 收紧上限时**立刻**裁掉超出的（不是等下一次 Push 才生效）；
    //   · 上限调大之后能多存（证明它是可变的，不是只紧不松）。
    {
        core::TabSet c;
        Check(c.closedLimit() == 20, "关闭历史上限默认 20（与旧硬编码一致）");
        for (int i = 0; i < 30; ++i) c.PushClosed(L"P" + std::to_wstring(i));
        Check(c.closed().size() == 20, "默认上限下只留 20 条");
        Check(c.closed().front() == L"P29", "留下的最近一条是最后压入的");

        c.SetClosedLimit(5);
        Check(c.closedLimit() == 5, "上限可设");
        Check(c.closed().size() == 5, "收紧上限时立刻裁掉超出的条目");
        Check(c.closed().front() == L"P29", "裁剪从尾部丢，最近的仍在最前");

        c.SetClosedLimit(50);
        for (int i = 0; i < 30; ++i) c.PushClosed(L"Q" + std::to_wstring(i));
        Check(c.closed().size() == 35, "调大上限后能存超过 20 条（旧硬编码做不到）");

        // 0/负数按"不允许历史"处理，而不是"不限" —— 后者会让配置写错时无限膨胀。
        c.SetClosedLimit(-3);
        Check(c.closedLimit() == 0 && c.closed().empty(), "非法上限按 0 处理并清空");
    }

    // ---- 固定标签：固定区始终在最前，取消固定回到第一个未固定位 ----
    {
        core::TabSet p;
        p.Add(L"A", L"A");
        const int b = p.Add(L"B", L"B");
        const int c = p.Add(L"C", L"C");
        const int d = p.Add(L"D", L"D");
        (void)b;
        p.SetPinned(c, true);   // [C,A,B,D]
        Check(p.items()[0].path == L"C", "固定 C 后移到最前");
        p.SetPinned(d, true);   // [C,D,A,B]
        Check(p.items()[1].path == L"D", "第二个固定项排在固定区末尾");
        p.SetPinned(c, false);  // [D,C,A,B]：取消固定落到第一个未固定位
        Check(p.items()[0].path == L"D" && p.items()[1].path == L"C",
              "取消固定后固定区仍连续在前");
        p.SetPinned(d, false);
        Check(p.items()[0].path == L"D", "全部取消后不再重排");
    }

    // ---- MRU 循环：Ctrl+Tab 连续按要走完一圈，不是两个标签来回跳 ----
    {
        core::TabSet c;
        const int a = c.Add(L"A", L"A");
        const int b = c.Add(L"B", L"B");
        const int d = c.Add(L"C", L"C");
        c.Touch(a);
        c.Touch(b);
        c.Touch(d);   // MRU：C B A
        const int first = c.CycleMru(1);
        Check(first == b, "第一次 Ctrl+Tab 回到上一个（B）");
        c.Touch(first);   // 模拟 Activate：实时 MRU 变了，但循环快照不变
        const int second = c.CycleMru(1);
        Check(second == a, "第二次继续走到更早的（A），而不是跳回 C");
        c.Touch(second);
        Check(c.CycleMru(1) == d, "第三次走完一圈回到起点（C）");
        Check(c.CycleMru(-1) == a, "反向循环取快照上的前一项");
        c.ResetMruCycle();
        c.Touch(d);
        Check(c.CycleMru(1) == a, "重置后从当前 MRU 重新快照");
    }

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
