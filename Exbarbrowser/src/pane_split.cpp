// 双栏分割几何的实现（原理与取舍见 pane_split.h）。
#include "pane_split.h"

#include <algorithm>

namespace panesplit {

namespace {

// 分割条把主轴切成三段：主栏 | 分割条 | 次栏。
// 主栏终点 = center - half，分割条 = [center - half, center - half + gap)，
// 次栏起点 = center - half + gap。
//
// 【百分比的分母是 content，不是 avail】50% 的语义是"两栏一样宽"，
// 而不是"主栏占整个窗格区的一半"。后者会让 50% 变成 500/494 ——
// 中间那条 6px 缝从次栏里再扣一次，用户看到的就是"50% 明明不均分"。
int Half(int gap) { return gap > 0 ? gap / 2 : 0; }
int Content(int avail, int gap) { return std::max(1, avail - std::max(0, gap)); }

}  // namespace

bool Bounds(int avail, int gap, int* loOut, int* hiOut) {
    if (avail <= 0) {
        if (loOut) *loOut = 0;
        if (hiOut) *hiOut = 0;
        return false;
    }
    const int content = Content(avail, gap);
    const int half = Half(gap);
    // 两栏最小边长给出的区间（主栏 = center-half，次栏 = avail-center+half-gap）。
    const int loMin = kMinPane + half;
    const int hiMax = avail - kMinPane - gap + half;
    // 百分比给出的区间（主栏占 content 的比例 -> center = 主栏长度 + half）。
    const int loPct = content * kMinPercent / 100 + half;
    const int hiPct = (content * kMaxPercent + 50) / 100 + half;
    const int lo = std::max(loMin, loPct);
    const int hi = std::min(hiMax, hiPct);
    if (loOut) *loOut = lo;
    if (hiOut) *hiOut = hi;
    // 交集为空 = 这个尺寸下两个约束没法同时满足（极窄窗口）。
    return lo <= hi;
}

int ClampCenter(int avail, int gap, int center) {
    if (avail <= 0) return 0;
    int lo = 0, hi = 0;
    if (!Bounds(avail, gap, &lo, &hi)) return avail / 2;
    return std::clamp(center, lo, hi);
}

int CenterFromPercent(int avail, int gap, int percent) {
    if (percent < kMinPercent) percent = kMinPercent;
    if (percent > kMaxPercent) percent = kMaxPercent;
    if (avail <= 0) return 0;
    const int content = Content(avail, gap);
    // 四舍五入：整除会把 50% 落成 497/496，肉眼能看出一像素的偏。
    const int mainLen = (content * percent + 50) / 100;
    return ClampCenter(avail, gap, mainLen + Half(gap));
}

int PercentFromCenter(int avail, int gap, int center) {
    if (avail <= 0) return (kMinPercent + kMaxPercent) / 2;
    const int content = Content(avail, gap);
    const int c = ClampCenter(avail, gap, center);
    const int mainLen = c - Half(gap);
    int p = (mainLen * 100 + content / 2) / content;
    if (p < kMinPercent) p = kMinPercent;
    if (p > kMaxPercent) p = kMaxPercent;
    return p;
}

Geometry Compute(const Rc& inner, int gap, int center, bool alongX) {
    Geometry g;
    if (gap < 0) gap = 0;
    const int start = alongX ? inner.left : inner.top;
    const int avail = alongX ? (inner.right - inner.left) : (inner.bottom - inner.top);
    if (avail <= 0) return g;   // 窗格区还没尺寸：三个矩形都留空，调用方按 0 处理
    // 【缝不能比窗格区还宽】极窄窗格区（avail < gap）下缝会把两栏挤出容器：
    // 主栏终点算到 start 左侧去，SetRect 收到负宽 —— shell 视图整块黑。
    // 这种尺寸下已经没有"两栏"可言，让缝吃掉全部长度、两栏各为 0。
    if (gap > avail) gap = avail;

    const int half = Half(gap);
    const int c = start + ClampCenter(avail, gap, center - start);
    g.center = c;
    const int s0 = c - half;          // 分割条起点
    const int s1 = s0 + gap;          // 分割条终点 = 次栏起点
    if (alongX) {
        g.pane0 = {inner.left, inner.top, s0, inner.bottom};
        g.splitter = {s0, inner.top, s1, inner.bottom};
        g.pane1 = {s1, inner.top, inner.right, inner.bottom};
    } else {
        g.pane0 = {inner.left, inner.top, inner.right, s0};
        g.splitter = {inner.left, s0, inner.right, s1};
        g.pane1 = {inner.left, s1, inner.right, inner.bottom};
    }
    return g;
}

}  // namespace panesplit