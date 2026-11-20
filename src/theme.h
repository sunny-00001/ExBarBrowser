// 主题：颜色令牌 + 字体 + 缩放，唯一视觉来源（对应 ExBar 的 tokens.css）
#pragma once
#include "common.h"

struct Theme {
    bool dark = true;
    int dpi = 96;

    COLORREF bg{};      // 内容区/视图底
    COLORREF bar{};     // 工具栏底
    COLORREF barAlt{};  // 输入框/次级底
    COLORREF hover{};
    COLORREF pressed{};
    COLORREF line{};
    COLORREF text{};
    COLORREF sub{};
    COLORREF disabled{};
    COLORREF chevron{};
    COLORREF accent{};
    COLORREF editBg{};
    COLORREF editBorder{};
    COLORREF listBg{};
    COLORREF listHot{};
    COLORREF listSel{};

    // 96 DPI 逻辑圆角（用 S() 换算）。以前只有导航栏按钮有圆角（写死在
    // nav_bar_draw.cpp 的私有 FillRound 里），其余栏是直角 —— 统一到这里。
    int radiusBtn96 = 4;
    int radiusPopup96 = 6;

    HBRUSH brBg{};
    HBRUSH brBar{};
    HBRUSH brBarAlt{};
    HBRUSH brHover{};
    HBRUSH brPressed{};
    HBRUSH brEdit{};
    HBRUSH brList{};
    HBRUSH brListHot{};
    HBRUSH brListSel{};
    HBRUSH brAccent{};
    HBRUSH brViewBg{};   // 视图区容器窗口类的背景刷（比窗口活得久）

    HFONT font{};
    HFONT fontBold{};

    // 把一个"纯背景窗口"（视图区容器）的类背景刷换成本主题的 bg。
    // 以前 app.cpp 的 OnCreate 与 app_layout.cpp 的 ApplyTheme 各写一遍
    // CreateSolidBrush + SetClassLongPtrW，且都要自己管 viewBg_ 的释放。
    // 窗口类持有的画刷必须比窗口活得久 → 由省 Theme 唯一持有，随 Reload 一起换。
    void ApplyToViewArea(HWND area);

    // dpi 变化时整体重建（先 Shutdown 再 Init）
    // dpiOverride > 0 时用它（WM_DPICHANGED 的 wParam 才是权威值）；0 则自动探测。
    void Init(HWND hwnd, int dpiOverride = 0);
    void Shutdown();
    void Reload(HWND hwnd, int dpiOverride = 0);

    int S(int px) const { return MulDiv(px, dpi, 96); }
    // 令牌版：圆角由 Theme 统一给出，绘制处不再写裸像素。
    int radiusBtn() const { return S(radiusBtn96); }
    int radiusPopup() const { return S(radiusPopup96); }
};

// 进程级深色模式（uxtheme 未文档化导出，Win10 1903+）。
// 不调用它时，IExplorerBrowser 托管的 Shell 视图会固定浅色（实测白底）。
// 取不到导出就静默降级为"仅工具栏深色"，不影响功能。
void ApplyProcessDarkMode(bool dark);
void AllowDarkForWindow(HWND hwnd, bool dark);
