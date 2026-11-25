// 状态栏右端的「图标大小」滑块（实现说明见 status_zoom_slider.h）。
//
// 三件事：画轨道与滑块、把鼠标 x 换算成档位、在按下/松开之间维护拖动状态。
// "改了图标大小之后界面怎么响应"不在这里 —— 那是 App 的事（走 view.zoom.set
// 命令总线），这里只负责"用户把滑块拖到了哪一档"。
#include "status_zoom_slider.h"

#include "config.h"        // Config::Get().s().statusBarTooltipWidth
#include "platform_win.h"
#include "ui_draw.h"

#include <algorithm>

namespace {

// 轨道两端各留的空白（96 DPI 逻辑值）。滑块中心在轨道内首/末档的位置，
// 两端留白让"拖到最左"不会把滑块画到窗口外面去。
constexpr int kTrackPad96 = 6;
// 滑块手柄直径（96 DPI）。
constexpr int kThumb96 = 12;
// 轨道厚度（96 DPI）。
constexpr int kTrackThick96 = 4;

int NearestStopIndex(int px) {
    int best = 0;
    int bestDelta = -1;
    for (int i = 0; i < kZoomIconStopCount; ++i) {
        const int d = kZoomIconStops[i] > px ? kZoomIconStops[i] - px : px - kZoomIconStops[i];
        if (bestDelta < 0 || d < bestDelta) {
            best = i;
            bestDelta = d;
        }
    }
    return best;
}

}  // namespace

int StatusZoomSlider::WidthPx(const Theme& theme) {
    // 96 DPI 下 120px 宽：8 个档位各占 15px，拇指 12px —— 拖得动也不至于太宽。
    // 窄窗口下 App 会把滑块整体收起（见 App::Layout），所以这里不必自适应。
    return theme.S(120);
}

bool StatusZoomSlider::Create(HWND parent, const Theme& theme, const wchar_t* cls) {
    if (!cls) return false;
    if (!win::RegisterClassOnce(cls, StatusZoomSlider::WndProc)) return false;
    parent_ = parent;
    theme_ = &theme;
    hwnd_ = win::CreateChild(cls, parent, this, theme.barStatus(), nullptr,
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    return hwnd_ != nullptr;
}

void StatusZoomSlider::ReloadTheme() {
    if (!hwnd_) return;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateTooltip(hwnd_);
}

void StatusZoomSlider::SetIconPx(int px) {
    const int idx = NearestStopIndex(px);
    const bool changed = (idx != stopIdx_) || (px != iconPx_);
    iconPx_ = px;
    stopIdx_ = idx;
    if (changed && hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateTooltip(hwnd_);
    }
}

void StatusZoomSlider::SetStopIndex(int idx, bool notify) {
    if (idx < 0) idx = 0;
    if (idx >= kZoomIconStopCount) idx = kZoomIconStopCount - 1;
    const int px = kZoomIconStops[idx];
    if (idx == stopIdx_ && px == iconPx_) return;
    stopIdx_ = idx;
    iconPx_ = px;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateTooltip(hwnd_);
    }
    // 回调在改完内部状态之后才发：回调方（App）通常会回头读 iconPx()，
    // 顺序反了会读到旧值。
    if (notify && onChange_) onChange_(iconPx_);
}

RECT StatusZoomSlider::TrackRect() const {
    RECT rc{};
    if (!hwnd_) return rc;
    GetClientRect(hwnd_, &rc);
    const int pad = theme_ ? theme_->S(kTrackPad96) : 6;
    const int thick = theme_ ? theme_->S(kTrackThick96) : 4;
    rc.left = pad;
    rc.right = rc.right - pad;
    rc.top = (rc.bottom - rc.top - thick) / 2;
    rc.bottom = rc.top + thick;
    return rc;
}

int StatusZoomSlider::ThumbCenterX(int idx) const {
    const RECT tr = TrackRect();
    if (tr.right <= tr.left) return tr.left;
    if (kZoomIconStopCount <= 1) return tr.left;
    const double t = static_cast<double>(idx) / (kZoomIconStopCount - 1);
    return tr.left + static_cast<int>((tr.right - tr.left) * t + 0.5);
}

int StatusZoomSlider::IndexFromX(int x) const {
    const RECT tr = TrackRect();
    if (tr.right <= tr.left) return 0;
    if (x <= tr.left) return 0;
    if (x >= tr.right) return kZoomIconStopCount - 1;
    const double t = static_cast<double>(x - tr.left) / (tr.right - tr.left);
    int idx = static_cast<int>(t * (kZoomIconStopCount - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= kZoomIconStopCount) idx = kZoomIconStopCount - 1;
    return idx;
}

void StatusZoomSlider::Paint(HWND h) {
    if (!theme_) return;
    ui::PaintBuffered(h, theme_->bar, [&](HDC mem, const RECT& rc) {
        const RECT tr = TrackRect();
        if (tr.right <= tr.left) return;
        // 槽底用 **line（分隔线色）**，不用 bg / barAlt。
        //
        // 【为什么】深浅两套调色板里 bar 与 bg 都是 viewBg（同一令牌）——
        // 用它们画槽，槽就与状态栏底**完全同色**，等于没画（实测：整条轨道
        // 隐形，只剩一个蓝点漂在栏里）。line 在深色下 0x3F3F3F、浅色下
        // 0xD0D0D0，两套都从底色里分得出来。
        ui::FillRound(mem, tr, theme_->line, theme_->S(3));
        // 已选区间：accent。
        const int cx = ThumbCenterX(stopIdx_);
        RECT fill = tr;
        if (cx > tr.left) {
            fill.right = cx;
            ui::FillRound(mem, fill, theme_->accent, theme_->S(3));
        }
        // 拇指：accent 圆点；悬停/拖动时放大一点（拖动反馈）。
        const int d = theme_->S(hot_ || dragging_ ? kThumb96 + 2 : kThumb96);
        RECT th{cx - d / 2, (rc.top + rc.bottom) / 2 - d / 2, cx - d / 2 + d,
                (rc.top + rc.bottom) / 2 - d / 2 + d};
        ui::FillRound(mem, th, theme_->accent, d / 2);
        // 拇指上再点一个更亮的芯，纯装饰：让它在深色底上更像个"把手"。
        const int c = std::max(2, d / 4);
        RECT core{cx - c / 2, (rc.top + rc.bottom) / 2 - c / 2, cx - c / 2 + c,
                  (rc.top + rc.bottom) / 2 - c / 2 + c};
        ui::FillRound(mem, core, theme_->selSolidFg, c / 2);
    });
}

void StatusZoomSlider::UpdateTooltip(HWND h) {
    if (!h || !theme_) return;
    const int maxW = std::clamp(Config::Get().s().statusBarTooltipWidth, 120, 1200);
    tooltip_.Attach(h, theme_->S(maxW));
    RECT rc{};
    GetClientRect(h, &rc);
    wchar_t buf[64];
    swprintf_s(buf, L"图标大小：%d 像素（拖动调节）", iconPx_);
    std::vector<ui::TooltipHost::Item> items;
    ui::TooltipHost::Item it;
    it.rc = rc;
    it.text = buf;
    items.push_back(std::move(it));
    tooltip_.SetTools(items);
}

LRESULT StatusZoomSlider::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_MOUSEMOVE: {
        if (!hot_) {
            hot_ = true;
            InvalidateRect(h, nullptr, FALSE);
            UpdateTooltip(h);
        }
        if (dragging_) {
            SetStopIndex(IndexFromX(GET_X_LPARAM(l)), true);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        hot_ = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        dragging_ = true;
        SetCapture(h);
        // 按下即跳到那一档：不用"先抓住拇指再拖"（那样用户会以为没反应）。
        SetStopIndex(IndexFromX(GET_X_LPARAM(l)), true);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (dragging_) {
            dragging_ = false;
            // 释放捕获前先撤：带着捕获销毁/换窗口会让系统把捕获交给别的窗口。
            ReleaseCapture();
            SetStopIndex(IndexFromX(GET_X_LPARAM(l)), true);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        // 捕获被别人抢走（窗口失活等）：结束拖动，别让它一直处于"拖动中"。
        dragging_ = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        // 横向拖动的光标（不是箭头）：一眼就知道这能拖。
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return TRUE;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK StatusZoomSlider::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    StatusZoomSlider* self = nullptr;
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<StatusZoomSlider*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StatusZoomSlider*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->Handle(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}
