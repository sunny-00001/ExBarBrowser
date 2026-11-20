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
    {IconId::Close, L"close", 0xE711},
    {IconId::Lock, L"lock", 0xE72E},
    {IconId::Search, L"search", 0xE721},
    {IconId::Settings, L"settings", 0xE713},

    {IconId::Folder, L"folder", 0xE8B7},
    {IconId::Page, L"page", 0xE7C3},
    {IconId::Calendar, L"calendar", 0xE787},
    {IconId::HardDrive, L"hard_drive", 0xEDA2},
    {IconId::Eye, L"eye", 0xE7B3},
    {IconId::Tag, L"tag", 0xE8EC},
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
