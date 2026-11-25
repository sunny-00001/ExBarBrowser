// 书签栏横向滚动的纯规则自测（bookmark_bar.h 的 inline 判据）。
//
// 【为什么必须把判据抽成纯函数来测】R21 输入纪律禁止合成滚轮消息
// （PostMessage/SendMessage 发 WM_MOUSEWHEEL 一律不允许），所以
// "滚轮能不能把放不下的书签滚出来"这件事**没法用脚本自动验证** ——
// 能自动验证的只有支撑它的几何判据。把它们放在头文件里，这里逐条钉死：
//   · 可见性按"与可视窗口求交"判，不是按"矩形非空"判（那是折叠语义的老判据）
//   · 跨过裁切线的项不能命中 —— 否则点 » 变成点它，溢出菜单永远打不开
//   · 滚动量必须夹在 [0, maxScroll]，滚到头不能滑出内容
//   · 滚轮方向与滚动方向的口径（向上滚 = 看更早的书签，与标签栏一致）
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/bookmark_bar_scroll_test.cpp -o /tmp/bmk_scroll_test.exe \
//     -lole32 -luuid -lshell32 -lshlwapi -luser32 -lgdi32 -lcomctl32 -ldwmapi -luxtheme
#include <cstdio>

#include "bookmark_bar.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void CheckEq(int got, int want, const char* what) {
    const bool ok = got == want;
    std::printf("  %s  %s（得到 %d，期望 %d）\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}

// 栏可视窗口：左缘 0，右缘（裁切线）250 —— » 按钮画在 250 右边。
constexpr int kViewRight = 250;

}  // namespace

int main() {
    std::printf("书签栏横向滚动：可见性 / 命中 / 滚动量 / 滚轮方向\n");

    // ---- 可见性：与可视窗口求交 ----
    Check(BmkInView(RECT{10, 0, 100, 20}, kViewRight), "整格在窗口内：可见");
    Check(BmkInView(RECT{200, 0, 300, 20}, kViewRight), "跨过裁切线：露出一半，算可见");
    Check(!BmkInView(RECT{-40, 0, -10, 20}, kViewRight), "整格滚到左边外：不可见");
    Check(!BmkInView(RECT{260, 0, 400, 20}, kViewRight), "整格在裁切线右侧：不可见");
    Check(!BmkInView(RECT{0, 0, 0, 20}, kViewRight), "零宽矩形：不可见");

    // ---- 完全可见（» 菜单只列"看不全"的）----
    Check(BmkFullyInView(RECT{10, 0, 100, 20}, kViewRight), "整格在窗口内：完全可见");
    Check(!BmkFullyInView(RECT{200, 0, 300, 20}, kViewRight), "跨过裁切线：看不全");
    Check(!BmkFullyInView(RECT{-40, 0, 100, 20}, kViewRight), "左边被切：看不全");

    // ---- 命中：裁切线右侧是 » 按钮，不属于任何书签 ----
    Check(BmkHitInView(0, kViewRight), "最左边仍可命中");
    Check(BmkHitInView(kViewRight - 1, kViewRight), "裁切线左侧仍可命中");
    Check(!BmkHitInView(kViewRight, kViewRight), "恰在裁切线上：属于 » 按钮");
    Check(!BmkHitInView(kViewRight + 30, kViewRight), "» 按钮区域内不属于任何书签");
    Check(!BmkHitInView(-1, kViewRight), "栏外不命中");

    // ---- 滚动量夹紧 ----
    CheckEq(BmkClampScroll(100, 50, 120), 120, "往前滚到头：停在上限");
    CheckEq(BmkClampScroll(100, -50, 120), 50, "往回滚");
    CheckEq(BmkClampScroll(0, -50, 120), 0, "往回滚到头：停在 0");
    CheckEq(BmkClampScroll(100, 50, 0), 0, "没有可滚内容时不动");
    CheckEq(BmkClampScroll(30, 0, 120), 30, "位移 0：原地不动");

    // ---- 让某一项露出来需要的位移 ----
    CheckEq(BmkScrollToShow(RECT{10, 0, 100, 20}, kViewRight), 0, "已在窗口内：不用滚");
    CheckEq(BmkScrollToShow(RECT{-30, 0, 100, 20}, kViewRight), -30, "露在左边：往回滚 30");
    CheckEq(BmkScrollToShow(RECT{200, 0, 400, 20}, kViewRight), 150, "露在右边：往前滚 150");

    // ---- 滚轮方向 → 滚动方向 ----
    CheckEq(BmkWheelScroll(120, 48, false), -48, "垂直轮向上滚：看更早的书签");
    CheckEq(BmkWheelScroll(-120, 48, false), 48, "垂直轮向下滚：看更晚的书签");
    CheckEq(BmkWheelScroll(120, 48, true), 48, "横向轮向右倾：看更晚的书签");
    CheckEq(BmkWheelScroll(-120, 48, true), -48, "横向轮向左倾：看更早的书签");

    // ---- 绘制裁剪：右缘不能糊到 » 按钮上 ----
    const RECT clipped = BmkClipRight(RECT{200, 0, 300, 20}, kViewRight);
    CheckEq(clipped.right, kViewRight, "右缘裁到裁切线");
    CheckEq(clipped.left, 200, "左缘不动（x<0 的部分由栏窗口自己裁）");
    const RECT inside = BmkClipRight(RECT{10, 0, 100, 20}, kViewRight);
    CheckEq(inside.right, 100, "窗口内的矩形不动");

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
