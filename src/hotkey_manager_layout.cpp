// 快捷键管理器：排布与命中。
//
// 【排布与命中必须同住一个文件】它们共用 rowLayout_：画的时候按它画，点的
// 时候按它判。两处各算一遍几何，差一个像素就是"点键位芯片却改了下一行"，
// 而这种错在界面上完全看不出规律，只能靠对着代码数像素。
//
// 【所有尺寸都从 Theme 来】不写死像素：128% / 168% DPI 下写死的 30 会变成
// "行高还是 30 物理像素、字已经 20 像素"——文字被裁一半，而代码里毫无异常。
#include "hotkey_manager.h"

#include <algorithm>

#include "ui_draw.h"

namespace {

// 三栏的基础宽度（96 DPI 逻辑值）。左栏只放筛选项与计数，够放下"系统对话框"
// 四个字 + 图标 + 计数；右栏要放下一个键位芯片与三四行说明。
constexpr int kRailW96 = 190;
constexpr int kDetailW96 = 270;
constexpr int kMidMinW96 = 420;
// 中栏表格的列宽：状态列要放下"系统占用"，芯片列要放下 "Ctrl+Shift+PageDown"。
constexpr int kStatusW96 = 88;
constexpr int kChipW96 = 152;
constexpr int kScopeW96 = 88;
constexpr int kRowH96 = 30;
constexpr int kTableHeadH96 = 26;
constexpr int kStatusBarH96 = 26;
constexpr int kPad96 = 12;

}  // namespace

void HotkeyManager::MinClientSize(const Theme& theme, int* w, int* h) {
    // 最小宽度 = 三栏 + 两条内边距（栏与栏之间不重叠，所以只加 pad 不加边框）。
    if (w) *w = theme.S(kRailW96) + theme.S(kMidMinW96) + theme.S(kDetailW96);
    // 最小高度 = 中栏头 + 表头 + 8 行 + 状态栏 + 上下留白。
    // 8 行不是随手定的：短于这个高度，列表就只剩两三行，"筛选"这个动作
    // 看不到结果，界面等于坏了。
    if (h) *h = theme.S(34) + theme.S(kTableHeadH96) + theme.S(kRowH96) * 8 +
                theme.S(kStatusBarH96) + theme.S(24);
}

int HotkeyManager::RowHeight() const { return theme_->S(kRowH96); }

void HotkeyManager::Layout() {
    if (!hwnd_ || !theme_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int pad = theme_->S(kPad96);
    const int statusH = theme_->S(kStatusBarH96);

    statusBar_ = {0, rc.bottom - statusH, rc.right, rc.bottom};
    const int topH = rc.bottom - statusH;

    colA_ = {0, 0, theme_->S(kRailW96), topH};
    colC_ = {rc.right - theme_->S(kDetailW96), 0, rc.right, topH};
    colB_ = {colA_.right, 0, colC_.left, topH};

    // ---- 左栏：标题 + 筛选项 + 页脚计数 ----
    searchBox_ = {};   // 搜索框在中栏，左栏没有
    railList_ = {colA_.left + theme_->S(6), colA_.top + theme_->S(44), colA_.right - theme_->S(6),
                 colA_.bottom - theme_->S(36)};
    railFoot_ = {colA_.left, colA_.bottom - theme_->S(34), colA_.right, colA_.bottom};

    // ---- 中栏：搜索/录制头 + 表头 + 行 ----
    head_ = {colB_.left + pad, colB_.top + theme_->S(8), colB_.right - pad, colB_.top + theme_->S(42)};
    const int recW = theme_->S(32);
    // 搜索框画在中栏头部左侧，录制按钮贴右边。
    searchBox_ = {head_.left, head_.top, head_.right - recW - theme_->S(6), head_.bottom};
    recordBtn_ = {head_.right - recW, head_.top + theme_->S(3), head_.right, head_.bottom - theme_->S(3)};

    tableHead_ = {colB_.left, head_.bottom + theme_->S(6), colB_.right,
                  head_.bottom + theme_->S(6) + theme_->S(kTableHeadH96)};
    body_ = {colB_.left, tableHead_.bottom, colB_.right, colB_.bottom - theme_->S(6)};

    // 真正的 EDIT 落在画出来的圆角框里。左右各缩 2px 让圆角露出来，否则
    // 矩形的 EDIT 会把四个角切平 —— 看着像"框没画完"。
    if (searchEdit_) {
        const int iconPx = theme_->iconSmall();
        const int lineH = ui::EditLineHeight(nullptr, theme_->font, theme_->S(18));
        RECT inner{searchBox_.left + theme_->S(8) + iconPx + theme_->S(6),
                   searchBox_.top + theme_->S(2), searchBox_.right - theme_->S(2),
                   searchBox_.bottom - theme_->S(2)};
        const RECT line = ui::CenteredLineRect(inner, lineH);
        MoveWindow(searchEdit_, line.left, line.top, std::max(1, static_cast<int>(line.right - line.left)),
                   std::max(1, static_cast<int>(line.bottom - line.top)), TRUE);
    }

    LayoutRows();
    LayoutRight();
}

void HotkeyManager::LayoutRows() {
    if (!theme_) return;
    const int pad = theme_->S(kPad96);
    const int rh = RowHeight();
    const int right = body_.right - pad;
    const int statusW = theme_->S(kStatusW96);
    const int chipW = theme_->S(kChipW96);
    const int scopeW = theme_->S(kScopeW96);

    const int statusL = right - statusW;
    const int chipL = statusL - theme_->S(10) - chipW;
    const int scopeL = chipL - theme_->S(12) - scopeW;
    const int titleL = body_.left + pad + theme_->S(16);   // 16 = 自定义圆点的位置

    if (rowLayout_.size() != view_.size()) rowLayout_.assign(view_.size(), RowLayout{});
    for (int vi = 0; vi < static_cast<int>(view_.size()); ++vi) {
        const int top = body_.top + vi * rh - scroll_;
        RowLayout& rl = rowLayout_[static_cast<size_t>(vi)];
        rl.row = {body_.left, top, body_.right, top + rh};
        rl.title = {titleL, top, scopeL - theme_->S(8), top + rh};
        rl.scope = {scopeL, top, scopeL + scopeW, top + rh};
        rl.chip = {chipL, top + theme_->S(4), chipL + chipW, top + rh - theme_->S(4)};
        rl.status = {statusL, top, right, top + rh};
    }

    // 滚动范围：内容总高减视口高。少算一次就会"滚到底还差半行"，
    // 或者"内容早没了还能继续滚"（滚出大片空白）。
    const int total = static_cast<int>(view_.size()) * rh;
    const int viewH = body_.bottom - body_.top;
    scrollMax_ = std::max(0, total - viewH);
    scroll_ = std::min(scroll_, scrollMax_);
    scroll_ = std::max(0, scroll_);
}

void HotkeyManager::LayoutRight() {
    if (!theme_) return;
    const int pad = theme_->S(kPad96);
    detailBox_ = {colC_.left + pad, colC_.top + theme_->S(10), colC_.right - pad,
                  colC_.bottom - theme_->S(10)};

    const int l = detailBox_.left;
    const int r = detailBox_.right;
    const int lineH = theme_->S(20);
    int y = detailBox_.top;

    detail_.title = {l, y, r, y + theme_->S(46)};   // 两行
    y = detail_.title.bottom + theme_->S(2);
    detail_.cmd = {l, y, r, y + lineH};
    y = detail_.cmd.bottom + theme_->S(14);
    detail_.keyLabel = {l, y, r, y + lineH};
    y = detail_.keyLabel.bottom + theme_->S(4);
    detail_.keyChip = {l, y, r, y + theme_->S(30)};
    y = detail_.keyChip.bottom + theme_->S(10);
    detail_.meta = {l, y, r, y + lineH};
    y = detail_.meta.bottom + theme_->S(2);
    detail_.defaultKey = {l, y, r, y + lineH};

    // 四颗按钮自下而上叠：还原全部（破坏性，最靠下、离主操作最远）/
    // 还原此项 / 清除 / 编辑。主操作放最上面，因为它是这个面板 90% 的动作。
    const int btnH = theme_->S(28);
    const int gap = theme_->S(6);
    detail_.resetAll = {l, detailBox_.bottom - btnH, r, detailBox_.bottom};
    detail_.reset = {l, detail_.resetAll.top - gap - btnH, r, detail_.resetAll.top - gap};
    detail_.clear = {l, detail_.reset.top - gap - btnH, r, detail_.reset.top - gap};
    detail_.edit = {l, detail_.clear.top - gap - btnH, r, detail_.clear.top - gap};
    detail_.hint = {l, detail_.edit.top - theme_->S(8) - theme_->S(32), r,
                    detail_.edit.top - theme_->S(8)};
    // 冲突说明吃掉中间剩下的空间（可能三四行）。
    detail_.conflict = {l, detail_.defaultKey.bottom + theme_->S(10), r,
                        std::max(detail_.defaultKey.bottom + theme_->S(10), detail_.hint.top - theme_->S(6))};
}

int HotkeyManager::HitGroup(int x, int y) const {
    if (!theme_) return -1;
    const POINT pt{x, y};
    if (!PtInRect(&railList_, pt)) return -1;
    const int rh = theme_->S(32);
    const int idx = (y - railList_.top) / rh;
    if (idx < 0 || idx >= static_cast<int>(groups_.size())) return -1;
    return idx;
}

int HotkeyManager::HitRow(int x, int y) const {
    const POINT pt{x, y};
    if (!PtInRect(&body_, pt)) return -1;
    const int rh = RowHeight();
    const int vi = (y - body_.top + scroll_) / rh;
    if (vi < 0 || vi >= static_cast<int>(view_.size())) return -1;
    return vi;
}

int HotkeyManager::HitDetail(int x, int y) const {
    const POINT pt{x, y};
    if (PtInRect(&detail_.edit, pt)) return 1;
    if (PtInRect(&detail_.clear, pt)) return 2;
    if (PtInRect(&detail_.reset, pt)) return 3;
    if (PtInRect(&detail_.resetAll, pt)) return 4;
    return 0;
}

void HotkeyManager::ScrollBy(int delta) {
    if (!theme_) return;
    const int step = RowHeight() * (delta > 0 ? -1 : 1) * 3;
    const int before = scroll_;
    scroll_ = std::max(0, std::min(scrollMax_, scroll_ + step));
    if (scroll_ != before) {
        LayoutRows();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void HotkeyManager::EnsureVisible(int viewIndex) {
    if (!theme_ || viewIndex < 0) return;
    const int rh = RowHeight();
    const int top = viewIndex * rh;
    const int viewH = body_.bottom - body_.top;
    if (top < scroll_) {
        scroll_ = top;
    } else if (top + rh > scroll_ + viewH) {
        scroll_ = top + rh - viewH;
    }
    scroll_ = std::max(0, std::min(scrollMax_, scroll_));
    LayoutRows();
}
