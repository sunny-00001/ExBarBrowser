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

void TabStrip::SetTabs(const std::vector<core::TabRef>& refs, int active) {
    // 上限 256：HICON 是真实分配（非 stock 共享），长会话浏览上百目录会一直涨。
    // 到顶才清，清的是本次不再展示的 path，热路径上图标仍复用。
    if (iconCache_.size() >= 256) PruneIcons(refs);
    items_.clear();
    items_.reserve(refs.size());
    for (const auto& ref : refs) {
        TabStripItem it;
        it.title = ref.title.empty() ? LeafOf(ref.path) : ref.title;
        if (it.title.empty()) it.title = L"新标签";
        it.path = ref.path;
        it.locked = ref.locked;
        it.icon = TabIcon(ref.path);
        items_.push_back(std::move(it));
    }
    active_ = active;
    if (hoverIndex_ >= static_cast<int>(items_.size())) hoverIndex_ = -1;
    // 不重置 scrollX_：切标签/新建标签时保持横向位置，避免溢出条每次跳回最左
    Layout();
    EnsureVisible(active_);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void TabStrip::ReloadTheme() {
    if (!hwnd_) return;
    tooltip_ = ui::TooltipHost();   // 重建（析构销毁旧窗），宽度随 DPI 变
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

LRESULT CALLBACK TabStrip::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<TabStrip>(h);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void TabStrip::NotifySelect(int index) {
    if (parent_) PostMessageW(parent_, WM_APP_TAB_SELECT, static_cast<WPARAM>(index), 0);
}

void TabStrip::NotifyClose(int index) {
    if (parent_) PostMessageW(parent_, WM_APP_TAB_CLOSE, static_cast<WPARAM>(index), 0);
}
