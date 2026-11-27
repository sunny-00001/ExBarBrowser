// 自绘标签栏：图标 + 标题 + 关闭钮 + 锁定态 + 溢出滚动/» 菜单 + 拖拽排序。
//
// 只做"画 + 命中 + 发消息"，标签状态仍由 App 持有；顺序变化也只上报，不自己改。
#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "mouse_rules.h"
#include "tooltip_host.h"
#include "tab_set.h"
#include "theme.h"

// 逻辑像素（Theme::S 再缩放）。Layout 与窗口初建尺寸共用，避免两处各写一个数。
// 【值只有一个出处】栏高令牌在 theme.h（Theme::barTab96）。这里不写第二遍数字 ——
// 标签栏按 26 建、布局按 28 排这类 1px 错位，只有"同一个来源"才能从根上避免。
constexpr int kTabBarHeightDp = Theme::barTab96;

// 动效时长与磁吸阈值的**默认值**在 config.h 的 Settings 成员初始化器里（唯一出处），
// 可被 settings.json 覆盖；这里不再写一份数字 —— 两份默认值迟早分叉，
// 表现为"设置里写着 140，实际按 120 跑"。
// 范围（含 0 = 关闭）的唯一出处是 settings_bounds.h 的 kTabAnimMs* / kTabTearOffDy*。

// 让位动画的剩余偏移（**纯函数**，便于单测）。
// t ∈ [0,1] 是归一化时间；返回值是"相对最终位置还要偏多少"：
//   t=0 → dx（停在起点），t=1 → 0（已就位）。
// 曲线是三次 ease-out（offset = dx·(1-t)³）：起步快、收尾稳。
// 线性插值会让最后几帧"还在动却快到了"，观感反而更差。
inline int TabAnimDx(int dx, double t) {
    if (t <= 0.0) return dx;
    if (t >= 1.0) return 0;
    const double inv = 1.0 - t;
    return static_cast<int>(dx * inv * inv * inv);
}

// 拖拽插入指示线画在哪个 x（客户区坐标）。-1 = 不画。**纯函数**，便于单测。
//
// drop 的语义来自 core::TabSet::Move：先删后插，所以它是"移动完成后所在的最终下标"。
//   · 往左拖（drop < drag）：最终落在目标格**左**边 → 线画目标格左边
//   · 往右拖（drop > drag）：最终落在目标格**右**边 → 线画目标格右边
//   · 原地（drop == drag）：没有插入动作，不画
// 一律画左边会让人误判"插到前面还是后面"，那正是"整格高亮"这种指示方式原有的歧义。
inline int TabDropLineX(const std::vector<RECT>& rects, int drag, int drop, bool dragging) {
    if (!dragging) return -1;
    if (drop < 0 || drop >= static_cast<int>(rects.size())) return -1;
    if (drag < 0 || drop == drag) return -1;
    const RECT& r = rects[static_cast<size_t>(drop)];
    return drop > drag ? static_cast<int>(r.right) : static_cast<int>(r.left);
}

// x 是否落在"标签可视区"内（溢出处右侧属于 » 按钮，不属于任何标签）。**纯函数**。
//
// 判据必须按**点**判，不能只过滤标签自己的 rect：
// 一个跨过裁切线的标签（left < clipRight < right）在 » 按钮下面仍然命中，
// 于是点 » 变成切到那个被遮住的标签，溢出菜单永远打不开。标签刚好多到溢出时，
// 最后一个可见标签几乎必然跨界 —— 这不是理论边界。
inline bool TabHitInView(int x, int clipRight) { return x < clipRight; }

// 新建按钮的左边缘：紧跟最后一个标签的右缘（tabsRight），但夹在 [minX, maxX] 内。
// **纯函数**，便于单测。
//
// 为什么要有 maxX：溢出时最后一个标签可能整格滚出视野，"跟着它"会把 + 一起带走
// （用户就永远点不到新建了）。那种情况调用方传"可视区右缘"当 maxX，+ 就停在
// 看得见的最右侧；非溢出时传"栏右缘 - 按钮宽"，+ 便真正贴着标签走。
// minX/maxX 都传客户区 x；maxX < minX（栏窄到放不下）时贴 minX，至少不越界。
inline int TabNewButtonX(int tabsRight, int minX, int maxX) {
    if (maxX < minX) return minX;
    return std::min(std::max(tabsRight, minX), maxX);
}

// 标签栏命中目标优先级（**纯函数**）：关闭钮 > 正文 > + > » > 空白。
// DOWN/UP/DBLCLK 必须共用它。一旦 DOWN 报 empty、UP 报 new_tab，
// InputRouter 就会拿 DOWN 的 claim 吞掉 UP 的默认行为（点 + 没反应）。
inline int TabMouseTarget(bool onClose, bool onTab, bool onNew, bool onOverflow) {
    if (onClose) return MT_TAB_CLOSE;
    if (onTab) return MT_TAB;
    if (onNew) return MT_NEW_TAB;
    if (onOverflow) return MT_OVERFLOW;
    return MT_EMPTY;
}

// 拖拽是否已"脱离"标签栏（tear-off）。**纯函数**，便于单测。
//
// 语义来自 Chromium 的 kVerticalDetachMagnetism：垂直方向必须离开栏底超过阈值
// 才算脱离 —— 横向挪出去不算。理由：拖拽重排时手会不自觉地上下抖，若按
// "出栏即脱离"判，想换位置的人会频繁误触发"移到新窗口"（那条路径会关掉源标签，
// 代价比误排序大得多）。
//
// dy：鼠标 y 相对标签栏客户区顶部的偏移（可超出，> height 即在栏下方）。
// height：标签栏客户区高度。threshold：磁吸阈值（0 = 关闭磁吸，任何出栏都算脱离）。
inline bool TabIsTearOff(int dy, int height, int threshold) {
    if (height <= 0) return false;              // 还没布局：不判
    if (threshold <= 0) return dy < 0 || dy >= height;   // 磁吸关闭：出栏即脱离
    return dy >= height + threshold;            // 磁吸开启：要越过阈值
}

// 标签右键菜单动作（TabStrip -> App，走 WM_APP_TAB_MENU 的 lParam）
enum TabMenuAction {    kTabMenuNew = 1,
    kTabMenuSwitcher,   // 打开标签切换器（全局动作，但挂在标签菜单里便于发现）
    kTabMenuClose,
    kTabMenuCloseOther,
    kTabMenuCloseLeft,
    kTabMenuCloseRight,
    kTabMenuClone,
    kTabMenuToggleLock,
    kTabMenuTogglePin,   // 固定/取消固定（与 locked 分开）
    kTabMenuCopyPath,
    kTabMenuReopen,
    // ---- 下列为后加：同一套消息通道，动作号只增不改（C1）----
    kTabMenuRename,      // App 弹输入框；改名结果存 TabRef::alias
    kTabMenuCopyTitle,   // 复制当前显示名（含改名后的）
    kTabMenuDedup,       // 关闭与本源同路径的其他标签
    kTabMenuMoveLeft,
    kTabMenuMoveRight,
    kTabMenuMoveFirst,
    kTabMenuMoveLast,
    kTabMenuMoveNewWindow,   // 有损近似：新实例按路径打开 + 关闭源标签
    kTabMenuColor,       // 不是动作本身：TabStrip 再弹一层色板，选中行走 TabSetColor(color,index)
    // 关闭历史条目：action = kTabMenuReopenBase + 序号（0 = 最近关闭）。
    // 用基址+偏移而不是为每条加一个枚举项：栈深 20，枚举会被撑成噪声。
    kTabMenuReopenBase = 1000,
};

class TabStrip {
public:
    ~TabStrip();
    void Create(HWND parent, const Theme& theme);
    HWND hwnd() const { return hwnd_; }
    // 只吃模型：标题兜底、图标解析与缓存都在本类内完成（App 不再持有 HICON）。
    // canReopen：关闭历史栈是否非空。菜单里"重新打开"能否点由它决定，
    // 不能让一个必然空操作的菜单项亮着（与"边界要灰掉"的原则冲突）。
    // closed：关闭历史（front = 最近关闭）。菜单用它列出最近 10 条，
    // 让"重开"从盲盒变成可选项。
    void SetTabs(const std::vector<core::TabRef>& refs, int active, bool canReopen,
                 const std::vector<std::wstring>& closed = {});
    void ReloadTheme();
    // 释放图标缓存；WM_DESTROY 与析构都会调，重复调用安全。
    void ReleaseIcons();
    void SetMouseSink(MouseSink fn) { sink_ = std::move(fn); }

private:
    // 行 = 一条标签的渲染数据，由 SetTabs 从 core::TabRef 生成；icon 归本类所有。
    struct TabStripItem {
        // core::TabRef::id 的快照。**动画与浮层用 id 做身份，不用下标**：
        // 重排后下标全体位移，按下标配对会把"旧位置"配给另一个标签，
        // 动画看起来就是乱飞。路径也不行（允许同路径多标签）。
        int id = 0;
        std::wstring title;
        std::wstring path;  // tooltip 用
        bool locked = false;
        bool pinned = false;   // 固定：图标化、无关闭钮、不被批量关闭
        HICON icon = nullptr;
        // 颜色：下标回传菜单打勾用；两个 COLORREF 是算好的绘制值。
        // 存 COLORREF 而不是每帧查主题：绘制热路径不该再算一次混色。
        int colorIdx = 0;
        COLORREF colorFill = CLR_INVALID;
        COLORREF colorBar = CLR_INVALID;
        // 同路径的其他标签是否存在（= 菜单里"关闭重复"能不能点）。
        // 在 SetTabs 里一次算完：菜单弹出时不该再遍历一遍全部标签。
        bool hasDuplicate = false;
    };

    enum Hover { kNone = 0, kTab, kClose, kNew, kOverflow };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Layout();
    void Paint(HWND);
    void UpdateHover(HWND, int x, int y);
    int HitTab(int x, int y) const;
    int HitClose(int x, int y) const;
    // 关闭钮只在"激活标签 / 悬停在该标签"时绘制；命中测试必须用同一判据，
    // 否则未激活的标签右端有一块"看不见但能关"的区域（点了没反应的反例）。
    bool CloseVisible(int index) const;
    bool HitNew(int x, int y) const;
    bool HitOverflow(int x, int y) const;
    // 按下/抬起/双击共用的目标判定：三处必须用同一口径。
    // 曾经 DOWN 只算 tab/close/empty，而 UP 还算 new/overflow —— 点在 "+" 上时，
    // DOWN 被当成空白（命中 double=NavUp 规则并 claim），UP 却被当成 new_tab，
    // 于是一次单击被规则吞掉；双击时第一次 UP 没生效，最后一次 UP 又真新建了标签。
    int MouseTargetAt(int x, int y) const;
    void ShowTabMenu(int index, POINT screen);
    // 颜色二级菜单（从 ShowTabMenu 里再弹一层）。当前色打勾。
    void ShowTabColorMenu(int index, POINT screen);
    void ShowOverflowMenu(POINT screen);
    void ScrollBy(int dx);
    void EnsureVisible(int index);
    // 拖拽时鼠标落在 (x,y) 应插到第几个位置。WM_MOUSEMOVE 与边缘滚动定时器共用，
    // 两者算出的 dropIdx_ 必须是同一口径，否则滚一下插入线就跳到别处。
    int DropTargetAt(int x, int y) const;
    // 拖拽靠近可视区边缘时启停自动滚动。x 是客户区横坐标（定时器里用缓存值）。
    // step>0 向右看（scrollX 增大），step<0 向左看；0 = 停。
    void UpdateAutoScroll(int x, int step);
    void StopAutoScroll();
    // 复位拖拽中的瞬态（dragIdx_/dragging_/dropIdx_/closeDownIdx_），并按需要提供
    // 是否重绘。捕获丢失（WM_CAPTURECHANGED）与 Esc 取消都走这里 —— 两处各写一份
    // 状态清理，迟早会出现"Esc 取消后残留 dropIdx_、下次点击 jumped"的分叉。
    void CancelDrag(HWND h, bool releaseCapture);
    // 拖拽插入位置的那条竖线画在哪：返回屏幕无关的客户区 x，-1 = 不画。
    //
    // 为什么需要它：dropIdx_ 只说"插到第几个"，而拖拽时用户需要看的是
    // **插在这一侧的边界上**。只把目标标签刷成悬停底色，读起来是"这个标签被选中了"
    // ——Chrome / Edge / VS Code / Opus 一律画竖线，就是因为整格高亮有歧义。
    int DropLineX() const;
    // ---- 拖拽让位动画（实现见 tab_strip_anim.cpp）----
    // 只在"同规模重排"（拖拽提交）时启动：增删标签各有既有路径（rapid close 还要
    // 冻结宽度），把关闭/新建也做成滑动会打乱关闭钮的落点，得不偿失。
    // oldOrder 是重排前的 id 序列，oldLeft 是 id -> 旧客户区 left。
    void StartReorderAnim(const std::vector<int>& oldOrder, const std::map<int, int>& oldLeft);
    void StopReorderAnim();
    // 当前帧该下标相对最终位置的横向偏移（0 = 已就位）。
    int AnimDx(int index) const;
    // 被拖标签的浮层：跟随光标抬升绘制（实现见 tab_strip_anim.cpp）。
    // 只画，不改命中/布局；clip 是标签可视区，浮层不越过 » 按钮。
    void DrawDragGhost(HDC dc, const RECT& clip);
    void RebuildTooltip(HWND);
    // 滚动/宽度恢复时只移动热区，不重建整套工具（文本和 id 没变）。
    void UpdateTooltipRects();
    // Chromium 的 rapid closure：关掉一个标签后，右侧标签左移，使下一个关闭钮
    // 仍落在鼠标下。内容宽模式下各标签宽度不同，直接左移会点空，因此记录
    // 被关标签的宽度，让接管该位置的标签临时用它；鼠标离开标签栏即恢复。
    void CancelRapidClose();
    void NotifyClose(int index);
    void NotifySelect(int index);
    // 小图标的唯一取图点：命中缓存就复用，未命中才去 Resolve。
    HICON TabIcon(const std::wstring& path);
    // 缓存到顶时清掉本次不再展示的 path（正在用的 icon 不能销毁）。
    void PruneIcons(const std::vector<core::TabRef>& refs);

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    ui::TooltipHost tooltip_;
    const Theme* theme_ = nullptr;
    std::vector<TabStripItem> items_;
    // path -> HICON（本类拥有，ReleaseIcons 统一销毁）。上限见 tab_strip.cpp TabIcon。
    std::map<std::wstring, HICON> iconCache_;
    // 缓存自上次清理后是否新装填过条目（TabIcon 置位、SetTabs 清理后复位）：
    // 没涨也全量扫一遍 PruneIcons 是白做的 O(n·m)。
    bool iconCacheGrew_ = false;
    std::vector<RECT> tabRects_;
    std::vector<RECT> closeRects_;
    RECT newRect_{};
    RECT ovfRc_{};
    // 标签可视右缘（客户区 x）：裁切线。非溢出=栏右缘；溢出=»/+ 左侧。
    // 命中与绘制都读它，不再从 ovfRc_.left 反推 —— + 移到标签右侧后，
    // » 已经不在标签右边紧邻，从它反推会把 + 区域误算成标签区。
    int tabViewRight_ = 0;
    int active_ = -1;
    int scrollX_ = 0;
    int maxScroll_ = 0;
    bool overflow_ = false;
    // 快速关闭模式：保存关闭前每个标签的**实际绘制宽度**，关闭后原样套用到
    // 剩余标签（去掉被关索引），使下一个关闭钮落在鼠标下。鼠标离开即恢复。
    std::vector<int> lastWidths_;    // 最近一次 Layout 的实际宽度，每次重排都刷新
    std::vector<int> rapidWidths_;   // 非空 = 快速关闭模式生效
    int rapidCloseIndex_ = -1;
    // 关闭历史是否非空（来自 App 的 TabSet，决定"重新打开"菜单项是否可点）。
    bool canReopen_ = false;
    // 关闭历史快照（front = 最近关闭）。只用于菜单展示；真正的重开走
    // WM_APP_TAB_MENU，由 App 从 TabSet 里取走 —— 本类不持有业务状态。
    std::vector<std::wstring> closed_;
    Hover hoverKind_ = kNone;
    int hoverIndex_ = -1;
    bool tracking_ = false;
    // 双击尾巴的那一次 UP 不是新的单击：关闭标签后若再 NotifySelect，
    // 消息顺序会是 CLOSE → SELECT(旧下标)，旧下标此时已经指向别的标签。
    bool suppressDblUp_ = false;

    bool dragging_ = false;
    int dragIdx_ = -1;
    POINT dragStart_{};
    int dropIdx_ = -1;
    // 拖拽边缘自动滚动：定时器只在拖到可视区边缘时运行。
    // 起停与边缘判定收口在 edge::Update/Stop（src/edge_autoscroll.h，与书签栏
    // 共用的唯一实现）；本类只持有状态与定时器 id。
    static constexpr UINT_PTR kDragScrollTimer = 3;  // 主窗口已用 1/2
    int dragScrollStep_ = 0;    // 0=停；>0 向右看；<0 向左看
    int dragMouseX_ = 0;        // 定时器没有鼠标坐标，用最后一次 MOUSEMOVE 的 x
    // 拖拽时的实时鼠标位置（浮层跟手用）与"已越过垂直磁吸阈值"标记。
    // tearOff_ 为真时：不画插入线，松手走"移到新窗口"而不是重排。
    POINT dragMouse_{};
    // 按下时鼠标相对被拖标签左缘的偏移：浮层"粘手"用。**不能在绘制时现算** ——
    // 拖到边缘自动滚动会改 tabRects_，现算会让浮层随滚动跳位。
    int dragGrabDx_ = 0;
    bool tearOff_ = false;
    // Layout 缓存的客户区高度：WM_MOUSEMOVE 判垂直越界要看它。
    int clientH_ = 0;
    // 拖拽让位动画：animFrom_[i] = 该位动画起点的客户区 x；animating_ 时
    // Paint 按 AnimDx(i) 偏移绘制。结束/取消一律走 StopReorderAnim。
    std::vector<int> animFrom_;
    ULONGLONG animStart_ = 0;
    bool animating_ = false;
    static constexpr UINT_PTR kAnimTimer = 4;   // 标签栏窗口内的定时器 id
    // 关闭按钮按下时锁定索引：up 必须用按下时的索引，否则按 A 松到 B 会关掉 B。
    int closeDownIdx_ = -1;

    MouseSink sink_;
};
