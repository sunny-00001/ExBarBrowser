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
    COLORREF viewBg, bar;                                    // 视图容器底 / 工具栏底
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
        // 实测：浅色文件区 #ffffff、工具栏 #f3f3f3（不是系统 COLOR_WINDOW 的纯白）
        .viewBg = 0xFFFFFF,     .bar = 0xF3F3F3,
        .text = 0x1A1A1A,       .hover = 0xE5E5E5,     .pressed = 0xD0D0D0,
        .disabled = 0xB0B0B0,   .separator = 0xD0D0D0,
        .editBg = 0xFFFFFF,     .editBorder = 0xC8C8C8,
        .listBg = 0xFFFFFF,     .listSelBg = RGB(204, 232, 255), .listHotBg = 0xE8E8E8,
        .chevron = 0x666666,
        .accent = RGB(0, 120, 215), .dimText = 0x606060,
    };
    static const UiPalette darkTheme = {
        // 实测：深色文件区 #141414、工具栏 #1e1e1e（不是 #202020）
        .viewBg = 0x141414,     .bar = 0x1E1E1E,
        .text = 0xEEEEEE,       .hover = 0x3A3A3A,     .pressed = 0x2E2E2E,
        .disabled = 0x606060,   .separator = 0x3F3F3F,
        .editBg = 0x2A2A2A,     .editBorder = 0x555555,
        .listBg = 0x202020,     .listSelBg = RGB(76, 159, 255),  .listHotBg = 0x353535,
        .chevron = 0xBBBBBB,
        .accent = RGB(76, 160, 255), .dimText = 0x9A9A9A,
    };
    return dark ? darkTheme : light;
}

// 标签分类色板（8 档：0=无颜色，1..7=红/橙/黄/绿/蓝/紫/灰）。
//
// 与 UiPalette 分开：这是"分类标记"，不是 UI 基础令牌 —— 深浅各一套是因为
// 同一色值在浅色标签上过重、在深色标签上过暗（详见 theme.cpp 的 TabColorFill）。
// 放在这里是为了让"色值不与实现混写"：theme.cpp 不再有硬编码 RGB。
struct TabPalette {
    COLORREF colors[8];
};

inline const TabPalette& TabColorsFor(bool dark) {
    static const TabPalette light = {
        {0,
         RGB(0xC0, 0x39, 0x2B), RGB(0xC4, 0x70, 0x0C), RGB(0xA8, 0x8A, 0x00),
         RGB(0x2E, 0x7D, 0x32), RGB(0x00, 0x78, 0xD7), RGB(0x6A, 0x3D, 0x9A),
         RGB(0x60, 0x60, 0x60)}};  // 黄在浅底上压暗，否则看不见
    static const TabPalette darkTheme = {
        {0,
         RGB(0xE0, 0x5A, 0x5A), RGB(0xE0, 0x93, 0x3F), RGB(0xD6, 0xC0, 0x4A),
         RGB(0x5F, 0xB8, 0x6A), RGB(0x4C, 0xA0, 0xFF), RGB(0xA0, 0x7C, 0xE0),
         RGB(0x8A, 0x8A, 0x8A)}};
    return dark ? darkTheme : light;
}
