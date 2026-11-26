// 标签栏几何/命中的纯规则自测（tab_strip.h 的两个 inline 判定）。
//
// 断言的全部来自这一轮真实修掉的缺陷，不是"函数能跑"：
//   · 拖拽指示线必须按拖动方向落在目标格左/右边 —— 一律画左边会让人误判
//     "插到前面还是后面"，那正是整格高亮那种指示方式的歧义来源
//   · 跨过裁切线的标签不能在 » 按钮下面命中 —— 否则溢出菜单永远打不开
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     -include-pch obj/common.h.gch tests/tab_strip_rules_test.cpp \
//     -o /tmp/tab_strip_rules_test.exe
// 或（无 PCH 时）直接 g++ 并让 common.h 自己引入 windows.h：
//   g++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/tab_strip_rules_test.cpp -o /tmp/tab_strip_rules_test.exe \
//     -lole32 -luuid -lshell32 -lshlwapi -luser32 -lgdi32 -lcomctl32 -ldwmapi -luxtheme
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "tab_strip.h"

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

// 4 个等宽标签：0..99 / 100..199 / 200..299 / 300..399
std::vector<RECT> Rects() {
    return {RECT{0, 0, 100, 26}, RECT{100, 0, 200, 26}, RECT{200, 0, 300, 26},
            RECT{300, 0, 400, 26}};
}

// 新建按钮布局：栏 [0,1000]，margin=6，plusW=28 → 按钮左缘可落在 [6, 966]。
constexpr int kBar = 1000, kMargin = 6, kPlusW = 28;

}  // namespace

int main() {
    std::printf("标签栏拖拽指示线 / 裁切命中\n");

    const std::vector<RECT> r = Rects();

    // ---- 拖拽指示线 ----
    CheckEq(TabDropLineX(r, 1, 3, true), 400, "往右拖到末位：线在目标格右边");
    CheckEq(TabDropLineX(r, 3, 1, true), 100, "往左拖：线在目标格左边");
    CheckEq(TabDropLineX(r, 0, 2, true), 300, "从首个往右拖两格：线在目标格右边");
    CheckEq(TabDropLineX(r, 2, 0, true), 0, "拖到最前：线在首格左边");
    CheckEq(TabDropLineX(r, 1, 1, true), -1, "原地（drop==drag）不画线");
    CheckEq(TabDropLineX(r, 1, 2, false), -1, "没在拖拽时不画线");
    CheckEq(TabDropLineX(r, 1, 4, true), -1, "drop 越界不画（不猜一个位置）");
    CheckEq(TabDropLineX(r, 1, -1, true), -1, "drop 为 -1 不画");
    CheckEq(TabDropLineX(r, -1, 2, true), -1, "drag 未初始化不画");
    CheckEq(TabDropLineX(std::vector<RECT>{}, 0, 0, true), -1, "空几何不画");

    // ---- 裁切命中 ----
    // » 按钮左边界（裁切线）在 250：标签 2 跨过它（200..299）。
    const int clip = 250;
    Check(TabHitInView(240, clip), "裁切线左侧仍属于标签");
    Check(!TabHitInView(250, clip), "恰在裁切线上不属于标签");
    Check(!TabHitInView(260, clip), "» 按钮区域内不属于任何标签");
    Check(TabHitInView(0, 32767), "无溢出时全宽都可命中");

    // ---- 新建按钮位置：紧跟标签右侧，夹逼在栏内 ----
    // 无标签：贴左边距（过去钉在最右 966，用户要点到栏尾）
    CheckEq(TabNewButtonX(kMargin, kMargin, kBar - kMargin - kPlusW), kMargin,
            "空栏：+ 贴左边距");
    // 3 个标签铺到 400：+ 紧跟其后（400 + gap 由调用方加好，这里传 404）
    CheckEq(TabNewButtonX(404, kMargin, kBar - kMargin - kPlusW), 404,
            "标签未铺满：+ 紧跟最后一个标签");
    // 内容铺到右边界之外（溢出被滚走）：夹到可视右缘，不能被带到栏外
    CheckEq(TabNewButtonX(1200, kMargin, 900), 900,
            "溢出：+ 夹到可视右缘，留在看得见的地方");
    // 标签比 margin 还靠左（滚动态）：不能让 + 跟着跑到负数
    CheckEq(TabNewButtonX(-80, kMargin, 900), kMargin,
            "标签滚到栏外：+ 不越左边距");
    // 窄栏：maxX < minX 时贴 minX，至少不越界
    CheckEq(TabNewButtonX(50, 6, 4), 6, "栏窄到放不下：+ 贴左而不是负坐标");

    // ---- 鼠标目标优先级：DOWN/UP/DBLCLK 必须共用同一口径 ----
    CheckEq(TabMouseTarget(false, false, false, false), MT_EMPTY, "空白 -> empty");
    CheckEq(TabMouseTarget(false, false, true, false), MT_NEW_TAB,
            "建按钮 -> new_tab（DOWN 也必须认得它）");
    CheckEq(TabMouseTarget(false, false, false, true), MT_OVERFLOW,
            "溢出按钮 -> overflow（DOWN 也必须认得它）");
    CheckEq(TabMouseTarget(false, true, true, true), MT_TAB, "标签正文优先于 +/» ");
    CheckEq(TabMouseTarget(true, true, true, true), MT_TAB_CLOSE, "关闭钮优先于正文");

    // ---- 让位动画的缓动：起点、终点、单调收窄 ----
    // ease-out = dx·(1-t)³：t=0 停在起点，t=1 精确归零，中途单调靠近 0。
    CheckEq(TabAnimDx(-140, 0.0), -140, "t=0：停在起点（不提前动）");
    CheckEq(TabAnimDx(-140, 1.0), 0, "t=1：精确就位（不留 1px 残差）");
    CheckEq(TabAnimDx(-140, -0.5), -140, "t<0 夹到起点");
    CheckEq(TabAnimDx(-140, 1.5), 0, "t>1 夹到终点");
    CheckEq(TabAnimDx(0, 0.5), 0, "无位移：任何时刻都返回 0");
    // dx=-140、t=0.5 → -140×0.125 = -17.5，向零取整 -17。
    CheckEq(TabAnimDx(-140, 0.5), -17, "半程只剩约 1/8（三次 ease-out）");
    // 单调性：同一 dx 下，时间越往后，剩余偏移的绝对值越小。
    {
        int prev = TabAnimDx(-200, 0.1);
        bool monotonic = true;
        for (int step = 2; step <= 9; ++step) {
            const int cur = TabAnimDx(-200, step / 10.0);
            if (std::abs(cur) > std::abs(prev)) monotonic = false;
            prev = cur;
        }
        Check(monotonic, "10%..90% 区间剩余偏移单调收窄");
    }

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
