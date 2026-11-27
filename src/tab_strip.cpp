// 标签栏组件的生命周期与模型装配：创建窗口、吃 core::TabRef、图标缓存、消息转发。
//
// 拆片约定（ARCHITECTURE-RULES N2）：绘制在 tab_strip_paint、命中/鼠标在 tab_strip_input、
// 几何在 tab_strip_layout、菜单在 tab_strip_actions。本文件不画也不命中。
#include "tab_strip.h"

#include "tab_uia.h"
#include "platform_win.h"

#include "icon_resolve.h"

void TabStrip::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kTabStripClass, TabStrip::WndProc, CS_DBLCLKS);
    parent_ = parent;
    theme_ = &theme;
    // 用运行值而不是编译期基准：tabBarHeight 可在设置里改，Create 时就要按新高度建窗。
    hwnd_ = win::CreateChild(kTabStripClass, parent, this, theme.barTab());
    if (!hwnd_) return;
}

TabStrip::~TabStrip() {
    uia::Release(hwnd_);   // 释放 UIA 根 provider（客户端仍持有的引用由 COM 计数管理）
    ReleaseIcons();
}

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
    // 新装填了条目：通知缓存涨了（SetTabs 用它决定要不要顺手清理，
    // 避免没涨也白扫一遍缓存 —— 长会话里 PruneIcons 每次全量 O(n·m)）。
    iconCacheGrew_ = true;
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

void TabStrip::SetTabs(const std::vector<core::TabRef>& refs, int active, bool canReopen,
                       const std::vector<std::wstring>& closed) {
    // 上限 256：HICON 是真实分配（非 stock 共享），长会话浏览上百目录会一直涨。
    // 只有缓存确实新装填过才值得清一次（grew 标志由 TabIcon 置位）；
    // 清的是本次不再展示的 path，热路径上图标仍复用。
    if (iconCacheGrew_ && iconCache_.size() >= 256) {
        PruneIcons(refs);
        iconCacheGrew_ = false;
    }
    // 让位动画的起点快照：必须在 items_/tabRects_ 被覆盖**之前**取。
    // 只有"规模没变"才可能是一次重排（增删标签各有既有路径，rapid close 还要
    // 冻结宽度）；StartReorderAnim 内部还会再核对 id 顺序真的变了才起表。
    std::vector<int> oldOrder;
    std::map<int, int> oldLeft;
    if (!items_.empty() && items_.size() == tabRects_.size()) {
        oldOrder.reserve(items_.size());
        for (size_t i = 0; i < items_.size(); ++i) {
            oldOrder.push_back(items_[i].id);
            oldLeft[items_[i].id] = static_cast<int>(tabRects_[i].left);
        }
    }
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
        // id 是动画与浮层的身份键（见 TabStripItem::id 的说明）：重排后下标全体
        // 位移，只有 id 能稳定配对"这一格原来在哪"。
        it.id = ref.id;
        // 显示名唯一口径在 core::DisplayTitle（改名 > Shell 名 > 路径末级）。
        it.title = core::DisplayTitle(ref);
        if (it.title.empty()) it.title = L"新标签";
        it.path = ref.path;
        it.locked = ref.locked;
        it.pinned = ref.pinned;
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
    closed_ = closed;
    // rapidClose 状态由 NotifyClose 设置、鼠标离开时取消，跨 SetTabs 保留 ——
    // 这正是"关闭后鼠标不动、连续点下一个关闭钮"能成立的前提。
    // 不重置 scrollX_：切标签/新建标签时保持横向位置，避免溢出条每次跳回最左
    Layout();
    EnsureVisible(active_);
    // 让位动画：此刻 tabRects_ 已是最终位置，oldLeft 是重排前的位置，
    // StartReorderAnim 按 id 配对算每格要"从哪滑过来"。顺序没变时它自己返回。
    StartReorderAnim(oldOrder, oldLeft);
    // hover 重建必须在 Layout() **之后**：UpdateHover 的命中测试读 tabRects_，
    // 先重建等于拿旧矩形命中。也不按下标夹取 —— 布局重算后矩形全体位移，
    // 旧 hoverIndex_ 指的可能是另一个标签；按当前光标重新命中才是对的
    // （快速关闭能连续点，正是靠"重排后关闭钮仍在光标下"，hover 必须跟布局走）。
    if (hwnd_) {
        POINT cur{};
        if (GetCursorPos(&cur)) {
            ScreenToClient(hwnd_, &cur);
            UpdateHover(hwnd_, cur.x, cur.y);
        } else {
            hoverKind_ = kNone;
            hoverIndex_ = -1;
        }
    } else {
        hoverIndex_ = -1;
    }
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
    // 【无条件清全部瞬态】三条路都调这里：捕获丢失、Esc 取消、鼠标规则接管按下。
    // 曾有早退判据 `if (!dragging_ && dragIdx_ < 0) return;`，会让"只剩 dropIdx_ /
    // tearOff_ 残留"的那次漏清 —— 下一次 MOUSEMOVE 带着旧落点进拖拽分支，
    // 表现就是"没在拖却画着插入线 / 松手搬错标签"。清是幂等的，早退才有害。
    const bool had = dragging_ || dragIdx_ >= 0 || dropIdx_ >= 0 || tearOff_;
    closeDownIdx_ = -1;
    dragIdx_ = -1;
    dragId_ = -1;
    dragging_ = false;
    dropIdx_ = -1;
    tearOff_ = false;   // 取消即退出脱离态：否则下次进栏会带着"移到新窗口"的提示
    if (releaseCapture && GetCapture() == h) ReleaseCapture();
    if (h && had) {
        InvalidateRect(h, nullptr, FALSE);
        RebuildTooltip(h);   // 拖动开始时 Clear 过：Esc/CAPTURECHANGED 取消时恢复
    }
}

void TabStrip::NotifySelect(int index) {
    // 【传 id 不传下标】消息是 PostMessage 入队的，而鼠标输入消息先于它被处理：
    // 队列里可能已经排着另一条"关掉某标签"，那条一旦先执行，集合就整体左移，
    // 这条携带的下标就指向了**另一个**标签。id 是稳定身份，App 侧用
    // TabSet::IndexOf(id) 现查下标，重排/关闭都不会错位。
    if (!parent_) return;
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const int id = items_[static_cast<size_t>(index)].id;
    PostMessageW(parent_, WM_APP_TAB_SELECT, static_cast<WPARAM>(id), 0);
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
    if (parent_) {
        // 【传 id 不传下标】同 NotifySelect：PostMessage 入队后下标可能已失效。
        // 连续中键关两个相邻标签时，两条消息若都带"关闭前"的下标，第一条关掉后
        // 集合左移，第二条就会关掉隔一个的那个标签。id 在 TabSet 里稳定，
        // 反查下标即可。越界时取不到 id，用 -1 表示"无效"，App 侧直接忽略。
        const int id = (index >= 0 && index < static_cast<int>(items_.size()))
                           ? items_[static_cast<size_t>(index)].id
                           : -1;
        PostMessageW(parent_, WM_APP_TAB_CLOSE, static_cast<WPARAM>(id), 0);
    }
}
