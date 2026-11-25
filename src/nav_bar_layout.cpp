// 导航栏布局：按钮/地址框/搜索框/面包屑段的矩形只在这里算。
//
// 【为什么排布单独成文件】绘制（nav_bar_draw.cpp）与命中（nav_bar_hit.cpp）
// 读的是同一批矩形：按钮 b.rc / b.arrow、地址 fieldRc_、下拉 dropRc_、
// moreRc_、segRects_ / chevRects_。两处各算一遍，差一个像素就是
// "点后退却打开了前进历史"，而截图看起来完全正常。
#include "nav_bar.h"

#include <algorithm>

#include "config.h"
#include "nav_metrics.h"
#include "platform_win.h"
#include "settings_bounds.h"
#include "ui_draw.h"

void NavBar::Layout() {
    if (!hwnd_ || !theme_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);

    const int btn = theme_->S(kBtn96);
    const int gap = theme_->S(kGap96);
    int x = theme_->S(kX96);
    int by = (rc.bottom - btn) / 2;
    for (auto& b : buttons_) {
        b.rc = {x, by, x + btn, by + btn};
        if (b.hasArrow) b.arrow = {b.rc.right - theme_->S(kArrow96), b.rc.top, b.rc.right, b.rc.bottom};
        x = b.rc.right + gap;
    }

    // 文字行高按栏字体实测（偶数化），与设置对话框同源。
    int lineH = theme_->S(18);
    if (HDC dc = GetDC(hwnd_)) {
        lineH = ui::EditLineHeight(dc, theme_->font, lineH);
        ReleaseDC(hwnd_, dc);
    }

    // 框高独立（28px@96DPI）：量的是边框；偶数框高+偶数行高 → 上下留白严格相等。
    int fieldH = theme_->S(kFieldH96);
    if (fieldH & 1) ++fieldH;
    const int cy = rc.bottom / 2;                 // 按钮与字段共用中心线
    const int fy = cy - fieldH / 2;
    const int fieldLeft = (buttons_.empty() ? theme_->S(kX96) : buttons_.back().rc.right) + gap +
                          theme_->S(2);
    const int fieldRight = rc.right - theme_->S(8);

    // 搜索框（常驻）从右侧切一段走，地址框让出宽度。
    // 顺序很关键：先算搜索框、再算地址框，否则地址框的右缘会盖在搜索框下面
    // （EDIT 子窗口的 z 序不保证，看起来像"地址框压住了搜索框一半"）。
    //
    // 宽度固定（对齐 mod 的 SEARCH_W=200），窗口太窄时**整体收起**：
    // 为了留住搜索框把地址框压到 160px 以下去，坏的是每天用几十次的那个。
    // 搜索框宽度与地址框最小宽都来自设置（kSearchW96/kAddrMin96 只是默认值）。
    // 夹一次防止测试/AI 绕过注册表直接写 Settings；设置面板与命令路径的正式夹取
    // 在 config_io/settings_reg 里，这里是最后一道几何防线。
    const Settings& s = Config::Get().s();
    const int searchWant = theme_->S(std::clamp(
        s.navBarSearchWidth, kNavBarSearchWidthMin, kNavBarSearchWidthMax));
    const int addrMin = theme_->S(std::clamp(
        s.navBarAddressMinWidth, kNavBarAddressMinWidthMin, kNavBarAddressMinWidthMax));
    int searchW = 0;
    if (search_) {
        if (fieldRight - fieldLeft - searchWant - theme_->S(kGap96) >= addrMin) searchW = searchWant;
    }
    const int addrRight = fieldRight - (searchW ? searchW + theme_->S(kGap96) : 0);
    fieldRc_ = {fieldLeft, fy, addrRight, fy + fieldH};
    dropRc_ = {fieldRc_.right - theme_->S(kPad96) - theme_->S(kDrop96), fieldRc_.top,
               fieldRc_.right - theme_->S(kPad96), fieldRc_.bottom};
    LayoutSearch(fieldRight, fy, fieldH, lineH);

    // EDIT 只占文字行并居中：拉满整框会让文字贴顶，选中态与面包屑错位。
    if (edit_) {
        const RECT lineRc = ui::CenteredLineRect(fieldRc_, lineH);
        int eW = fieldRc_.right - fieldRc_.left - theme_->S(kDrop96) - theme_->S(kPad96) - 1;
        MoveWindow(edit_, fieldRc_.left + 1, lineRc.top, eW > 0 ? eW : 0, lineRc.bottom - lineRc.top, TRUE);
    }

    segRects_.clear();
    chevRects_.clear();
    // 段矩形重建后，旧的悬停段号可能指向不存在的段（或多段折叠后指错），先清掉。
    hotSeg_ = -1;
    hotChev_ = -1;
    firstShown_ = 0;
    moreRc_ = {fieldRc_.left + theme_->S(kLpad96) + theme_->S(kLead96), fieldRc_.top,
               fieldRc_.left + theme_->S(kLpad96) + theme_->S(kLead96) + theme_->S(kSep96),
               fieldRc_.bottom};
    if (editing_) return;

    const auto& parts = model_.parts();
    if (parts.empty()) return;

    // 量字走唯一原语（ui::MeasureText 内部选字体并还原）
    HDC dc = GetDC(hwnd_);
    const int sepW = theme_->S(kSep96);
    std::vector<int> widths(parts.size(), 0);
    int total = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        widths[i] = ui::MeasureText(dc, theme_->font, parts[i].label) + sepW;
        total += widths[i];
    }
    const int startX = moreRc_.right;
    const int avail = dropRc_.left - startX;
    while (total > avail && firstShown_ + 1 < static_cast<int>(parts.size())) {
        total -= widths[static_cast<size_t>(firstShown_)];
        ++firstShown_;
    }

    int cx = startX;
    const int clipR = dropRc_.left;
    // firstShown_ 恒 >= 0（只在上面的循环里 ++），转到 size_t 是明确的非负转换。
    for (size_t i = static_cast<size_t>(firstShown_); i < parts.size(); ++i) {
        if (cx >= clipR) break;
        int labelW = widths[i] - sepW;
        if (cx + labelW > clipR) labelW = clipR - cx;
        segRects_.push_back({cx, fieldRc_.top, cx + labelW, fieldRc_.bottom});
        chevRects_.push_back({cx + labelW, fieldRc_.top, cx + widths[i], fieldRc_.bottom});
        cx += widths[i];
    }

    ReleaseDC(hwnd_, dc);
    // 按钮矩形刚刚重算：提示热区必须跟着走，否则窗口缩放后气泡落在旧位置
    // （表现是"鼠标停在按钮上没提示，停在旁边的空白处却提示"）。
    RebuildTooltip();
}
