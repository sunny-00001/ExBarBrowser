// 标签栏组件的生命周期与模型装配：创建窗口、吃 core::TabRef、图标缓存、消息转发。
//
// 拆片约定（ARCHITECTURE-RULES N2）：绘制在 tab_strip_paint、命中/鼠标在 tab_strip_input、
// 几何在 tab_strip_layout、菜单在 tab_strip_actions。本文件不画也不命中。
#include "tab_strip.h"

#include "platform_win.h"

#include "icon_resolve.h"

namespace {

}  // namespace

void TabStrip::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kTabStripClass, TabStrip::WndProc, CS_DBLCLKS);
    parent_ = parent;
    theme_ = &theme;
    hwnd_ = win::CreateChild(kTabStripClass, parent, this, theme.S(kTabBarHeightDp));
    if (!hwnd_) return;
}

TabStrip::~TabStrip() { ReleaseIcons(); }

void TabStrip::ReleaseIcons() {
    for (auto& kv : iconCache_)
        if (kv.second) DestroyIcon(kv.second);
    iconCache_.clear();
    // items_ 里的 icon 是指向上面这些句柄的裸指针，销毁后必须置空，避免退出前再画一次。
    for (auto& it : items_) it.icon = nullptr;
}

HICON TabStrip::TabIcon(const std::wstring& path) {
    if (path.empty()) return nullptr;
    auto cached = iconCache_.find(path);
    if (cached != iconCache_.end()) return cached->second;
    // 统一图标识别：此电脑/回收站/侧边栏/云存储等命名空间取准确图标。
    HICON icon = ico::Resolve(path, true);
    iconCache_.emplace(path, icon);
    return icon;
}

// 清缓存只能清"本次不展示"的 path：items_ 里已填好的 icon 就是这些指针，
// 一把 clear 会让正在画的标签引用已 DestroyIcon 的句柄（原实现在 App 里就有这个洞）。
void TabStrip::PruneIcons(const std::vector<core::TabRef>& refs) {
    for (auto it = iconCache_.begin(); it != iconCache_.end();) {
        const bool inUse = std::any_of(refs.begin(), refs.end(),
                                       [&](const core::TabRef& r) { return r.path == it->first; });
        if (inUse) {
            ++it;
            continue;
        }
        if (it->second) DestroyIcon(it->second);
        it = iconCache_.erase(it);
    }
}

void TabStrip::SetTabs(const std::vector<core::TabRef>& refs, int active, bool canReopen) {
    // 上限 256：HICON 是真实分配（非 stock 共享），长会话浏览上百目录会一直涨。
    // 到顶才清，清的是本次不再展示的 path，热路径上图标仍复用。
    if (iconCache_.size() >= 256) PruneIcons(refs);
    items_.clear();
    items_.reserve(refs.size());
    // 路径集先摊平一次：重复判定是 O(n²) 的两两比较，但 n 是标签数（几十），
    // 且只在 SetTabs 时算一次 —— 比菜单弹出时现遍历更省，也让判定口径唯一。
    std::vector<std::wstring> paths;
    paths.reserve(refs.size());
    for (const auto& ref : refs) paths.push_back(ref.path);
    for (size_t i = 0; i < refs.size(); ++i) {
        const core::TabRef& ref = refs[i];
        TabStripItem it;
        // 显示名唯一口径在 core::DisplayTitle（改名 > Shell 名 > 路径末级）。
        it.title = core::DisplayTitle(ref);
        if (it.title.empty()) it.title = L"新标签";
        it.path = ref.path;
        it.locked = ref.locked;
        it.icon = TabIcon(ref.path);
        it.colorIdx = ref.color;
        // 颜色换算只在这里做一次，绘制处直接用结果。
        it.colorFill = theme_->TabColorFill(ref.color);
        it.colorBar = theme_->TabColorBar(ref.color);
        it.hasDuplicate = core::PathRepeated(paths, i);
        items_.push_back(std::move(it));
    }
    active_ = active;
    canReopen_ = canReopen;
    // rapidClose 状态由 NotifyClose 设置、鼠标离开时取消，跨 SetTabs 保留 ——
    // 这正是"关闭后鼠标不动、连续点下一个关闭钮"能成立的前提。
    if (hoverIndex_ >= static_cast<int>(items_.size())) hoverIndex_ = -1;
    // 不重置 scrollX_：切标签/新建标签时保持横向位置，避免溢出条每次跳回最左
    Layout();
    EnsureVisible(active_);
    // 标签集变了才需要全量重建 tooltip（文本/数量都会变）。
    RebuildTooltip(hwnd_);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void TabStrip::ReloadTheme() {
    if (!hwnd_) return;
    tooltip_ = ui::TooltipHost();   // 重建（析构销毁旧窗），宽度随 DPI 变
    // DPI 变了，系统小图标的真实尺寸也变了（实测 96dpi=16px、120dpi=20px）。
    // ico::Resolve 按 GetSystemMetrics(SM_CXSMICON) 取图，缓存里留着旧 DPI 的
    // HICON 会让标签图标与新令牌体系再次脱节 —— 整批丢弃、按新 DPI 重取。
    ReleaseIcons();                 // 同时把 items_[].icon 置空
    for (auto& it : items_) it.icon = TabIcon(it.path);
    // 标签颜色是按主题算好的（深/浅两套色值），主题换了必须重算 ——
    // 否则切到浅色后彩色标签还是深色底上那档，对比度直接不对。
    // 只重算颜色，不碰 items_ 的其余字段（SetTabs 可能还没被调用）。
    for (auto& it : items_) {
        it.colorFill = theme_->TabColorFill(it.colorIdx);
        it.colorBar = theme_->TabColorBar(it.colorIdx);
    }
    Layout();
    // DPI 变了，tooltip 的换行宽度也变了，必须全量重建。
    RebuildTooltip(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

LRESULT CALLBACK TabStrip::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<TabStrip>(h);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void TabStrip::CancelDrag(HWND h, bool releaseCapture) {
    StopAutoScroll();
    closeDownIdx_ = -1;
    if (!dragging_ && dragIdx_ < 0) return;
    dragIdx_ = -1;
    dragging_ = false;
    dropIdx_ = -1;
    if (releaseCapture && GetCapture() == h) ReleaseCapture();
    if (h) InvalidateRect(h, nullptr, FALSE);
}

void TabStrip::NotifySelect(int index) {
    if (parent_) PostMessageW(parent_, WM_APP_TAB_SELECT, static_cast<WPARAM>(index), 0);
}

void TabStrip::CancelRapidClose() {
    if (rapidWidths_.empty()) return;
    rapidWidths_.clear();
    rapidCloseIndex_ = -1;
    if (hwnd_) {
        Layout();
        UpdateTooltipRects();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void TabStrip::NotifyClose(int index) {
    // 只有"按在关闭钮上"的那次点击才进入快速关闭模式；菜单/热键关闭不进入，
    // 因为那时鼠标根本不在标签栏上，保留关闭钮位置没有意义。
    const bool fromStrip = hoverIndex_ == index &&
                           (hoverKind_ == kClose || hoverKind_ == kTab);
    if (fromStrip && index >= 0 &&
        index < static_cast<int>(tabRects_.size()) &&
        index < static_cast<int>(items_.size()) && !items_[static_cast<size_t>(index)].locked &&
        index < static_cast<int>(lastWidths_.size())) {
        // 保存关闭前的实际宽度表。Layout 在 rapid 模式下会跳过这一个索引，
        // 其余标签保持原宽 —— 下一个标签的关闭钮就落在原来的鼠标位置。
        rapidWidths_ = lastWidths_;
        rapidCloseIndex_ = index;
    }
    if (parent_) PostMessageW(parent_, WM_APP_TAB_CLOSE, static_cast<WPARAM>(index), 0);
}
