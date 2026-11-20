// 自绘状态栏：8 段（文件夹 / 文件 / 已选 / 日期 / 路径 / 选中容量 / 容器容量 / 眼睛）
//
// 取数在 view_stats，本类只做"排版 + 绘制 + 命中 + 气泡"，段显隐来自配置。
// 段顺序固定；关掉的段不参与排版（与 mod 的 statusSegs 同语义）。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common.h"
#include "tooltip_host.h"
#include "theme.h"

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
    Seg_Count
};

struct StatusModel {
    std::wstring text[Seg_Count];
    std::wstring tip[Seg_Count];
    int hiddenState = 0;  // 0 全显 / 1 隐系统 / 2 隐全部
};

class StatusBar {
public:
    void Create(HWND parent, const Theme& theme);
    HWND hwnd() const { return hwnd_; }

    void SetModel(const StatusModel& m);
    void SetEnabledSegments(const std::vector<int>& segs);
    const std::vector<int>& enabledSegments() const { return enabled_; }

    void SetClickFn(std::function<void(int seg)> fn) { onClick_ = std::move(fn); }
    void SetContextFn(std::function<void(POINT)> fn) { onContext_ = std::move(fn); }
    void SetMouseSink(MouseSink fn) { sink_ = std::move(fn); }
    void ReloadTheme();

private:
    struct SegLayout {
        int seg = -1;
        RECT rc{};
        RECT textRc{};
        bool glyph = false;
        wchar_t glyphCh = 0;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Layout(HWND);
    void Paint(HWND);
    void UpdateHover(HWND, int x, int y);
    int HitSegment(int x, int y) const;
    void RebuildTooltip(HWND);

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    eb::TooltipHost tooltip_;
    const Theme* theme_ = nullptr;
    StatusModel model_;
    std::vector<int> enabled_{0, 1, 2, 3, 4, 5, 6};
    std::vector<SegLayout> layout_;
    std::vector<int> stableW_;  // 每段历史最大宽（数值稳定宽）
    int hoverSeg_ = -1;
    bool tracking_ = false;

    std::function<void(int)> onClick_;
    std::function<void(POINT)> onContext_;
    MouseSink sink_;
};
