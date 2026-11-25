// 导航栏 96 DPI 基准尺寸（对齐 ExBar mod 运行时生效值：栏高32 / 按钮框24 / 图标14）
// 注：mod 里 BAR_HEIGHT=30/BTN_SIZE=28/ICON_SIZE=16 只是宏初值，运行时在
//     Exbar.cpp:36505-36512 被 MulDiv(32/24/14, dpi, 96) 覆盖。此处取生效值。
//
// 【这里只放"几何"】按钮框、留白、槽宽属于本栏的布局，栏内改动只影响本栏。
// 图标/字形的**大小**不在这里 —— 它是跨栏观感，统一由 Theme 的尺寸令牌给出
// （theme.h 的 iconChevron / iconGlyph / iconCompact / iconSmall）。
// 原来的 kIcon96 = 14 是这条边界的反面教材：它和"栏内字形 S(11)""列表图标 S(16)"
// 并存，同一行里三个图标来自三个数字，且它自己零引用 —— 已删除。
#pragma once

constexpr int kBtn96 = 24;    // 按钮边长（独立于地址框高度）
// 地址框/搜索框的**整体高度**（96 DPI，边框外沿到外沿）。取 28px：与 30px 导航栏
// 留出上下各 1px；用户量的是边框，字高由字体实测决定，两者是独立量。
constexpr int kFieldH96 = 28;
constexpr int kArrow96 = 12;  // 分裂箭头宽
constexpr int kGap96 = 6;     // 按钮间距
constexpr int kX96 = 8;       // 左起
constexpr int kLpad96 = 6;    // 字段内左边距
constexpr int kLead96 = 18;   // 前导图标槽
constexpr int kSep96 = 20;    // 每级 chevron 区（分隔符前后留白：区宽的一半，兼作点击热区宽）
constexpr int kDrop96 = 24;   // 字段内下拉箭头宽
constexpr int kPad96 = 4;     // 箭头距右边框

// ---- 搜索框（常驻导航栏右侧）----
// 形态对齐 ExBar mod：左带（放大镜 + 下拉箭头，点开「搜索范围」）、中间输入条、
// 右带（箭头，点开「搜索历史」）。mod 的实测值（exbar.cpp:27590-27601）：
// SEARCH_W=200 / btnZone=26 / filterW=24 / rightPad=4 —— 这里照抄同一套数，
// 免得"参照实现"只参照了个大概，量出来又不是那个样子。
constexpr int kSearchW96 = 200;       // 搜索框总宽
constexpr int kSearchBtnZone96 = 26;  // 左带（放大镜 + 箭头）
constexpr int kSearchHistZone96 = 24; // 右带（历史箭头）
constexpr int kSearchGap96 = 4;       // 输入条与右带之间的留白
constexpr int kAddrMin96 = 160;       // 地址框最小宽：不够就让搜索框整体收起
