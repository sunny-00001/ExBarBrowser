// 侧栏边界的纯几何单测：导航 | 文件区 | AI 面板三条边界中，
// 两条侧栏的宽度求解与抓取条位置（AGENTS.md §1.1 的第一优先验证路径）。
//
// 这里钉住的是"窗口变窄时谁让位"的产品规则：AI 先收、导航后收，
// 文件区永远守住最小宽度；边界中心落在侧栏外沿，
// 所以 navPaneWidth=220 就真的是"文件区从 x=220 开始"（旧配置无需迁移）。
#include <cstdio>

#include "side_layout.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("侧栏边界宽度求解 / 抓取条几何\n");
    using namespace sidelayout;

    // ---- 宽窗口：两边都放得下，宽度就是配置值，一分不差 ----
    {
        int nav = 0, ai = 0;
        SolveWidths(1400, 220, 420, &nav, &ai);
        Check(nav == 220 && ai == 420, "宽窗口：导航 220、AI 420 原样落地");
        const Layout l = Solve(1400, 220, 420, 8);
        Check(l.fileLo == 220 && l.fileHi == 980, "文件区 = 两侧栏之间（220..980）");
        Check(l.nav.center == 220 && l.ai.center == 980, "边界中心落在侧栏外沿");
        Check(l.nav.barLo <= l.nav.center && l.nav.center <= l.nav.barHi,
              "导航抓取条覆盖边界中心");
        Check(l.ai.barLo <= l.ai.center && l.ai.center <= l.ai.barHi,
              "AI 抓取条覆盖边界中心");
    }

    // ---- 窗口变窄：AI 先整块收起，导航留下，文件区守住最小宽度 ----
    {
        int nav = 0, ai = 0;
        SolveWidths(760, 220, 420, &nav, &ai);
        Check(nav == 220 && ai == 0, "放不下时先收 AI，导航保留");
        Check(760 - nav - ai >= kMinFileAreaPx, "文件区 >= kMinFileAreaPx");
    }

    // ---- 更窄：导航也放不下时两块一起收，文件区独占 ----
    {
        int nav = 0, ai = 0;
        SolveWidths(500, 600, 900, &nav, &ai);
        Check(nav == 0 && ai == 0, "极窄窗口：两侧栏都收起");
        const Layout l = Solve(500, 600, 900, 8);
        Check(l.fileLo == 0 && l.fileHi == 500, "文件区占满客户区");
    }

    // ---- 关掉某条栏（期望宽 = 0）：对面不受影响 ----
    {
        int nav = 0, ai = 0;
        SolveWidths(1200, 0, 500, &nav, &ai);
        Check(nav == 0 && ai == 500, "导航收起不影响 AI 宽度");
    }

    // ---- 落盘/界面同一份上下限：超范围输入被夹住 ----
    {
        int nav = 0, ai = 0;
        SolveWidths(2000, 9999, 1, &nav, &ai);
        Check(nav == kNavPaneWidthMax, "导航超上限被夹到 600");
        Check(ai == kAiPanelWidthMin, "AI 超下限被夹到 240");
    }

    std::printf("%s\n", g_fail ? "侧栏几何：FAIL" : "侧栏几何：全部通过");
    return g_fail;
}
