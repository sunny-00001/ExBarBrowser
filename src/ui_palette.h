// 色值令牌表（唯一来源）。
//
// 【来历】色值最初从 mod 的 `src/parts/001-eb.cpp` 的 ThemeColors 原样搬来，
// 目的是"换栈前后像素级一致"。现在这些值已成为本工程自己的基线 —— 改它就是在
// 改本工程的观感，不再需要与 mod 对账（工程已独立，见 docs/NAMING.md）。
//
// 【纪律】不要在栏/弹层里自己调色。所有颜色经 Theme（theme.h）流转 ——
// Theme::Init 从这里取令牌，再把它们变成 COLORREF 与 HBRUSH 供组件使用。
#pragma once
#include "common.h"

struct UiPalette {
    // ---- 已被 Theme 消费（改动生效）----
    COLORREF text, hover, pressed, disabled, separator;      // 栏基础色
    COLORREF editBg, editBorder;                             // 编辑框底/边框
    COLORREF listBg, listSelBg, listHotBg;                   // 下拉列表底/选中/悬停
    COLORREF chevron;                                        // 面包屑展开箭头
    COLORREF accent, dimText;                                // 强调色 / 次要文字

};

inline const UiPalette& PaletteFor(bool dark) {
    // 指定初始化器（C++20）：**字段顺序无关**。
    // 这里原来是位置式初始化 —— 20 个连续 COLORREF 靠逗号对齐，
    // 插一个字段、删一个字段都会让后面全部错位，而错位的表现是"某个颜色莫名其妙变了"，
    // 比编译错误难查得多。
    static const UiPalette light = {
        .text = 0x1A1A1A,       .hover = 0xE5E5E5,     .pressed = 0xD0D0D0,
        .disabled = 0xB0B0B0,   .separator = 0xD0D0D0,
        .editBg = 0xFFFFFF,     .editBorder = 0xC8C8C8,
        .listBg = 0xFFFFFF,     .listSelBg = RGB(204, 232, 255), .listHotBg = 0xE8E8E8,
        .chevron = 0x666666,
        .accent = RGB(0, 120, 215), .dimText = 0x606060,
    };
    static const UiPalette darkTheme = {
        .text = 0xEEEEEE,       .hover = 0x3A3A3A,     .pressed = 0x2E2E2E,
        .disabled = 0x606060,   .separator = 0x3F3F3F,
        .editBg = 0x2A2A2A,     .editBorder = 0x555555,
        .listBg = 0x202020,     .listSelBg = RGB(76, 159, 255),  .listHotBg = 0x353535,
        .chevron = 0xBBBBBB,
        .accent = RGB(76, 160, 255), .dimText = 0x9A9A9A,
    };
    return dark ? darkTheme : light;
}
