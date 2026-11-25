// 侧栏边界的宽度求解（原理与取舍见 side_layout.h）。
#include "side_layout.h"

#include <algorithm>

namespace sidelayout {

void SolveWidths(int totalW, int navWant, int aiWant, int* navOut, int* aiOut) {
    int nav = navWant > 0 ? std::min(navWant, kNavPaneWidthMax) : 0;
    int ai = aiWant > 0 ? std::min(aiWant, kAiPanelWidthMax) : 0;
    if (totalW <= 0) totalW = 0;

    // 【先按"都放得下"算一次】两条侧栏都夹在各自上下限里，再看文件区还剩多少。
    // 上下限必须与落盘口径同一份（layout_limits.h），否则拖到头、存下去，
    // 重启后被夹回一个更小的值 —— 用户会以为"宽度没保存住"。
    if (nav > 0) nav = std::max(nav, kNavPaneWidthMin);
    if (ai > 0) ai = std::max(ai, kAiPanelWidthMin);

    // 【让位顺序：AI 先让，导航后让】见 side_layout.h 的说明。文件区是主体，
    // 它必须保住 kMinFileAreaPx；保不住就整块收掉最外侧的那条栏。
    if (ai > 0 && totalW - nav - ai < kMinFileAreaPx) ai = 0;
    if (nav > 0 && totalW - nav - ai < kMinFileAreaPx) nav = 0;

    // 【收掉外侧之后再把剩下的那条压到"刚好留住文件区"】只收不压的话，
    // 900px 窗口上"导航 220 + 面板 420"仍可能差几像素 —— 而用户看不到差别，
    // 只觉得文件区比上次窄了一点点。
    if (nav > 0) nav = std::min(nav, totalW - ai - kMinFileAreaPx);
    if (ai > 0) ai = std::min(ai, totalW - nav - kMinFileAreaPx);
    // 压到下限以下就不再显示：留一条 20px 的残条比没有更难看，也没法用。
    if (nav > 0 && nav < kNavPaneWidthMin) nav = 0;
    if (ai > 0 && ai < kAiPanelWidthMin) ai = 0;

    *navOut = nav;
    *aiOut = ai;
}

Layout Solve(int totalW, int navWant, int aiWant, int barW) {
    Layout out;
    SolveWidths(totalW, navWant, aiWant, &out.navW, &out.aiW);

    // 文件区 = 两条侧栏之间剩下的全部。边界中心落在侧栏的外沿上 ——
    // 也就是说 navPaneWidth=220 就真的是"文件区从 220 开始"，
    // 抓手压在 220 附近 ±barW/2，不额外吃掉文件区。
    out.fileLo = out.navW;
    out.fileHi = totalW - out.aiW;

    if (barW < 1) barW = 1;
    const int half = barW / 2;

    if (out.navW > 0) {
        out.nav.center = out.navW;
        out.nav.barLo = out.navW - half;
        out.nav.barHi = out.nav.barLo + barW;
    }
    if (out.aiW > 0) {
        out.ai.center = totalW - out.aiW;
        out.ai.barLo = out.ai.center - half;
        out.ai.barHi = out.ai.barLo + barW;
    }
    return out;
}

}  // namespace sidelayout