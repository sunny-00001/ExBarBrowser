// 字形图标登记表实现（唯一）。见 icon_glyph.h。
#include "icon_glyph.h"

#include <cstring>

namespace ico {
namespace {

struct Entry {
    IconId id;
    const wchar_t* name;
    wchar_t glyph;
};

// 码位口径与 ExBar mod 的 kNamedIcons 一致（Segoe Fluent Icons / MDL2 通用区）。
// 新增图标只在这里加一行 —— 不要在业务代码里写数字。
constexpr Entry kEntries[] = {
    {IconId::Back, L"back", 0xE72B},
    {IconId::Forward, L"forward", 0xE72A},
    {IconId::Up, L"up", 0xE74A},
    {IconId::Refresh, L"refresh", 0xE72C},
    {IconId::Home, L"home", 0xE80F},

    {IconId::Menu, L"menu", 0xE700},
    {IconId::ViewMenu, L"view_menu", 0xE70D},
    {IconId::More, L"more", 0xE712},
    {IconId::Overflow, L"overflow", 0xE70D},

    {IconId::ChevronDown, L"chevron_down", 0xE70D},
    {IconId::ChevronUp, L"chevron_up", 0xE70E},
    {IconId::ChevronLeft, L"chevron_left", 0xE76C},
    {IconId::ChevronRight, L"chevron_right", 0xE76B},

    {IconId::Check, L"check", 0xE73E},
    {IconId::Add, L"add", 0xE710},        // 新建标签的 ＋（曾由 tab_strip 折线自绘）
    {IconId::Close, L"close", 0xE711},
    {IconId::Lock, L"lock", 0xE72E},
    {IconId::Pin, L"pin", 0xE718},
    // 键盘：序列键候选弹层输入区的芯片图标（默认态 = "序列键"）。
    {IconId::Keyboard, L"keyboard", 0xE765},
    {IconId::Search, L"search", 0xE721},
    {IconId::Settings, L"settings", 0xE713},

    {IconId::Folder, L"folder", 0xE8B7},
    {IconId::Page, L"page", 0xE7C3},
    {IconId::Calendar, L"calendar", 0xE787},
    {IconId::HardDrive, L"hard_drive", 0xEDA2},
    {IconId::Eye, L"eye", 0xE7B3},
    {IconId::Tag, L"tag", 0xE8EC},

    // 菜单动作（MDL2 / Segoe Fluent Icons 共用码位）
    {IconId::Edit, L"edit", 0xE70F},
    {IconId::Copy, L"copy", 0xE8C8},
    {IconId::Delete, L"delete", 0xE74D},
    {IconId::Undo, L"undo", 0xE7A7},

    // 右键菜单动作：码位取自 Segoe MDL2 Assets 图标表，
    // 仍只在这一张表里落数字，业务代码只认 IconId。
    {IconId::NewWindow, L"new_window", 0xE78B},
    {IconId::Terminal, L"terminal", 0xE756},
    {IconId::Sort, L"sort", 0xE8CB},
    {IconId::Import, L"import", 0xE8B5},
    {IconId::Export, L"export", 0xE898},   // Upload：导出到外部

    // 视图菜单：网格 / 列表 / 详情（MDL2 的 ViewAll / BulletedList / CheckList）
    {IconId::GridView, L"grid_view", 0xE8A9},
    {IconId::ListView, L"list_view", 0xE8FD},
    {IconId::DetailsView, L"details_view", 0xE9D5},

    // 设置对话框的分组图标。码位同样取自 MDL2 / Segoe Fluent Icons 共用区，
    // 且都经渲染核对过确实有字形（不是豆腐块）：
    //   E8A0 窗框    E81C 时钟回转   E945 闪电
    //   E762 带勾选的分段列表         E93E 信号波
    //
    // 【四条栏各占一个导航项】设置左栏里"每个栏一个入口"，所以这四个字形是
    // 按栏分配的：E72A 前进箭头（导航栏）· E8EC 标签牌（标签栏）
    // · E718 图钉（书签栏）· E762 分段（状态栏）。
    // 同一形状被两处用是允许的（形状一样、语义不同），与上面 Bars/Menu 同一口径。
    {IconId::NavBar, L"nav_bar", 0xE72A},
    {IconId::TabBar, L"tab_bar", 0xE8EC},
    {IconId::Bookmark, L"bookmark", 0xE718},
    {IconId::StatusBar, L"status_bar", 0xE762},
    {IconId::Window, L"window", 0xE8A0},
    {IconId::History, L"history", 0xE81C},
    {IconId::Bolt, L"bolt", 0xE945},
    {IconId::Channel, L"channel", 0xE93E},
};

constexpr wchar_t kFallback = 0xE721;   // 放大镜：必然存在的字形

}  // namespace

wchar_t Glyph(IconId id) {
    if (id == IconId::None) return 0;
    for (const auto& e : kEntries)
        if (e.id == id) return e.glyph;
    return kFallback;
}

IconId IconIdFromName(const wchar_t* name) {
    if (!name) return IconId::None;
    for (const auto& e : kEntries)
        if (wcscmp(e.name, name) == 0) return e.id;
    return IconId::None;
}

}  // namespace ico
