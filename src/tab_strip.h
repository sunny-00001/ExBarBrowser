// 自绘标签栏：图标 + 标题 + 关闭钮 + 锁定态 + 溢出滚动/» 菜单 + 拖拽排序。
//
// 只做"画 + 命中 + 发消息"，标签状态仍由 App 持有；顺序变化也只上报，不自己改。
#pragma once
#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "tooltip_host.h"
#include "tab_set.h"
#include "theme.h"

// 逻辑像素（Theme::S 再缩放）。Layout 与窗口初建尺寸共用，避免两处各写一个数。
constexpr int kTabBarHeightDp = 26;

// 标签右键菜单动作（TabStrip -> App，走 WM_APP_TAB_MENU 的 lParam）
enum TabMenuAction {
    kTabMenuNew = 1,
    kTabMenuClose,
    kTabMenuCloseOther,
    kTabMenuCloseLeft,
    kTabMenuCloseRight,
    kTabMenuClone,
    kTabMenuToggleLock,
    kTabMenuCopyPath,
    kTabMenuReopen,
};

class TabStrip {
public:
    ~TabStrip();
    void Create(HWND parent, const Theme& theme);
    HWND hwnd() const { return hwnd_; }
    // 只吃模型：标题兜底、图标解析与缓存都在本类内完成（App 不再持有 HICON）。
    void SetTabs(const std::vector<core::TabRef>& refs, int active);
    void ReloadTheme();
    // 释放图标缓存；WM_DESTROY 与析构都会调，重复调用安全。
    void ReleaseIcons();
    void SetMouseSink(MouseSink fn) { sink_ = std::move(fn); }

private:
    // 行 = 一条标签的渲染数据，由 SetTabs 从 core::TabRef 生成；icon 归本类所有。
    struct TabStripItem {
        std::wstring title;
        std::wstring path;  // tooltip 用
        bool locked = false;
        HICON icon = nullptr;
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
    // 否则未激活的标签右端有一块"看不见但能关"的区域（双击还会变成新建标签）。
    bool CloseVisible(int index) const;
    bool HitNew(int x, int y) const;
    bool HitOverflow(int x, int y) const;
    void ShowTabMenu(int index, POINT screen);
    void ShowOverflowMenu(POINT screen);
    void ScrollBy(int dx);
    void EnsureVisible(int index);
    void RebuildTooltip(HWND);
    void NotifyClose(int index);
    void NotifySelect(int index);
    // 小图标的唯一取图点：命中缓存就复用，未命中才去 Resolve。
    HICON TabIcon(const std::wstring& path);
    // 缓存到顶时清掉本次不再展示的 path（正在用的 icon 不能销毁）。
    void PruneIcons(const std::vector<core::TabRef>& refs);

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    eb::TooltipHost tooltip_;
    const Theme* theme_ = nullptr;
    std::vector<TabStripItem> items_;
    // path -> HICON（本类拥有，ReleaseIcons 统一销毁）。上限见 tab_strip.cpp TabIcon。
    std::map<std::wstring, HICON> iconCache_;
    std::vector<RECT> tabRects_;
    std::vector<RECT> closeRects_;
    RECT newRect_{};
    RECT ovfRc_{};
    int active_ = -1;
    int scrollX_ = 0;
    int maxScroll_ = 0;
    bool overflow_ = false;
    Hover hoverKind_ = kNone;
    int hoverIndex_ = -1;
    bool tracking_ = false;

    bool dragging_ = false;
    int dragIdx_ = -1;
    POINT dragStart_{};
    int dropIdx_ = -1;
    // 关闭按钮按下时锁定索引：up 必须用按下时的索引，否则按 A 松到 B 会关掉 B。
    int closeDownIdx_ = -1;

    MouseSink sink_;
};
