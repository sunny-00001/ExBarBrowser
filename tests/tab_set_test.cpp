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

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
