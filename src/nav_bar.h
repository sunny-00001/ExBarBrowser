// 导航栏：5 个导航按钮（真实系统图标 + 分裂箭头）+ 面包屑 + 地址编辑 + 自绘下拉
//
// 视觉与交互对齐 ExBar mod 的 ebnav / NavDrawBar。
// 尺寸口径分两类，不要混：
//   · **几何**（本栏专属，见 nav_metrics.h）：按钮框 24、分裂箭头宽 12、每级 chevron 区 20、
//     前导图标槽 18、字段内左边距 6…
//   · **图标大小**（跨栏观感，见 theme.h 的尺寸令牌）：按钮/前导图标 iconCompact（14）、
//     面包屑与分裂箭头与下拉箭头 iconChevron（10）。
// 这里过去写的是"按钮 28px、图标 16px"，而代码取的是 kBtn96=24 / kIcon96=14 ——
// 注释与代码各说一套，正是"图标大小莫名其妙变了"时最难查的那种不一致。
//   · 面包屑：前导文件夹图标 + 常驻「‹」折叠钮 + 每级 20px chevron 区
//   · 地址框：单一边框，右侧内嵌下拉箭头（Directory Opus 风格）
//   · 所有下拉走自绘 popup_list（带图标、24px 行高），不用系统菜单
#pragma once
#include "common.h"
#include "theme.h"
#include "nav_model.h"
#include "tooltip_host.h"

class NavBar {
public:
    using NavFn = std::function<void(const std::wstring&)>;
    using ActFn = std::function<void()>;
    using ViewFn = std::function<void(int fvm, int iconPx)>;
    using ListFn = std::function<std::vector<std::wstring>(bool backward)>;  // 最近的在前
    using JumpFn = std::function<void(int depth, bool backward)>;
    using SortGroupFn = std::function<void(int kind, int arg)>;  // kind: 0 排序 1 分组
    using RecentFn = std::function<std::vector<std::wstring>()>;
    using ClearHistoryFn = std::function<void()>;  // 清空地址框下拉里的历史
    // 打开设置。参数是弹层的屏幕锚点（视图按钮下方）—— 设置现在是**独立窗口**
    // （三栏版式，见 src/settings_dialog.h），自己居中于主窗口，不用这个锚点；
    // 参数保留是为了与 ColumnsFn / ZoomFn 共用同一种回调形状。
    using SettingsFn = std::function<void(POINT)>;
    // “选择详细信息…”入口，与设置面板共用锚点类型；回调由 App 转投命令。
    using ColumnsFn = std::function<void(POINT)>;
    // “图标大小…”入口，同样由 App 转投 view.zoom.choose。
    using ZoomFn = std::function<void(POINT)>;
    // 当前视图状态的只读查询：视图模式下标 / 排序方式 / 分组方式。
    // 视图菜单要打勾（"现在用的是哪一档"），而导航栏不持有这些状态 ——
    // 它们只在 Config::Settings + ShellView 里。以前菜单完全不标当前值，
    // 用户看不出"详细信息 + 按日期排序"到底生效没有。
    struct ViewState {
        int mode = -1;    // 视图档位下标（-1 = 未知，不打勾）
        int sort = -1;    // 0..3（-1 = 未知）
        int group = -1;   // 0..3
        bool ascending = true;
        // 任意属性排序/分组时，int 档位表达不了真实列；父菜单小结改用这两个
        // 从 shell 现读的 {GUID}:pid 字符串（D2/D9）。空 = 无/未知。
        std::wstring sortKey;
        std::wstring groupKey;
    };
    using ViewStateFn = std::function<ViewState()>;
    // 面包屑段右键菜单的动作号。用枚举而不是裸 int：App 侧要 switch 它，
    // 裸 int 会让"菜单加一项忘了加分支"变成静默无动作。
    enum SegCmd {
        kSegOpenNewTab = 1,    // 在新标签打开
        kSegOpenNewWindow,     // 在新窗口打开
        kSegCopyPath,          // 复制该段路径
        kSegTerminal,          // 在该段开终端
        kSegProperties,        // 属性
    };
    // 面包屑段右键菜单的动作。path 是该段代表的目录，**不是当前目录** ——
    // 菜单必须作用于用户右键的那一段（Opus / TC / XYplorer 都是这个语义）。
    using SegmentCmdFn = std::function<void(const std::wstring& path, SegCmd action)>;

    void Create(HWND parent, const Theme& theme);
    ~NavBar();
    HWND hwnd() const { return hwnd_; }
    void SetPath(const std::wstring& path, const std::wstring& display);
    std::wstring DisplayText() const { return model_.AddressText(); }
    void FocusAddress();    bool IsEditing() const { return editing_; }
    // 聚焦搜索框（Ctrl+F）。
    //
    // 搜索框**常驻**导航栏右侧（对齐 exbar / 资源管理器）：以前它只在
    // Ctrl+F 时出现，平时把宽度还给地址框 —— 于是"搜索"这个每天都要用的
    // 入口反而要靠一个快捷键才找得到。现在它在窄窗口下才整体收起。
    //
    // 搜索交给 shell 视图自己的搜索（与在资源管理器里输入即搜同一套），
    // 不自己扫盘 —— 扫盘要么卡 UI，要么结果与用户看到的不一致。
    void FocusSearch();
    // 搜索框句柄。给焦点观测面用：地址框与搜索框都在导航栏里，
    // 不给出这个句柄就分不清"焦点在地址栏"和"焦点在搜索框"，
    // 于是 win.focus.search 只能靠猜（而猜错比不答更糟）。
    HWND searchHwnd() const { return search_; }
    // 地址框句柄。与 searchHwnd() 同一口径：焦点循环要分清"焦点在地址栏"
    // 和"焦点在搜索框"，没有句柄就只能猜（猜错比不答更糟）。
    HWND addressHwnd() const { return edit_; }
    bool IsSearching() const;
    // 输入框里现在是什么（**还没提交**）。这是搜索框唯一的可回读面 ——
    // 命令 search.query.set 写的就是它，AI 要能确认"词确实填进去了"。
    std::wstring SearchText() const { return searchText_; }
    // 外部设置搜索词（搜索历史回填 / 命令）。**不提交**：提交是回车的事，
    // 替用户按下回车等于替他决定了要不要真去搜。
    void SetSearchText(const std::wstring& q);
    // 清空搜索词并退出搜索（Esc 的语义）。搜索框本身留着 —— 它是常驻的。
    void ClearSearch();
    // 用户提交搜索词（Enter）。空串 = 清除搜索回到原目录。
    // 由 NavBar 自己调，不让 App 去读控件文本（那是两份状态）。
    using SearchFn = std::function<void(const std::wstring& query)>;
    void SetSearchFn(SearchFn fn) { onSearch_ = std::move(fn); }
    // 打开搜索框左带（搜索范围）/ 右带（搜索历史）的弹层。
    //
    // 菜单内容由 App 组装（范围的真相在 Config::Settings 里，导航栏不该持有
    // 副本），导航栏只算锚点并转过去 —— 与"设置…""选择详细信息…"同一套路。
    //
    // 【宽度随回调一起给】弹层要与搜索框**同宽**（左右边缘齐平），而搜索框
    // 的几何在导航栏这一侧 —— 锚点从导航栏算，宽度也得从导航栏给；否则 App
    // 只能自己再量一遍宽度，两处各量一次必然对不上（缩放/DPI 一变就错位）。
    using PopupFn = std::function<void(POINT screen, int widthPx)>;
    void SetSearchScopeFn(PopupFn fn) { onSearchScope_ = std::move(fn); }
    void SetSearchHistoryFn(PopupFn fn) { onSearchHistory_ = std::move(fn); }
    void SetMouseSink(MouseSink fn) { sink_ = std::move(fn); }
    void ShowViewMenuNow();  // 命令面板/快捷键触发"视图菜单"
    void ReloadTheme();

    // 后退/前进是否可去。唯一判据在 nav::History（canBack/canForward），
    // App 每次导航完成/失败后回调到这里。
    //
    // 【为什么必须由外面告诉】导航栏自己不知道历史栈 —— 它只持有"当前路径"。
    // 以前后退/前进按钮**恒亮**：灰显只做了"向上"一个（因为它能由路径推出来），
    // 于是"没历史可退时按钮亮着、点了没反应"。这类"亮着但点不动"比灰着更糟：
    // 用户会怀疑是程序卡了，而不是"这里没东西可去"。
    struct NavState {
        bool canBack = false;
        bool canForward = false;
        bool canUp = false;
    };
    void SetNavState(const NavState& st);

    void SetNavFn(NavFn fn) { onNavigate_ = std::move(fn); }
    void SetBackFn(ActFn fn) { onBack_ = std::move(fn); }
    void SetForwardFn(ActFn fn) { onForward_ = std::move(fn); }
    void SetUpFn(ActFn fn) { onUp_ = std::move(fn); }
    void SetRefreshFn(ActFn fn) { onRefresh_ = std::move(fn); }
    void SetViewFn(ViewFn fn) { onView_ = std::move(fn); }
    void SetHistoryFn(ListFn list, JumpFn jump) {
        historyList_ = std::move(list);
        historyJump_ = std::move(jump);
    }
    void SetSortGroupFn(SortGroupFn fn) { onSortGroup_ = std::move(fn); }
    void SetViewStateFn(ViewStateFn fn) { viewState_ = std::move(fn); }
    void SetSegmentCmdFn(SegmentCmdFn fn) { segmentCmd_ = std::move(fn); }
    void SetRecentFn(RecentFn fn) { recentList_ = std::move(fn); }
    void SetClearHistoryFn(ClearHistoryFn fn) { clearHistory_ = std::move(fn); }
    void SetSettingsFn(SettingsFn fn) { onSettings_ = std::move(fn); }
    void SetColumnsFn(ColumnsFn fn) { onColumns_ = std::move(fn); }
    void SetZoomFn(ZoomFn fn) { onZoom_ = std::move(fn); }
    // 「设置…」面板的锚点（屏幕坐标）。鼠标路径（右键菜单第 900 项）与
    // 命令路径（win.preferences / 命令面板 / 键位）必须落在**同一个**位置 ——
    // 两处各算一份，早晚会因为布局调整而指向不同的按钮。
    POINT SettingsAnchor() const;
    // 退出地址编辑后把焦点还回视图（Enter/Esc/Tab 三条路径共用）
    void SetRefocusFn(std::function<void()> fn) { refocus_ = std::move(fn); }

private:
    enum BtnId { kBack = 1, kForward, kUp, kRefresh, kView };

    struct Btn {
        int id = 0;
        HICON icon = nullptr;   // owned
        wchar_t glyph = 0;      // MDL2 字形（无图标时用）
        RECT rc{};
        RECT arrow{};
        bool hasArrow = false;
        bool enabled = true;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SearchProc(HWND, UINT, WPARAM, LPARAM);
    // nav_bar_input.cpp：地址框子类安装与 SHAutoComplete 只在这里做，
    // g_oldEditProc 也只活在该 TU，避免生命周期文件与输入文件各持一份。
    void InstallEditSubclass(HWND edit);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);

    void BuildButtons();
    void Layout();  // nav_bar_layout.cpp：唯一的几何出处
    void Paint(HWND);
    void DrawButton(HDC dc, const Btn& b, bool hover, bool down);
    void DrawBreadcrumb(HDC dc, const RECT& rc);
    // 搜索框（整框底色 + 左带放大镜 + 右带箭头 + 悬停高亮）。
    void DrawSearchBox(HDC dc);

    // ---- 命中（nav_bar_hit.cpp；只读 Layout 产出的矩形）----
    int HitButton(int x, int y) const;
    bool HitArrow(int x, int y, int* id) const;
    bool HitMore(int x, int y) const;
    bool HitDrop(int x, int y) const;
    int HitSeg(int x, int y) const;
    int HitChev(int x, int y) const;
    int NavTargetAt(int x, int y) const;   // 鼠标规则的目标区（MT_NAV / MT_ADDRESS）

    void BeginEdit();
    // refocus=true 只用于"用户主动结束编辑"（Enter/Esc/Tab）。下拉菜单抢焦点时
    // 也会走 EndEdit，那时回焦会把焦点从弹层抢回视图，弹层立刻 WA_INACTIVE 关闭。
    void EndEdit(bool refocus = false);
    void CommitEdit();
    std::wstring AddressOfEdit() const;
    bool DoComplete();  // 返回 true = 有候选并已补全（Tab 才吃掉）

    void ShowChildMenu(int partIndex, POINT screen);
    void ShowFoldMenu(POINT screen);
    void ShowHistoryMenu(bool backward, POINT screen);
    void ShowViewMenu(POINT screen);
    // 视图菜单的二级层：0=排序方式 1=分组依据。kind 与 SortGroupFn 一致。
    void ShowSortGroupSubMenu(int kind, POINT screen);
    void ShowDropMenu(POINT screen);
    void ShowSegmentMenu(int partIndex, POINT screen);
    void UpdateLeadIcon();
    // 按钮的气泡提示（纯图标按钮没有文字，用户只能靠悬停确认它是什么）。
    // 状态变化后重建：提示文案要带上可用态，否则灰掉的按钮还提示"后退"，
    // 用户会以为是程序没响应。
    void RebuildTooltip();

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HWND edit_ = nullptr;
    // 上次给 edit_ 设字体时的 Theme::fontGeneration。不用 HFONT 值比较：GDI
    // 句柄会复用，删了旧字体再建新的可能拿到同一数值，误判成"没变"就会让 EDIT
    // 继续持有已失效句柄。
    unsigned editFontGen_ = 0;
    const Theme* theme_ = nullptr;
    NavModel model_;

    std::vector<Btn> buttons_;
    RECT fieldRc_{};
    RECT dropRc_{};
    // 搜索框（地址框右侧，常驻）。形态对齐 exbar：左带点开「搜索范围」，
    // 中间是输入条，右带点开「搜索历史」。三段的判定与几何在 nav_bar_search.cpp，
    // 绘制在 nav_bar_draw.cpp，栏本体只管命中与命中后的转投。
    HWND search_ = nullptr;
    RECT searchRc_{};       // 整个搜索框（边框由本栏画）
    RECT searchMenuRc_{};   // 左带：搜索范围
    RECT searchHistRc_{};   // 右带：搜索历史
    bool searchShown_ = false;  // 布局给了空间（窄窗口时整体收起）
    std::wstring searchText_;   // 输入框里的词（还没提交）
    SearchFn onSearch_;
    PopupFn onSearchScope_;
    PopupFn onSearchHistory_;
    bool hotSearchMenu_ = false;
    bool hotSearchHist_ = false;
    void SearchCreate();
    void LayoutSearch(int fieldRight, int fy, int fieldH, int lineH);
    void CommitSearch();
    // 命中左带 / 右带。
    bool HitSearchMenu(int x, int y) const;
    bool HitSearchHist(int x, int y) const;
    // 弹层的锚点（屏幕坐标）：从带的位置算，与点击落在带的哪一像素无关。
    void ShowSearchScopeMenu();
    void ShowSearchHistoryMenu();
    RECT moreRc_{};
    std::vector<RECT> segRects_;   // 可见段文本区
    std::vector<RECT> chevRects_;  // 可见段 chevron 区（与 segRects_ 平行）
    int firstShown_ = 0;

    int hoverBtn_ = 0;
    int downBtn_ = 0;   // 按下中的按钮（画 pressed 底），抬起/移出清掉
    bool hotMore_ = false;
    bool hotDrop_ = false;
    int hotSeg_ = -1;
    int hotChev_ = -1;
    int openChev_ = -1;   // 当前点开下拉菜单的面包屑级（-1 = 无）；用于分隔符展开态 › -> ⌄
    bool tracking_ = false;

    bool editing_ = false;
    std::wstring tabBase_;
    std::vector<std::wstring> tabMatches_;
    int tabIdx_ = -1;

    HICON leadIcon_ = nullptr;
    std::wstring leadIconPath_;

    // 按钮可用态。来源是 App 的 SetNavState（后退/前进）与路径推导（向上）。
    // 两者都存下来：SetPath 只更新"向上"，不能让后退/前进被顺便重置成 true。
    NavState state_;
    ui::TooltipHost tooltip_;

    MouseSink sink_;
    NavFn onNavigate_;
    ActFn onBack_, onForward_, onUp_, onRefresh_;
    ViewFn onView_;
    ListFn historyList_;
    JumpFn historyJump_;
    SortGroupFn onSortGroup_;
    ViewStateFn viewState_;
    SegmentCmdFn segmentCmd_;
    RecentFn recentList_;
    ClearHistoryFn clearHistory_;
    SettingsFn onSettings_;
    ColumnsFn onColumns_;
    ZoomFn onZoom_;
    std::function<void()> refocus_;
};
