// ExBar 调色板（从 src/parts/001-eb.cpp 的 ThemeColors 原样搬运，只保留导航栏/弹层用到的令牌）
//
// 不要再自己调色：栏/面包屑/下拉的所有颜色必须来自这里，才能和 mod 完全一致。
#pragma once
#include "common.h"

struct EbColors {
    COLORREF text, hover, pressed, disabled, separator;      // 栏基础色
    COLORREF editBg, editBorder, editFocus, editPlaceholder; // 编辑框
    COLORREF listBg, listSelBg, listHotBg, listText, listInertText;  // 下拉列表
    COLORREF chevron, chevronHot;                            // 面包屑展开箭头
    COLORREF panelBorder, accent, dimText;
};

inline const EbColors& EbPalette(bool dark) {
    static const EbColors light = {
        0x1A1A1A, 0xE5E5E5, 0xD0D0D0, 0xB0B0B0, 0xD0D0D0,
        0xFFFFFF, 0xC8C8C8, RGB(0, 102, 201), 0x808080,
        0xFFFFFF, RGB(204, 232, 255), 0xE8E8E8, 0x1A1A1A, 0x9A9A9A,
        0x666666, 0x202020,
        0xA0A0A0, RGB(0, 120, 215), 0x606060,
    };
    static const EbColors darkTheme = {
        0xEEEEEE, 0x3A3A3A, 0x2E2E2E, 0x606060, 0x3F3F3F,
        0x2A2A2A, 0x555555, RGB(76, 159, 255), 0x777777,
        0x202020, RGB(76, 159, 255), 0x353535, 0xF0F0F0, 0x777777,
        0xBBBBBB, 0xDDDDDD,
        0x000000, RGB(76, 160, 255), 0x9A9A9A,
    };
    return dark ? darkTheme : light;
}
