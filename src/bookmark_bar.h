// 自绘书签栏：图标 + 名称 + 分类下拉 + 溢出 » + 右键菜单 + 拖入文件夹添加。
//
// 数据来自 BookmarkStore（%APPDATA%\ExBar\bookmarks.html，与 mod 共用）；
// 本类只读展示 + 发导航/命令，增删改由 App 的 Bmk* 命令完成。
#pragma once
#include <map>
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
    void Reload();        // 用内存中的 store 重排
    void ReloadFromDisk();// 重新读文件
    void ReloadTheme();

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
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void BuildItems();
    void Layout();
    void Paint(HWND);
    void UpdateHover(HWND, int x, int y);
    int HitItem(int x, int y) const;
    bool HitOverflow(int x, int y) const;
    HICON IconFor(const BookmarkNode& n);
    void ShowItemMenu(int index, POINT screen);
    void ShowEmptyMenu(POINT screen);
    void ShowFolderMenu(int index, POINT screen);
    void ShowOverflowMenu(POINT screen);
    void Navigate(int index);
    void RebuildTooltip(HWND);
    void DragUpdate(HWND h, int x, int y);   // 拖动中：算插入线 + 重绘
    void DragFinish(HWND h, bool commit);    // 松手提交 / 取消

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    eb::TooltipHost tooltip_;
    const Theme* theme_ = nullptr;
    std::vector<Item> items_;
    std::map<std::wstring, HICON> icons_;
    RECT ovfRc_{};
    bool overflow_ = false;
    int hover_ = -1;
    bool hotOvf_ = false;
    bool tracking_ = false;

    // 顶层拖动重排（仅文件系统样式的可见项；按下→移动超阈值→松手提交 Move）
    bool dragArm_ = false;     // 按下未超阈值
    bool dragging_ = false;    // 已超阈值，正在拖
    int dragIdx_ = -1;         // 起始项
    int dropIdx_ = -1;         // 插入线位置（绘制到 dropIdx_ 左缘）
    POINT dragStart_{};

    std::function<void(const std::wstring&)> onNavigate_;
    MouseSink sink_;
};
