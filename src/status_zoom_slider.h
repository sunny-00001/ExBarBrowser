// 状态栏右端的「图标大小」滑块（自绘，无子控件）。
//
// 【为什么单独成一个窗口，而不是状态栏的一个"段"】状态栏的段模型是
// "文字 + 图标 + 气泡"，每段占一个矩形、点一下触发一个动作。滑块要的是
// "按住拖、值连续变"——塞进段模型就得给它开一堆特例（拖动态、连续值、
// 特殊命中形状），而段模型会被这些特例污染（排版、命中、气泡三处都要开分支）。
// 独立成一个子窗口，它的输入只有"拖动 → 值"，边界清楚得多。
//
// 【为什么值是离散的档位而不是连续像素】图标像素连续可调没有意义：shell 只认
// 若干尺寸，落在档位之间的值会被它自己归到最近的一档 —— 于是滑块显示"37px"
// 而实际是 32px，看起来就是"滑块不准"。档位表与视图菜单同源（viewmode），
// 下标即契约。
//
// 【与 Ctrl+滚轮的双向同步】滚轮改大小 → App 读回真实像素 → 回灌给滑块；
// 拖滑块 → App 调 view.zoom.set → shell 落值 → 再回读校准。任一侧都只有一个
// 真源（shell 里的真实像素），滑块自己那份只是显示。
#pragma once
#include <functional>
#include <windows.h>

#include "common.h"
#include "theme.h"
#include "tooltip_host.h"

// 图标大小档位（滑块的离散取值）。
//
// 【为什么不直接用 viewmode::kModes】那张表的档位是"视图模式"（列表/详细信息/
// 平铺…），每档带一个 FVM；滑块只管**图标像素**，不管视图模式。两张表会分叉
// （视图加一档不等于缩放加一档），所以滑块自己列一份像素档位 —— 只此一份。
inline constexpr int kZoomIconStops[] = {16, 24, 32, 48, 64, 96, 128, 256};
inline constexpr int kZoomIconStopCount =
    static_cast<int>(sizeof(kZoomIconStops) / sizeof(kZoomIconStops[0]));

class StatusZoomSlider {
public:
    // 滑块在状态栏行里占多宽（App 布局要拿它给状态栏让位）。
    static int WidthPx(const Theme& theme);

    bool Create(HWND parent, const Theme& theme, const wchar_t* cls);
    HWND hwnd() const { return hwnd_; }

    // 值变化时回调（参数 = 图标像素）。拖动过程中会连续回调。
    void SetOnChange(std::function<void(int)> fn) { onChange_ = std::move(fn); }

    // 外部同步：视图模式 / 滚轮改了图标大小后，把真实像素回灌进来。
    // 非档位值会落到**最近**的档位（滑块的位置只能是档位），但 tooltip
    // 显示的仍是真实像素 —— 滑块不撒谎。
    void SetIconPx(int px);
    int iconPx() const { return iconPx_; }
    int stopIndex() const { return stopIdx_; }

    void ReloadTheme();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Paint(HWND h);
    void UpdateTooltip(HWND h);
    void SetStopIndex(int idx, bool notify);
    // 鼠标 x → 档位下标（按轨道两端对齐到第一/最后一档）。
    int IndexFromX(int x) const;
    // 档位下标 → 滑块中心的屏幕 x。
    int ThumbCenterX(int idx) const;
    RECT TrackRect() const;

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    const Theme* theme_ = nullptr;
    ui::TooltipHost tooltip_;
    int iconPx_ = kZoomIconStops[2];
    int stopIdx_ = 2;
    bool dragging_ = false;
    bool hot_ = false;
    // 是否已向系统登记"鼠标离开时通知我"（TrackMouseEvent 是一次性的：
    // 收到 WM_MOUSELEAVE 后系统就不再跟踪，必须重新登记）。
    bool tracking_ = false;
    std::function<void(int)> onChange_;
};
