// 原生导航窗格：**独立控件**（INameSpaceTreeControl），与文件列表彻底解耦。
//
// 【为什么不用 IExplorerBrowser 的 EP_NavPane】
// 试过，结构上做不到"只要树"。依据（全部实测/官方文档）：
//   1. 文件列表是浏览器的**主视图**，导航窗格是框架里的可选窗格。官方 EBO_SHOWFRAMES
//      原文："If EBO_SHOWFRAMES is not set, Explorer browser uses a single view object."
//      —— 能只要文件列表（EBO_NONE），但不能只要导航窗格。
//   2. 官方的窗格清单（IExplorerPaneVisibility::GetPaneState）里**没有内容列**这个
//      GUID，所以"关掉内容列只留树"没有合法途径（探针 18：强行关掉会让整棵树一起消失）。
//   3. 唯一能"看起来只有树"的办法是裁剪：给 browser 一个 300 宽的 RECT、宿主只有
//      150 宽 + WS_CLIPCHILDREN，把内容列裁到边界外。但导航列宽度被 shell 内部锁死
//      在 150px（探针 23：RECT 给 200/250/300/900，树恒为 w=150），**无法随窗格缩放**。
//      用户明确否决了这条路："光靠物理切割是不行的，那我缩放窗格不就失效了"。
//
// 【为什么独立控件这条路成立】探针 29~35 逐条验证：
//   · 图标正常（探针 30：img=51~70，imagelist 54~70 张全在）。之前"图标空白"是
//     我们自己的事件实现写了 OnGetDefaultIconIndex 却返回 S_OK 不填输出参数，
//     NSTC 于是退到索引 0（= 空白图）。现在**不实现该回调**，图标就正常。
//   · 根结构可精确拼成 explorer 左栏（探针 35）：
//       主文件夹 CLSID {679F85CB-...}  -> 桌面/Downloads/文档/图片/音乐/视频
//       图库 CLSID     {E88865EA-...}
//       此电脑 FOLDERID_ComputerFolder -> 各盘符
//       网络   FOLDERID_NetworkFolder
//     解析前缀用 "shell:::{GUID}"（"::{GUID}" 对"主页"会失败，实测）。
//   · 宽度能外部控制（探针 31）：SetWindowPos(NSTC 窗口) **并补发 WM_SIZE** 后，
//     内部 SysTreeView32 才跟着变（只 SetWindowPos 不动，是因为控件按 WM_SIZE 重排）。
//   · 树 -> 视图：**用 OnItemClick**（真实左键/键盘动作），不是 OnSelectionChanged。
//     实测（2026-11-22）：NSTC 在建树/首次布局/SyncTo 时都会自己抛选中变化，
//     把 OnSelectionChanged 接给导航会让"一启动就跳到最后一个根（网络）"，
//     并且 SyncTo 会回灌成导航，形成视图->树->视图的自激回环。
//   · 视图 -> 树：SetItemState(psi, NSTCIS_SELECTED, NSTCIS_SELECTED) +
//     EnsureItemVisible(psi（需先 TreeAdvise）) —— 按 IShellItem 定位，
//     不需要"选中->导航->回传路径"那套异步状态机。
//
// 【结论】这条路比 EP_NavPane 更简单也更强：树是真独立控件，宽度由布局直接算，
// 双栏/缩放/显隐都不受 shell 框架约束。
#pragma once
#include "common.h"
#include "nav_pane_root.h"   // RootSpec 在那边（纯配置语义，可单测）
#include "nav_pane_sep.h"    // 组间分隔线的绘制（画在内部树的 WM_PAINT 里）
#include "theme.h"

#include <shobjidl.h>

#include <vector>

namespace navpane {

// 取 IShellItem 的解析路径。文件系统项给盘符路径；命名空间项给 `shell:::{GUID}`
// （**必须带 shell: 前缀**：裸 `::{GUID}` 对"主页"返回 E_INVALIDARG）。
// 实现与理由在 nav_pane.cpp；事件适配器（nav_pane_events.cpp）也要用它。
std::wstring PathOfItem(IShellItem* item);

class NavPane;

// 原生导航窗格宿主。整个窗口一棵，不属于任何窗格。
class NavPane {
public:
    NavPane() = default;
    ~NavPane() { Destroy(); }
    NavPane(const NavPane&) = delete;
    NavPane& operator=(const NavPane&) = delete;

    // 用户点中树节点时回调。参数是该节点的真实路径
    // （SIGDN_DESKTOPABSOLUTEPARSING；纯命名空间节点给 "::{GUID}" 形式）。
    using PickFn = std::function<void(const std::wstring& path)>;

    // 在 parent 的 (0,0,w,h) 建窗格宿主。失败返回 false。
    bool Create(HWND parent, const Theme& theme, int w, int h);
    void Destroy();

    // 布局：宿主占 parent 左侧 w 宽、h 高（App 会把窗格区右移 w）。
    // 这里是**真的改宽度**：宿主 + NSTC 窗口 + 内部 SysTreeView32 三层一起调，
    // 不存在 EP_NavPane 那种"shell 内部把导航列锁死在 150px"的问题。
    void Resize(int w, int h);
    void SetVisible(bool visible);
    void ApplyTheme(const Theme& theme);
    // 配置改了行距/缩进时由 App 调：只重设 TVM_* 与分隔线，不重建整棵树。
    void RefreshTreeMetrics();

    void SetPickFn(PickFn fn) { pickFn_ = std::move(fn); }

    // 当前主题（供空白区右键菜单复用书签菜单的绘制器）。
    // 返回引用而不是快照：Theme::Reload 会原地重建画刷/字体，快照会立刻过期。
    const Theme& theme() const { return *theme_; }

    // 命令层/右键菜单的统一刷新入口：刷新已展开节点并重发现 Explorer 侧栏根集。
    void Refresh();

    // 让某个路径在树里可见并选中（视图导航后同步树的高亮）。
    // 尽力语义：树里没有对应节点（未展开/不存在）时静默跳过，绝不让导航失败。
    void SyncTo(const std::wstring& path);

    // 展开整棵树（Ctrl+Shift+E）。返回 false = 没有树控件或树是空的。
    bool ExpandAll();

    bool ok() const { return tree_ != nullptr; }
    // 树占据的宽度（0 = 未创建/隐藏）。App 布局用它决定窗格区左边界。
    int width() const { return visible_ ? width_ : 0; }
    // 当前是否可见。App 用它判断"要不要重排" —— 宽度没变就不必 Layout。
    bool visible() const { return visible_ && tree_ != nullptr; }

    // 按新根列表重建（配置改动后热应用）。保留当前选中项（尽力）。
    void SetRoots(const std::vector<RootSpec>& roots);

    // 重算组间分隔线。线的位置在**每次绘制时现算**（见 nav_pane_sep.h），
    // 所以这个入口只负责"让树重画一遍"，顺带把线位记进日志。
    void UpdateSeparatorLines();

    // 内部 SysTreeView32 的窗口句柄（0 = 还没建/已销毁）。
    //
    // 【为什么需要公开】"启动首帧的树高度对不对"只能在**代码里**判定：
    // ShowWindow 与 Layout 之间的错误状态一闪而过，截图/外部探针都采不到
    // （实测把顺序改回旧的，探针仍报 0 空白 —— 判据是空的）。
    // 诊断代码需要能在显示之前直接量树的高度，所以暴露这个只读访问器。
    // 不要拿它去改树的样式/状态 —— 那是 NavPane 的职责。
    HWND treeWindow() const { return FindSysTree(); }

    // ---- IUnknown（事件适配器持有的是"回指"，不参与生命周期）----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv);
    ULONG STDMETHODCALLTYPE AddRef();
    ULONG STDMETHODCALLTYPE Release();

    // 事件适配器的工厂（定义在 nav_pane_events.cpp）。做成静态成员是因为
    // Events 是私有嵌套类：类外无法直接 new，而 nav_pane.cpp 又只需要把它
    // 当成 IUnknown 指针用，不该为了构造它而 include 200 行 COM 样板。
    static INameSpaceTreeControlEvents* MakeEvents(NavPane* owner);

private:
    class Events;   // INameSpaceTreeControlEvents 适配器
    friend class Events;

    void OnPicked(IShellItem* item);      // 事件适配器 -> 这里（**用户真实动作**才走）
    void OnPickedPath(const std::wstring& path);  // 鼠标/键盘两条入口的共同出口
    bool PickCurrent();                   // 键盘回车：导航到当前高亮项；没高亮返回 false
    void NoteSelection(IShellItem* item); // 选中变化：只记录高亮位置，**不导航**
    void ApplyLayoutToControl(bool syncTreeState = true);   // width_/height_ 真正落到 NSTC + 内部树
    void PaintTreeColors(const Theme& theme);  // 给内部树刷背景/文字色（幂等）
    void RepaintTreeColorsFromCache();    // 用缓存的颜色重刷（树刚重建时用）
    HWND FindSysTree() const;             // 内部 SysTreeView32
    void RebuildRoots();

    // 行几何对齐 explorer（indent / 行高）。
    void ApplyTreeMetrics();

    // 内部树的滚动钩子：分隔线画在树**外面**的覆盖层上，树一滚，线必须跟着走。
    void HookTreeScroll();
    void UnhookTreeScroll();

    // 树滚动/改尺寸后的回调（由 TreeSubclassProc 触发）。
    void OnTreeGeometryChanged();

    // ---- 分隔线：画在内部树自己的 WM_PAINT 里（见 nav_pane_sep.h 的取舍）----
    //
    // 算出组界线在**内部树客户区**里的 y 坐标（每次绘制现算，不缓存位置）。
    std::vector<int> ComputeSeparatorYs(HWND tv);
    // 在 dc 上补画分隔线。dc 传 nullptr 时自己取树的窗口 DC。
    void PaintSeparatorLines(HWND tv, HDC dc);

    // 内部树的子类化过程（要访问私有成员，所以做成静态成员而不是自由函数）。
    static LRESULT CALLBACK TreeSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                              UINT_PTR id, DWORD_PTR refData);

    // 宿主窗口过程：SHChangeNotifyRegister 的消息必须发给一个窗口。
    // 右键的“是否要弹、给谁弹”仍由 NSTC 事件决定；但真正显示延后到这里，
    // 走与书签栏同一个 ui::Menu::Show 弹层，避免原生菜单窗口的第二套视觉。
    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleHostMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void ShowBlankMenu(POINT screen);   // WM_APP_NAV_BLANK_MENU 的实际处理

    void RegisterShellChanges();
    void UnregisterShellChanges();
    void ScheduleShellRefresh();
    void RefreshExpandedItems();

    // 根集替换/结构刷新共用的展开状态快照。只记录已经物化且展开过的节点，
    // 恢复时按前序 SetItemState(NSTCIS_EXPANDED)，父在子前。
    std::vector<std::wstring> CaptureExpandedPaths() const;
    void RestoreExpandedPaths(const std::vector<std::wstring>& paths);

    INameSpaceTreeControl* tree_ = nullptr;
    // 事件适配器（定义在 nav_pane_events.cpp，由 MakeEvents 造）。
    // 类型是接口指针而非 Events*：nav_pane.cpp 不需要它的完整定义。
    INameSpaceTreeControlEvents* events_ = nullptr;
    DWORD cookie_ = 0;
    const Theme* theme_ = nullptr;    // 借用 App::theme_，不拥有
    HWND parent_ = nullptr;
    HWND host_ = nullptr;      // 承载 NSTC 的自绘宿主（我们自己的窗口类）
    HWND nstcWnd_ = nullptr;   // NSTC 自己的窗口（QI IOleWindow 拿到的）
    int width_ = 0;
    int height_ = 0;
    bool visible_ = true;
    // 重建树（RemoveAllRoots + AppendRoot）期间为 true：NSTC 会在 AppendRoot 的
    // 调用栈内同步抛"点击展开"回调，那不是用户意图，必须挡住。
    bool suppressClick_ = false;
    std::vector<RootSpec> roots_;
    std::wstring darkTheme_;
    // 最近一次套用的颜色。SetRoots 会重建内部树（树可能被 NSTC 换新），
    // 那时拿不到 Theme 参数，就用这份缓存把背景色重刷回去。
    COLORREF treeBg_ = CLR_INVALID;
    COLORREF treeFg_ = CLR_INVALID;
    // 树当前高亮的路径。**只用于去重，不是导航意图** ——
    // NoteSelection（控件自己抛的选中变化）与 SyncTo 都维护它。
    std::wstring selection_;
    PickFn pickFn_;
    // 分隔线的组界项（**缓存**）。滚动时每次重算线位都去
    // SHCreateItemFromParsingName 的话，等于每滚一下就把第三方网盘扩展的 DLL
    // 再绑一遍 —— 太贵，而且 shell 调用会重入 NSTC，风险高。
    //
    // 【必须连根下标一起存】这里存的是**紧凑**数组（只有组界项），
    // 下标与 roots_ 的下标**不是一回事**：27 个根里只有 2 个组界，
    // sepItems_ 的长度是 2，而组界项在 roots_ 里是第 3、10 项。
    // 曾经按 roots_ 的下标去取 sepItems_[i-1] —— 越界读野指针，
    // GetItemRect 拿垃圾指针失败 -> ys 为空 -> **一条分隔线都没画出来**
    // （顺带是 comctl32 崩溃的嫌疑源）。现在按结构体里的 rootIndex 走。
    struct SepItem {
        size_t rootIndex = 0;       // 在 roots_ 里的下标（诊断/日志用）
        IShellItem* item = nullptr; // 组界项 = **新组的第一项**
    };
    std::vector<SepItem> sepItems_;
    // 分隔线颜色。深色用 explorer 的实测值 0x383838（浅色用主题的 separator）。
    COLORREF sepColor_ = CLR_INVALID;
    // 最近一次算出的线位（客户区坐标）。只用于日志/诊断，不参与绘制决策。
    std::vector<int> sepYs_;
    // 画线时的重入保护：ComputeSeparatorYs 会回 NSTC 取行矩形，
    // 那是一次跨窗口消息，若它反过来又触发 WM_PAINT 就会无限递归。
    bool paintingSep_ = false;
    // 当前挂着滚动钩子的内部树句柄（内部树可能被 NSTC 重建，要重新挂）。
    HWND treeHooked_ = nullptr;
    // 事件适配器用到的回指计数（适配器自己不持所有权，由 NavPane 管）。
    mutable LONG comRef_ = 1;

    // ---- Shell 变更通知 / 右键菜单的运行期状态 ----
    ULONG_PTR shellReg_ = 0;      // SHChangeNotifyRegister 的注册 id（0 = 没注册）
    UINT_PTR refreshTimer_ = 0;   // 合并短时间内的多条通知（0 = 没有待处理刷新）
    bool refreshing_ = false;     // 防止 RefreshExpandedItems 重入
};

}  // namespace navpane