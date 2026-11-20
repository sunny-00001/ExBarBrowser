// 字形图标登记表（唯一来源）。
//
// 【为什么要有这张表】句柄式图标（HICON）在深色主题下几乎不可见（系统给的是
// 深色位图），所以栏内图标一律走 Segoe Fluent Icons 字形 + 主题色。但以前
// 各处直接写裸码位：
//   nav_bar.cpp:71-75     0xE72B / 0xE72A / 0xE74A / 0xE72C / 0xE700
//   status_bar.cpp:23-35  另一张私有表（7 个码位，其中 0xE73E 与这里重复）
//   tab_strip_paint.cpp   0xE72E / 0xE70D
//   bookmark_bar.cpp      0xE712
// 同一个"后退箭头"在三处各写一遍数字，字形语义完全靠注释维系。
//
// 【纪律】新代码一律用 Glyph(IconId::Xxx)，不写数字。判据见
// tools/check_components.py 的"绕过 ui::DrawChevron 自绘箭头"规则。
#pragma once
#include "common.h"

namespace ico {

// 图标 id。命名按"它表示什么"，不按码位。
enum class IconId {
    None = 0,
    // 导航
    Back, Forward, Up, Refresh, Home,
    // 视图/菜单
    Menu, ViewMenu, More, Overflow,
    // 方向
    ChevronDown, ChevronUp, ChevronLeft, ChevronRight,
    // 状态
    Check, Close, Lock, Search, Settings,
    // 状态栏段
    Folder, Page, Calendar, HardDrive, Eye, Tag,
};

// id -> 字形（码位）。未登记的 id 返回兜底字形（放大镜，必然存在）。
// 需要按名字取（数据驱动处，如 keymap 的图标名）用 IconIdFromName + Glyph。
wchar_t Glyph(IconId id);

// 名字 -> id（"back" / "chevron_down" / ...）。未知返回 IconId::None。
IconId IconIdFromName(const wchar_t* name);

}  // namespace ico
