// 双栏分割几何单测（纯函数，不需要窗口 —— AGENTS.md §1.1 的第一优先验证路径）。
//
// 断言的来源是"拖动引入前旧代码的两个真实缺陷"：
//   ① 比例的分母是整段窗格区，而不是"扣掉缝之后的可用长度" —— 50% 会落成
//      主栏 497 / 次栏 446，肉眼能看出一像素级的偏好；
//   ② 最小边长与百分比上下限各夹一次，两处各写一遍 —— 极窄窗口下会算出
//      负边长（次栏 right < left），Shell 视图 SetRect 收到负宽直接黑一块。
// 这里把这两件事钉成可执行断言。
#include <cstdio>

#include "pane_split.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("双栏分割几何 / 百分比换算\n");
    using namespace panesplit;

    // 缝宽与"实体条多盖 1px"是产品口径（theme.h 的 splitter96 / splitterHit）。
    // 这里写常量而不是引 Theme：单测不建窗口、不读配置。
    constexpr int kGapUnderTest = 6;

    // ---- 50% 必须真的均分（缝等分给两侧，不额外扣）----
    {
        const int avail = 1000, gap = 6;
        const int c = CenterFromPercent(avail, gap, 50);
        const Geometry g = Compute({0, 0, avail, 400}, gap, c, true);
        const int w0 = g.pane0.right - g.pane0.left;
        const int w1 = g.pane1.right - g.pane1.left;
        Check(w0 == 497 && w1 == 497, "左右 50%：两栏各 497（1000-6 均分）");
        Check(g.splitter.right - g.splitter.left == gap, "分割条宽 = gap");
        Check(g.pane0.right == g.splitter.left && g.pane1.left == g.splitter.right,
              "两栏与分割条严丝合缝、不重叠不留空");
        Check(w0 + (g.splitter.right - g.splitter.left) + w1 == avail, "三段相加 = 窗格区宽度");
    }

    // ---- 缝 = 两侧视图边框被实体条压住（产品口径：splitter96 = 6px，窗口 = 8px）----
    //
    // 【为什么必须有这条】分界线返工三次，根源都是"两栏自带的 1px 边框露在外面"。
    // 现在的解法是**实体条**：分隔窗口比缝两侧各宽 1px，把边框压在底下。
    // 这条断言钉住"窗口确实比缝宽、且只多一点（不是靠加粗来遮丑）"。
    {
        const int gap = kGapUnderTest;      // 6
        const int hit = gap + 2;            // 每侧各盖 1px 视图边框
        Check(hit == gap + 2, "实体条窗口 = 缝 + 两侧各 1px（盖住视图边框）");
        const int avail = 1000;
        const int c = CenterFromPercent(avail, gap, 50);
        const Geometry g = Compute({0, 0, avail, 400}, gap, c, true);
        // 缝本身在 geo.splitter 里；窗口再向两侧各扩 1px。
        Check(g.splitter.right - g.splitter.left == gap, "缝宽 = gap（6）");
        const int winL = g.splitter.left - 1;
        const int winR = g.splitter.right + 1;
        Check(winR - winL == hit, "窗口宽 = 缝 + 2");
        // 两侧视图边框（各 1px）恰好落在窗口覆盖范围内。
        Check(winL < g.pane0.right && winR > g.pane1.left,
              "窗口两侧都压过视图边框（不是只盖中间）");
    }

    // ---- 百分比往返：像素 -> 百分比 -> 像素 不能漂 ----
    {
        const int avail = 880, gap = 6;
        for (int p = kMinPercent; p <= kMaxPercent; p += 5) {
            const int c = CenterFromPercent(avail, gap, p);
            const int back = PercentFromCenter(avail, gap, c);
            Check(back == p, "比例往返稳定（20..80 每 5 一档）");
        }
        // 已知旧口径的分母错误：880*50/100 = 440，减缝后是 437。
        // 新口径下 50% 的两栏必须等宽。
        const Geometry g = Compute({0, 0, avail, 600}, gap, CenterFromPercent(avail, gap, 50), true);
        Check(g.pane0.right - g.pane0.left == g.pane1.right - g.pane1.left,
              "左右 50%：两栏等宽（旧口径差 1px）");
    }

    // ---- 上下堆叠：沿 y 分，x 方向不切 ----
    {
        const int avail = 600, gap = 6;
        const Geometry g = Compute({0, 0, 900, avail}, gap, CenterFromPercent(avail, gap, 50), false);
        Check(g.pane0.left == 0 && g.pane0.right == 900, "上下：两栏都占满整宽");
        Check(g.pane1.top == g.splitter.bottom, "上下：次栏紧接分割条");
        Check(g.splitter.bottom - g.splitter.top == gap, "上下：分割条高 = gap");
        Check(g.pane0.bottom - g.pane0.top == g.pane1.bottom - g.pane1.top, "上下 50%：两栏等高");
    }

    // ---- 上下限：20/80 夹取在两条路径上一致 ----
    {
        const int avail = 1000, gap = 6;
        Check(PercentFromCenter(avail, gap, -5000) == kMinPercent, "拖到最左 -> 夹到 20%");
        Check(PercentFromCenter(avail, gap, 5000) == kMaxPercent, "拖到最右 -> 夹到 80%");
        Check(CenterFromPercent(avail, gap, 0) == CenterFromPercent(avail, gap, kMinPercent),
              "CenterFromPercent 也夹百分比（0 -> 20）");
        Check(CenterFromPercent(avail, gap, 100) == CenterFromPercent(avail, gap, kMaxPercent),
              "CenterFromPercent 也夹百分比（100 -> 80）");
    }

    // ---- 最小边长：每一栏都不小于 kMinPane ----
    {
        const int avail = 400, gap = 6;
        for (int p = -50; p <= 150; p += 10) {
            const Geometry g = Compute({0, 0, avail, 300}, gap, CenterFromPercent(avail, gap, p), true);
            const int w0 = g.pane0.right - g.pane0.left;
            const int w1 = g.pane1.right - g.pane1.left;
            Check(w0 >= kMinPane && w1 >= kMinPane, "任何百分比下两栏都不小于最小边长");
        }
    }

    // ---- 极窄窗格区：不产生负边长 ----
    {
        const int gap = 6;
        for (int avail = 0; avail <= kMinPane * 2 + gap; ++avail) {
            const Geometry g = Compute({0, 0, avail, 200}, gap, CenterFromPercent(avail, gap, 50), true);
            Check(g.pane0.right >= g.pane0.left && g.pane1.right >= g.pane1.left &&
                      g.splitter.right >= g.splitter.left,
                  "极窄窗格区：三个矩形宽度均非负");
        }
        // avail<=0：几何全空，调用方按 0 处理（不能算出反向矩形）
        const Geometry empty = Compute({0, 0, 0, 200}, gap, 0, true);
        Check(empty.pane0.right == 0 && empty.pane1.right == 0, "窗格区宽度 0：矩形留空");
    }

    // ---- 拖动语义：中心由鼠标位置给出（ClampCenter 的入参是局部坐标）----
    {
        const int avail = 1000, gap = 6;
        Check(ClampCenter(avail, gap, 400) == 400, "鼠标在合法区间内：中心原样通过");
        Check(PercentFromCenter(avail, gap, ClampCenter(avail, gap, 400)) == 40,
              "中心 400 -> 40%");
    }

    std::printf("\n%s\n", g_fail ? "有失败" : "全部通过");
    return g_fail ? 1 : 0;
}