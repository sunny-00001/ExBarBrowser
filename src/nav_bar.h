// 导航栏：5 个导航按钮（真实系统图标 + 分裂箭头）+ 面包屑 + 地址编辑 + 自绘下拉
//
// 视觉与交互对齐 ExBar mod 的 ebnav / NavDrawBar：
//   · 按钮 28px、图标 16px，back/forward 带 12px 分裂箭头（点击出方向历史）
//   · 面包屑：前导文件夹图标 + 常驻「‹」折叠钮 + 每级 20px chevron 区（前导图标 14px）
//   · 地址框：单一边框，右侧内嵌下拉箭头（Directory Opus 风格）
//   · 所有下拉走自绘 popup_list（带图标、24px 行高），不用系统菜单
#pragma once
#include "common.h"
#include "theme.h"
#include "nav_model.h"

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

    void Create(HWND parent, const Theme& theme);
    ~NavBar();
    HWND hwnd() const { return hwnd_; }
    void SetPath(const std::wstring& path, const std::wstring& display);
    std::wstring DisplayText() const { return model_.AddressText(); }
    void FocusAddress();    bool IsEditing() const { return editing_; }
    void SetMouseSink(MouseSink fn) { sink_ = std::move(fn); }
    void ShowViewMenuNow();  // 命令面板/快捷键触发"视图菜单"
    void ReloadTheme();

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
    void SetRecentFn(RecentFn fn) { recentList_ = std::move(fn); }
    void SetClearHistoryFn(ClearHistoryFn fn) { clearHistory_ = std::move(fn); }
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
    void UpdateLeadIcon();

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HWND edit_ = nullptr;
    const Theme* theme_ = nullptr;
    NavModel model_;

    std::vector<Btn> buttons_;
    RECT fieldRc_{};
    RECT dropRc_{};
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

    MouseSink sink_;
    NavFn onNavigate_;
    ActFn onBack_, onForward_, onUp_, onRefresh_;
    ViewFn onView_;
    ListFn historyList_;
    JumpFn historyJump_;
    SortGroupFn onSortGroup_;
    RecentFn recentList_;
    ClearHistoryFn clearHistory_;
    std::function<void()> refocus_;
};
