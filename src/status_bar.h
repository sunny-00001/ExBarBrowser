// 自绘状态栏：10 段（文件夹 / 文件 / 已选 / 日期 / 路径 / 选中容量 / 容器容量 / 隐藏三态 / 类型 / AI chip）
//
// 取数在 view_stats + app_stats，本类只做"排版 + 绘制 + 命中 + 气泡"，段显隐来自配置。
// 段顺序固定；关掉的段不参与排版（与 mod 的 statusSegs 同语义）。
//
// 【AI chip 是常驻段】Seg_Ai 不跟随 statusSegs 的显隐：面板收起后，AI 的
// busy / approval / done / error 仍要在状态栏上有落点。用户关掉的是"文件统计段"，
// 不是"后台 AI 是否还在跑"。SetEnabledSegments 会补上它（重复会被归一掉）。
//
// 【图标口径】段的图标走**系统默认图标**（见 sys_icon.h）：优先系统位图
// （SHGetStockIconInfo / 系统 DLL 序号资源），系统没有对应资源的形状（日期、隐藏三态、
// 类型）退 Segoe Fluent Icons 字形 —— 那也是系统字体，只是位图形态。
// 段位图与段字形的对应关系只有一份映射（StatusSegmentSysId / StatusSegmentIcon），
// 状态栏本体与"状态栏右键菜单"共用 —— 同一段在两处必须长得一样。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common.h"
#include "icon_glyph.h"
#include "status_segs.h"   // SegId / Seg_Count（段号唯一来源；保留传递包含给既有调用方）
#include "sys_icon.h"
#include "tooltip_host.h"
#include "theme.h"

struct StatusModel {
    std::wstring text[Seg_Count];
    std::wstring tip[Seg_Count];
    int hiddenState = 0;  // 0 全显 / 1 隐系统 / 2 隐全部
    // 当前文件夹的真实系统图标（**借用**：ico::Cached 的进程级缓存，调用方不销毁）。
    // 路径段用它 —— 显示"你正在哪"的不只是文字，还有那一枚文件夹图标。
    HICON pathIcon = nullptr;

    StatusModel() {
        text[Seg_Ai] = L"AI · 空闲";
        tip[Seg_Ai] = L"AI 助手：点击查看最新状态";
    }
};

// 段 -> 字形图标。状态栏绘制与"状态栏右键菜单"共用这一份映射，
// 否则菜单里会出现与栏上完全不同的图标（同一段两个语义）。
ico::IconId StatusSegmentIcon(int seg);

// 段 -> 系统位图语义。状态栏本体与右键菜单共用这一份映射。
// 返回 sysicon::Id::None 表示系统没有对应资源（该段退字形）。
sysicon::Id StatusSegmentSysId(int seg);

// 段 -> 菜单里显示的名字。状态栏右键菜单用（本体不画名字）。
//
// 【为什么要放在这里而不是调用方的静态表】那张表以前漏了一项（Seg_Ai 加入时），
// 缺项在聚合初始化里是**nullptr**，而菜单接口收的是 const std::wstring& ——
// 从 nullptr 构造 std::wstring 会直接崩。于是"状态栏右键菜单一点就崩"。
// 现在名字表与图标表放在同一处（status_bar.cpp），加段必须同时改两处，
// 而漏项会在下面的编译期断言里当场报出来。
const wchar_t* StatusSegmentName(int seg);

class StatusBar {
public:
    void Create(HWND parent, const Theme& theme);
    HWND hwnd() const { return hwnd_; }

    void SetModel(const StatusModel& m);
    const StatusModel& model() const { return model_; }
    void SetEnabledSegments(const std::vector<int>& segs);
    const std::vector<int>& enabledSegments() const { return enabled_; }

    // 单段更新：AI 状态是异步事件，不能等下一次 RefreshStats 才亮。
    void SetSegmentText(int seg, const std::wstring& text, const std::wstring& tip);
    // 段的屏幕矩形：peek 气泡要以状态栏 chip 为锚点，不能另猜一套坐标。
    bool SegmentScreenRect(int seg, RECT* out) const;

    void SetClickFn(std::function<void(int seg)> fn) { onClick_ = std::move(fn); }
    void SetContextFn(std::function<void(POINT)> fn) { onContext_ = std::move(fn); }
    void SetMouseSink(MouseSink fn) { sink_ = std::move(fn); }
    void ReloadTheme();

private:
    // 段的图标解析结果。系统位图优先；系统没有对应资源时退字形。
    // ★只有一处解析 —— 排版（占多宽）与绘制（画哪个）都问它，
    // 两处各写一份迟早漂成"占位宽度按字形算、画出来却是位图"。
    struct SegIcon {
        HICON sys = nullptr;   // 系统位图
        wchar_t glyph = 0;     // 字形兜底
        int slot = 0;          // 占位宽（系统图标槽 / 字形槽），0 = 不画图标
    };

    struct SegLayout {
        int seg = -1;
        RECT rc{};
        RECT textRc{};
        SegIcon icon;
    };

    SegIcon ResolveSegIcon(int seg) const;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Layout(HWND);
    void Paint(HWND);
    void UpdateHover(HWND, int x, int y);
    int HitSegment(int x, int y) const;
    void RebuildTooltip(HWND);

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    ui::TooltipHost tooltip_;
    const Theme* theme_ = nullptr;
    StatusModel model_;
    std::vector<int> enabled_{0, 1, 2, 3, 4, 5, 6};
    std::vector<SegLayout> layout_;
    int hoverSeg_ = -1;
    bool tracking_ = false;

    std::function<void(int)> onClick_;
    std::function<void(POINT)> onContext_;
    MouseSink sink_;
};
