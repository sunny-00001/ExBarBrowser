// 状态栏段 id：段的稳定下标，含上界 Seg_Count。
//
// 【为什么单独一个头】段号是对外契约（settings.json 的 statusSegs 数组、
// 状态栏右键菜单、状态栏本体共用同一口径）。但 status_bar.h 带着 HWND /
// Theme / TooltipHost 等 UI 依赖，config / settings 层包含它会形成
// platform → ui 的反向边（settings_reg_table.cpp 曾经如此）。把纯枚举挪到
// 这里，两层都能包含，谁也不拖谁；段号仍然只有一份，不会分叉。
#pragma once

enum SegId {
    Seg_Fld = 0,   // 文件夹 选中/总数
    Seg_File,      // 文件 选中/总数
    Seg_Chk,       // 已选/项目总数
    Seg_Date,      // 修改日期
    Seg_Path,      // 图标 + 路径（弹性）
    Seg_Size,      // 选中容量
    Seg_Disk,      // 容器容量 / 磁盘
    Seg_Eye,       // 隐藏文件三态
    Seg_Type,      // 类型（有选中时给扩展名）
    Seg_Ai,        // AI 助手状态 chip（常驻，点击弹 peek）
    Seg_Undo,      // 撤销历史 chip（常驻，有可撤步数才显示，点击弹撤销历史列表）
    Seg_Count
};

// 常驻段：不受 statusSegs 显隐控制（开关它们不是"显示哪些段"能表达的意思）。
//
// 【为什么需要这个函数】Seg_Ai（AI 状态）与 Seg_Undo（撤销历史）都必须一直有落点：
// AI 的 busy/approval 状态、以及"还有几步可撤"不能因为配置里没勾就消失。
// 于是它们在状态栏右键菜单的勾选列表里被**排除**——
// 列进去让人勾掉、而点完又立刻被补回来，是最坏的组合
//（用户以为关掉了，实际没关，下次还会困惑）。
inline bool StatusSegAlwaysOn(int seg) { return seg == Seg_Ai || seg == Seg_Undo; }
