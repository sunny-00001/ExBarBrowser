// 自绘书签栏：图标 + 名称 + 分类下拉 + 溢出 » + 右键菜单 + 拖入文件夹添加。
//
// 数据来自 BookmarkStore（%APPDATA%\ExBar\bookmarks.html，与 mod 共用）；
// 本类只读展示 + 发导航/命令，增删改由 App 的 Bmk* 命令完成。
#pragma once
#include <string>
#include <vector>

#include "bookmarks.h"
#include "common.h"
#include "tooltip_host.h"
#include "theme.h"

class BookmarkBar {
public:
    void Create(HWND parent, const Theme& theme);
    ~BookmarkBar();

    HWND hwnd() const { return hwnd_; }
    void SetNavigateFn(std::function<void(const std::wstring&)> fn) { onNavigate_ = std::move(fn); }
    void SetMouseSink(MouseSink fn) { sink_ = std::move(fn); }
    // 键盘退出本栏（Esc）时把焦点还给视图。与 nav_bar 的 SetRefocusFn 同一契约：
    // 焦点卡在栏上会让文件视图的 Delete / type-ahead 全部失效。
    void SetRefocusFn(std::function<void()> fn) { refocus_ = std::move(fn); }
    void Reload();        // 用内存中的 store 重排
    void ReloadFromDisk();// 重新读文件
    void ReloadTheme();
    // 键盘进入本栏：把焦点给窗口并把 kbdIdx_ 落在第一项。
    // 本程序不跑 IsDialogMessage，Tab 不是焦点遍历键 —— 所以"到达书签栏"
    // 必须由一条命令/热键显式触发（与 F4 到地址栏同一模式）。
    void FocusBar();

    // OLE 拖放支持（bookmark_drop 调用）：
    int FolderItemAt(POINT screen) const;            // 屏幕点 → 可见项索引，-1=无
    std::wstring FolderIdAt(int index) const;        // 项索引 → 文件夹书签 id（非文件夹=空）
    void ReloadExternal();                           // 拖放完成后的外部触发的刷新

private:
    struct Item {
        std::wstring id;
        std::wstring label;
        std::wstring path;  // 分类为空
        bool folder = false;
        HICON icon = nullptr;
        RECT rc{};
        RECT iconRc{};      // 左侧图标命中区：点击 → 子目录树（等同面包屑分隔符）
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void BuildItems();
    void Layout();
    void Paint(HWND);
    void UpdateHover(HWND, int x, int y);
    int HitItem(int x, int y) const;
    int HitIcon(int x, int y) const;
    bool HitOverflow(int x, int y) const;
    HICON IconFor(const BookmarkNode& n);
    void ShowItemMenu(int index, POINT screen);
    void ShowEmptyMenu(POINT screen);
    void ShowFolderMenu(int index, POINT screen);
    void ShowChildTree(int index);   // 图标：列该路径的子目录，可就地逐级展开
    void ShowOverflowMenu(POINT screen);
    void Navigate(int index);
    void RebuildTooltip(HWND);
    void DragUpdate(HWND h, int x, int y);   // 拖动中：算插入线 + 重绘
    void DragFinish(HWND h, bool commit);    // 松手提交 / 取消

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    ui::TooltipHost tooltip_;
    const Theme* theme_ = nullptr;
    std::vector<Item> items_;
    RECT ovfRc_{};
    bool overflow_ = false;
    int hover_ = -1;
    int hoverIcon_ = -1;   // 悬停在图标格上的项（高亮该格，提示"这里能点开子目录"）
    bool hotOvf_ = false;
    bool tracking_ = false;

    // 顶层拖动重排（仅文件系统样式的可见项；按下→移动超阈值→松手提交 Move）
    bool dragArm_ = false;     // 按下未超阈值
    bool dragging_ = false;    // 已超阈值，正在拖
    int dragIdx_ = -1;         // 起始项
    int dropIdx_ = -1;         // 插入线位置（绘制到 dropIdx_ 左缘）
    int dropInto_ = -1;        // 拖到某个分类"内部"时该分类的项索引（-1 = 无）
    POINT dragStart_{};

    // ---- 键盘访问 ----
    //
    // 书签栏原先**完全没有**键盘路径：只能鼠标点。而标签栏早就有焦点态与裸键处理，
    // 于是"用键盘切换标签可以、用键盘选书签不行" —— 这不是取舍，是缺口。
    // Opus / Total Commander / XYplorer 的收藏栏都能 Tab 进去、方向键走、Enter 打开、
    // Shift+F10 出菜单。这里照同一套做：
    //   · Tab / Shift+Tab 到达本栏 → kbdIdx_ 落在第一项（或上次的位置）
    //   · ←/→ 在项间移动，Home/End 到首/尾
    //   · Enter / 空格 → 打开（分类则展开菜单）
    //   · Shift+F10 / 应用键 → 该项的右键菜单（无障碍要求：图标功能都要有键盘路径）
    //   · Esc → 把焦点还给视图（不然焦点卡在栏上，Delete/type-ahead 全哑）
    int kbdIdx_ = -1;
    void PaintFocusRing(HDC dc) const;

    std::function<void(const std::wstring&)> onNavigate_;
    std::function<void()> refocus_;
    MouseSink sink_;
};
