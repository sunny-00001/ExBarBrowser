// 导航栏 96 DPI 基准尺寸（对齐 ExBar mod 运行时生效值：栏高32 / 按钮框24 / 图标14）
// 注：mod 里 BAR_HEIGHT=30/BTN_SIZE=28/ICON_SIZE=16 只是宏初值，运行时在
//     Exbar.cpp:36505-36512 被 MulDiv(32/24/14, dpi, 96) 覆盖。此处取生效值。
#pragma once

constexpr int kBtn96 = 24;    // 按钮边长（独立于地址框高度）
constexpr int kFieldVPad96 = 2;  // 地址框边框内、文字上下的垂直留白（各 2px）
constexpr int kIcon96 = 14;   // 图标/前导图标（与上面注释的 mod 生效值一致，原来写的 16 会把 16px 图标塞进 14px 槽）
constexpr int kArrow96 = 12;  // 分裂箭头宽
constexpr int kGap96 = 6;     // 按钮间距
constexpr int kX96 = 8;       // 左起
constexpr int kLpad96 = 6;    // 字段内左边距
constexpr int kLead96 = 18;   // 前导图标槽
constexpr int kSep96 = 20;    // 每级 chevron 区（分隔符前后留白：区宽的一半，兼作点击热区宽）
constexpr int kDrop96 = 24;   // 字段内下拉箭头宽
constexpr int kPad96 = 4;     // 箭头距右边框
