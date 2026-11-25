// 组间分隔线的绘制实现（原理见 nav_pane_sep.h 顶部）。
#include "nav_pane_sep.h"

#include "ui_draw.h"

namespace navpane {

void DrawSeparatorLines(HDC dc, const std::vector<int>& ys, int width, int height,
                        COLORREF color) {
    if (!dc || width <= 0 || height <= 0 || ys.empty()) return;
    if (color == CLR_INVALID) return;
    for (const int y : ys) {
        if (y < 0 || y >= height) continue;
        // FillRect 画的是 [left, right) x [top, bottom)，所以 bottom = y+1 正好 1px。
        // 画刷不自己建：实心画刷的唯一入口是 ui::FillRectColor（画刷由它创建并销毁）。
        const RECT r{kSepLeftInset, y, width, y + 1};
        ui::FillRectColor(dc, r, color);
    }
}

}  // namespace navpane
