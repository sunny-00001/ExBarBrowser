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
    Seg_Count
};
