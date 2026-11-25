// 布局尺度约束的**唯一来源**：侧栏宽度的上下限与默认值、文件区最小宽度。
//
// 【为什么必须是一份】这几个数以前分散在三处：config.h 的成员默认值、
// config_io.cpp 的读盘夹取、settings_reg_table.cpp 的写入范围。三处只要差一点点，
// 界面能拖到的范围就和落盘接受的范围不同 —— 松手那一刻分割条往回跳一截。
// pane_split.h 的老注释里记着同一条教训（"界面给 90%、落盘只收 80%"）。
// 现在 UI（Layout 给的拖动范围）、命令（settings_reg）、落盘（config_io）读同一份。
//
// 单位是**物理像素**（96 DPI 下与逻辑像素相同）：拖动给的是像素、落盘存的是像素，
// 中间不插一次 DPI 缩放，否则同一份配置在不同 DPI 下拖出来的边界会对不上。
#pragma once

// ---- 导航窗格宽度 ----
constexpr int kNavPaneWidthDefault = 220;
constexpr int kNavPaneWidthMin = 120;
constexpr int kNavPaneWidthMax = 600;

// ---- AI 面板宽度 ----
constexpr int kAiPanelWidthDefault = 420;
constexpr int kAiPanelWidthMin = 240;
constexpr int kAiPanelWidthMax = 900;

// 文件区的最小宽度：任何一条侧栏都不能把中间压到比它还窄。
// 窗口太窄时 AI 面板整块收起（LayoutKey 的 aiW 归 0），而不是把文件区挤没。
constexpr int kMinFileAreaPx = 160;