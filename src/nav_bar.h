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
    // 打开设置面板。参数是弹层的屏幕锚点（视图按钮下方）—— 面板与其它弹层
    // 同一套定位方式，不在面板内部自己算位置（那样两个地方都要算一次）。
    using SettingsFn = std::function<void(POINT)>;
    // 当前视图状态的只读查询：视图模式下标 / 排序方式 / 分组方式。
    // 视图菜单要打勾（"现在用的是哪一档"），而导航栏不持有这些状态 ——
    // 它们只在 Config::Settings + ShellView 里。以前菜单完全不标当前值，
    // 用户看不出"详细信息 + 按日期排序"到底生效没有。
    struct ViewState {
        int mode = -1;    // 视图档位下标（-1 = 未知，不打勾）
        int sort = -1;    // 0..3（-1 = 未知）
        int group = -1;   // 0..3
        bool ascending = true;
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
    // 搜索框此前**不存在** —— 于是 Ctrl+F 既没绑上也没处可绑。这里补的是控件本身，
    // 而不是让热键去"假装做了点什么"：一个按键要么把人带到光标已在闪烁的输入框，
    // 要么就该说清楚"这个功能没做"（我们选择前者）。
    //
    // 搜索交给 shell 视图自己的搜索（与在资源管理器里输入即搜同一套），
    // 不自己扫盘 —— 扫盘要么卡 UI，要么结果与用户看到的不一致。
    void FocusSearch();
    bool IsSearching() const;
    // 用户提交搜索词（Enter）。空串 = 清除搜索回到原目录。
    // 由 NavBar 自己调，不让 App 去读控件文本（那是两份状态）。
    using SearchFn = std::function<void(const std::wstring& query)>;
    void SetSearchFn(SearchFn fn) { onSearch_ = std::move(fn); }
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
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);

    void BuildButtons();
    void Layout();
    void Paint(HWND);
    void DrawButton(HDC dc, const Btn& b, bool hover, bool down);
    void DrawBreadcrumb(HDC dc, const RECT& rc);

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
    const Theme* theme_ = nullptr;
    NavModel model_;

    std::vector<Btn> buttons_;
    RECT fieldRc_{};
    RECT dropRc_{};
    // 搜索框（地址框右侧）。没做成常显：它只在 Ctrl+F / 有搜索词时占位，
    // 平时把宽度还给地址框 —— 地址栏是每天用几十次的东西，不该为偶尔的搜索让位。
    HWND search_ = nullptr;
    RECT searchRc_{};
    bool searchVisible_ = false;
    std::wstring searchText_;   // 已有搜索词时保持可见（否则用户看不到"还在筛选"）
    SearchFn onSearch_;
    void SearchCreate();
    void LayoutSearch(int fieldRight, int fy, int fieldH);
    void CommitSearch();
    void ShowSearch(bool show);
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
    std::function<void()> refocus_;
};
