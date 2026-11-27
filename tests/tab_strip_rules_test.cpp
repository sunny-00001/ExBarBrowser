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

    // ---- tear-off 磁吸判定：垂直越过栏体（上或下）+ 阈值才算脱离 ----
    // 栏高 26、磁吸 18 → 下方脱离线 y=44，上方脱离线 y=-18。
    {
        const int h = 26, th = 18;
        Check(!TabIsTearOff(0, h, th), "栏内顶部：不脱离");
        Check(!TabIsTearOff(25, h, th), "栏内底部（未出栏）：不脱离");
        Check(!TabIsTearOff(26, h, th), "刚出栏底：仍算栏内（磁吸拉住）");
        Check(!TabIsTearOff(43, h, th), "阈值内（43 < 26+18）：仍被磁吸拉住");
        Check(TabIsTearOff(44, h, th), "恰在阈值上：脱离（>= 即算）");
        Check(TabIsTearOff(200, h, th), "远远拖到下方：脱离");
        // 负 dy（栏上方）：Chromium 的 above-or-below 语义 —— 向上也要越过阈值。
        // 阈值内的上抖仍被磁吸拉住，所以不会把"想重排却抖了一下"误判成成窗。
        Check(!TabIsTearOff(-10, h, th), "向上 10dp（阈值内）：仍拉住");
        Check(!TabIsTearOff(-17, h, th), "向上 17dp（阈值内）：仍拉住");
        Check(TabIsTearOff(-18, h, th), "向上恰在阈值：脱离（<= 即算）");
        Check(TabIsTearOff(-60, h, th), "向上拖到标题栏：脱离");
        // 磁吸关闭（threshold<=0）：任何出栏都算脱离，上方也算。
        Check(TabIsTearOff(-1, h, 0), "磁吸关闭：拖到栏上方即脱离");
        Check(TabIsTearOff(26, h, 0), "磁吸关闭：刚出栏底即脱离");
        Check(!TabIsTearOff(25, h, 0), "磁吸关闭：栏内仍不脱离");
        // 未布局（height<=0）：不判，避免拖拽在 Layout 之前误触发成窗。
        Check(!TabIsTearOff(100, 0, th), "未布局（height=0）：一律不脱离");
        Check(!TabIsTearOff(-100, 0, th), "未布局 + 向上：一律不脱离");
        Check(!TabIsTearOff(100, -5, th), "高度为负：一律不脱离");
    }

    // ---- 拖拽落点：被拖标签左缘 + 方向记忆 ----
    // drag=1（标签 B），其余 A(50)/C(250)/D(350) 的中点就是落点分界。
    {
        CheckEq(TabDropTargetInView(r, 50, 1, 1, 0), 0, "左缘在 A 中点：插到最前");
        CheckEq(TabDropTargetInView(r, 100, 1, 1, 0), 1, "左缘在 B 原位：原地不动");
        CheckEq(TabDropTargetInView(r, 250, 1, 1, 0), 1, "恰在 C 中点：尚未跨过");
        CheckEq(TabDropTargetInView(r, 251, 1, 1, 0), 2, "刚过 C 中点：插到 C 后");
        CheckEq(TabDropTargetInView(r, 400, 1, 1, 0), 3, "左缘拖到末位右侧：插到最后");
        // 方向记忆：current=1，越过 250 但只到 255（<250+10）→ 不换。
        CheckEq(TabDropTargetInView(r, 255, 1, 1, 10), 1, "滞后带内：不换落点");
        CheckEq(TabDropTargetInView(r, 265, 1, 1, 10), 2, "明确越过分界+eps：更新落点");
        // 反向：B 已落到位置 2（current=2），退回 245（>250-10）不换；235 才换回。
        CheckEq(TabDropTargetInView(r, 245, 1, 2, 10), 2, "反向滞后：不换回");
        CheckEq(TabDropTargetInView(r, 235, 1, 2, 10), 1, "反向明确越界：换回");
        // eps=0 关闭滞后：越过 1px 立即更新。
        CheckEq(TabDropTargetInView(r, 251, 1, 1, 0), 2, "eps=0：越过即更新");
        // 初始一致性：末位标签停在原位（current=3）时不能自己跳格。
        CheckEq(TabDropTargetInView(r, 350, 3, 3, 4), 3, "末位标签停原位：不跳");
        // 越界：宁可返回 -1，也不猜一个落点。
        CheckEq(TabDropTargetInView(r, 100, -1, 0, 0), -1, "drag 未初始化：-1");
        CheckEq(TabDropTargetInView(r, 100, 4, 0, 0), -1, "drag 越界：-1");
        CheckEq(TabDropTargetInView({}, 100, 0, 0, 0), -1, "空几何：-1");
    }

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
