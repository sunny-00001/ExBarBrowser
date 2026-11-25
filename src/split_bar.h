// 可拖动的**布局边界**：夹在两块内容之间、用鼠标拖动改变宽窄的细条。
//
// 【服务哪些边界 —— 本类现在不只服务双栏】工程里能拖的边界有三条：
//   ① 导航窗格 | 文件区     （navPaneWidth，像素）
//   ② 窗格 0 | 窗格 1       （paneSplit，百分比）
//   ③ 文件区 | AI 面板      （aiPanelWidth，像素）
// 三者的差别只有**落点口径**（百分比 or 像素）与**夹取区间**，而"命中、悬停、
// 按下、抓取偏移、拖动去重、松手提交"这套状态机完全一样。所以本类只留一份，
// 用 Metric 区分口径 —— 复制三份状态机的话，某一处漏 ReleaseCapture 或漏
// WM_MOUSELEAVE，表现是"某条边界拖完还粘手"，而另外两条正常，很难联想到一起。
//
// 【为什么是**真窗口**，不是在父窗口的 WM_PAINT 里画一条线】
// 画在宿主上等于没有命中测试：鼠标永远落不到它身上，只能靠宿主的 WM_MOUSEMOVE
// 反推"是不是在缝附近"。而宿主里全是子窗口（Shell 视图、导航树、WebView2），
// 鼠标在它们上面时消息根本不上抛 —— 于是"鼠标靠近缝"这件事永远收不到，
// 光标也永远变不成 ↔。真窗口把命中/悬停/按下/拖动全部交给系统。
//
// 【为什么侧栏边界不额外留缝】双栏之间那 6px 缝是必要的（两栏是两个独立视图，
// 中间不属于任何一栏的像素得有归属）。侧栏与文件区之间没这个问题：交界处本来
// 就该是一条线。所以侧栏边界**压在交界上**，两侧内容一点不让 ——
// navPaneWidth=220 仍然意味着"文件区从 x=220 开始"，拖动引入前的旧配置不需要迁移。
// 见 theme.h 的 edgeHit96。
//
// 【拖动落点：百分比 or 像素】
//   · 百分比（双栏）：落盘口径是 paneSplit（20..80），像素 ↔ 百分比的换算在
//     pane_split.h —— 拖动中只改内存，松手才落盘；
//   · 像素（侧栏）：落盘口径就是宽度本身，不做任何圆整，拖到哪就是哪。
// 无论哪种，拖动中都只改内存：每移动一个像素写一次 settings.json 是纯磁盘抖动，
// 而且崩溃时留下的半截拖动值会覆盖用户上一次明确选择的比例。
#pragma once
#include "common.h"
#include "theme.h"

class SplitBar {
public:
    // 拖动落点的口径。
    //   Percent —— 给的是主栏占比（已夹到 panesplit 的上下限）；
    //   Pixels  —— 给的是边界中心的绝对位置（父窗口客户区坐标，已夹到区间）。
    //              让外部自己换算成"哪条栏多宽"：导航 = center，AI = 客户区宽 - center。
    //              不在本类里换算，是因为同一个位置在不同边界上的含义相反。
    enum class Metric { Percent, Pixels };

    // 拖动回调：value 按 Metric 解释；final=false 是拖动中（只改内存并重排），
    // true 是松开/取消（落盘）。
    using DragFn = std::function<void(int value, bool final)>;

    SplitBar() = default;
    ~SplitBar() { Destroy(); }
    SplitBar(const SplitBar&) = delete;
    SplitBar& operator=(const SplitBar&) = delete;

    // cls 是窗口类名（identity.h 的唯一来源）：双栏沿用旧类名（外部契约），
    // 两条侧栏用新类名，避免按旧类名找分割条的脚本拿到三条中的任意一条。
    bool Create(HWND parent, const Theme& theme, const wchar_t* cls);
    void Destroy();
    void ReloadTheme();
    void SetDragFn(DragFn fn) { dragFn_ = std::move(fn); }

    // ---- 摆位 ----
    // 百分比口径（双栏）：rc 是 parent 客户区里的目标矩形（含整条缝宽）。
    //
    // vertical=true 表示上下堆叠（分割线横向，拖它改高度）。
    // avail / origin 是**窗格区**主轴长度与主轴起点（parent 客户区坐标）：
    // 拖动时要把鼠标位置换算成"主栏占比"，而这需要整段窗格区的几何 ——
    // 边界条自己只有一小段，反推不出来。由 App::Layout() 一次给全，避免两边
    // 各算一遍主轴（那正是"左右能拖、上下拖不动"的成因）。
    // center 是分割线中心的**绝对**坐标（parent 客户区）：按下时用它算抓取偏移。
    void SetGeometry(const RECT& rc, bool vertical, int avail, int origin, int center);

    // 像素口径（侧栏）：center 是边界中心的绝对坐标，[loCenter, hiCenter] 是
    // 允许拖到的区间（由 side_layout 的求解结果给出，夹取规则只有那一个来源）。
    //
    // 【为什么区间由外部给】"导航最窄 120、文件区至少剩 160"是布局规则，
    // 不是控件规则；写在控件里就会出现两份（布局一份、拖动一份），拖到边界
    // 松手后再被布局夹回来 —— 那条"松手跳一截"的老毛病。
    void SetPixelGeometry(const RECT& rc, bool vertical, int center, int loCenter, int hiCenter);

    void SetVisible(bool visible);

    HWND hwnd() const { return hwnd_; }
    bool ok() const { return hwnd_ != nullptr; }
    Metric metric() const { return metric_; }
    // 拖动中？供宿主在重排时判断"这次几何变化是不是自己造成的"（当前未用，
    // 但调试与单测需要读它，且比暴露内部字段干净）。
    bool dragging() const { return dragging_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Paint(HWND);
    void SetHover(bool on);
    void NoteDrag(const POINT& screenPt, bool final);
    int GapWidth() const;   // 缝宽（沿分割方向的法向）
    HCURSOR ResizeCursor() const;
    void SyncWindow();      // 把 rc_ 落到窗口上（含"腾出来的那块"重绘）

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    const Theme* theme_ = nullptr;
    RECT rc_{};
    bool vertical_ = false;
    Metric metric_ = Metric::Percent;

    // ---- 百分比口径 ----
    int avail_ = 0;
    int origin_ = 0;
    // 分割线中心的绝对坐标（parent 客户区）。由 Layout 给，按下时算抓取偏移用。
    int center_ = 0;

    // ---- 像素口径 ----
    int loCenter_ = 0;
    int hiCenter_ = 0;

    bool hover_ = false;
    bool tracking_ = false;
    bool dragging_ = false;
    // 上一次提交给外部的落点。拖动中去重：像素位置连续，但落点可能是 1 度/1px
    // 的整数，同一步内重复重排只会让两侧内容白重排一次（每移动 1px 一次，
    // 一次拖动就是几百次整块重绘 —— 那正是"拖起来一顿一顿"的成因）。
    int lastValue_ = INT_MIN;
    // 上一次拖动落点（屏幕坐标）：捕获被系统抢走时用它补一次 final 提交。
    POINT lastScreen_{};
    // 按下点相对**边界中心**的偏移（沿主轴，父窗口客户区坐标）。
    //
    // 【为什么必须有】不加它的话，抓到缝的边缘按下，第一帧就把线拽到鼠标位置 ——
    // 线从"你抓的地方"瞬间跳半条缝宽。单次 3px 看着无害，但拖着拖着一路偏移，
    // 用户的手感和线位置对不上。记下偏移后，线的相对位置全程保持不变。
    int dragOffset_ = 0;
    DragFn dragFn_;
};