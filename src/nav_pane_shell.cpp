// 导航树的“外壳消息层”：内部树子类化、shell/注册表变更触发的根集重发现、
// 以及结构刷新时的展开状态快照。
//
// 【为什么单独成文件】这三件事都由“窗口消息 + COM 通知”驱动，改动理由是
// “外部事件怎么变成树的变化”；nav_pane.cpp 则是控件生命周期/几何/主题。
// 【为什么在这里显示空白菜单】NSTC 的回调栈里跑模态弹层会重入 shell，实测卡死。
// 现在事件侧只交出一个“空 IContextMenu”让 NSTC 结束自己的流程，再用
// WM_APP_NAV_BLANK_MENU 把真正的 ui::Menu::Show 推迟到正常消息循环；那条路径与
// 书签栏右键菜单逐像素同源（同一个 PopupList 窗口、同一份 Theme）。
#include "nav_pane.h"

#include "commands.h"
#include "log.h"
#include "menu.h"
#include "nav_pane_root.h"

#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>

namespace navpane {

namespace {

// SHChangeNotifyRegister 的 wMsg 必须在 WM_USER..0x7FFF（不能用 WM_APP），
// 且通知发给宿主而不是顶层窗口：宿主过程的 this 指针是现成的，不必再做
// HWND -> App 的全局反查。0x55 与工程里其它 WM_USER 用途不冲突。
constexpr UINT kNavShellChangeMsg = WM_USER + 0x55;
constexpr UINT_PTR kNavRefreshTimer = 1;
constexpr UINT kNavRefreshDelayMs = 300;
constexpr size_t kMaxRestoredNodes = 256;
constexpr int kMaxTraverseDepth = 24;

// 只订阅"会改变导航树结构或文件视图内容"的事件。
//
// 两组事件的处理不同（见 HandleHostMessage）：
//   · kNavShellEvents —— 目录结构变化，重建已展开节点 + 根集重发现；
//   · kViewShellEvents —— 文件级增删改，回投主窗口刷新文件视图。
// 分开是为了让文件级的频繁事件不去反复推后导航树的 300ms 合并定时器。
constexpr DWORD kNavShellEvents =
    SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEFOLDER | SHCNE_UPDATEDIR |
    SHCNE_ASSOCCHANGED | SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED |
    SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED | SHCNE_NETSHARE | SHCNE_NETUNSHARE;
constexpr DWORD kViewShellEvents =
    SHCNE_CREATE | SHCNE_DELETE | SHCNE_UPDATEITEM | SHCNE_RENAMEITEM |
    SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEFOLDER | SHCNE_UPDATEDIR;

// shell 通知说明“文件夹内容变了”；explorer 侧边栏项的增删则走注册表监听
// （nav_pane_root.cpp）。两条路最终都重新发现根集并投同一个消息给 App。
void RequestRootRediscovery(HWND host) {
    HWND root = GetAncestor(host, GA_ROOT);
    if (!root) return;
    navpane::RefreshDefaultsAsync([root] {
        if (IsWindow(root)) PostMessageW(root, WM_APP_NAV_ROOTS_READY, 0, 0);
    });
}

}  // namespace

// ------------------------------------------------------------ 展开状态快照

std::vector<std::wstring> NavPane::CaptureExpandedPaths() const {
    std::vector<std::wstring> out;
    if (!tree_) return out;
    auto addUnique = [&out](const std::wstring& p) {
        if (p.empty() || out.size() >= kMaxRestoredNodes) return;
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    auto walk = [&](auto&& self, IShellItem* item, int depth) -> void {
        if (!item || depth > kMaxTraverseDepth || out.size() >= kMaxRestoredNodes) return;
        NSTCITEMSTATE state = NSTCIS_NONE;
        if (SUCCEEDED(tree_->GetItemState(item, NSTCIS_EXPANDED, &state)) &&
            (state & NSTCIS_EXPANDED)) {
            addUnique(PathOfItem(item));
        }
        IShellItem* child = nullptr;
        if (SUCCEEDED(tree_->GetNextItem(item, NSTCGNI_CHILD, &child)) && child) {
            self(self, child, depth + 1);
            child->Release();
        }
        IShellItem* next = nullptr;
        if (SUCCEEDED(tree_->GetNextItem(item, NSTCGNI_NEXT, &next)) && next) {
            self(self, next, depth);
            next->Release();
        }
    };
    ComPtr<IShellItemArray> roots;
    if (SUCCEEDED(tree_->GetRootItems(&roots)) && roots) {
        DWORD n = 0;
        if (SUCCEEDED(roots->GetCount(&n))) {
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* root = nullptr;
                if (SUCCEEDED(roots->GetItemAt(i, &root)) && root) {
                    walk(walk, root, 0);
                    root->Release();
                }
            }
        }
    }
    return out;
}

void NavPane::RestoreExpandedPaths(const std::vector<std::wstring>& paths) {
    if (!tree_ || paths.empty()) return;
    // 与 RebuildRoots 同一条理由：SetItemState(NSTCIS_EXPANDED) 会在调用栈内
    // 同步抛 OnItemClick，不能让它把活动窗格带走。
    const bool prev = suppressClick_;
    suppressClick_ = true;
    size_t restored = 0;
    // 捕获顺序是前序：父在子前，正好满足 NSTC"先找到父才能展开子"的要求。
    for (const auto& path : paths) {
        IShellItem* item = ItemFromPath(path);
        if (!item) continue;
        HRESULT hr = tree_->SetItemState(item, NSTCIS_EXPANDED, NSTCIS_EXPANDED);
        if (FAILED(hr)) hr = tree_->EnsureItemVisible(item);
        if (SUCCEEDED(hr)) ++restored;
        item->Release();
    }
    suppressClick_ = prev;
    LogDbg(L"[navpane] 恢复导航树展开状态：%d/%d 个节点",
           static_cast<int>(restored), static_cast<int>(paths.size()));
}

// ------------------------------------------------------------ 内部树子类化
//
// 只处理绘制/滚动与安全的窗口销毁。右键完全留给 NSTC：这里拦截会重入 shell。
LRESULT CALLBACK NavPane::TreeSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR id, DWORD_PTR refData) {
    // 【WM_NCDESTROY 必须单独处理 —— 这是崩溃的根因】
    // 内部树是 NSTC 建的：RemoveAllRoots 之后它可能被**销毁并重建**，句柄也会被
    // 回收给别人。此时如果还拿着旧句柄去 RemoveWindowSubclass / 再挂一次，
    // comctl32 就会在野窗口上动链子 —— 实测直接崩在 comctl32.dll。
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &NavPane::TreeSubclassProc, id);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_PAINT) {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (refData) {
            auto* self = reinterpret_cast<NavPane*>(refData);
            self->PaintSeparatorLines(hwnd, nullptr);
        }
        return r;
    }
    if (msg == WM_PRINTCLIENT) {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (refData) {
            auto* self = reinterpret_cast<NavPane*>(refData);
            HDC dc = reinterpret_cast<HDC>(wp);
            self->PaintSeparatorLines(hwnd, dc);
        }
        return r;
    }
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_VSCROLL:
    case WM_HSCROLL:
    case WM_MOUSEWHEEL:
        if (refData) reinterpret_cast<NavPane*>(refData)->OnTreeGeometryChanged();
        break;
    default:
        break;
    }
    return r;
}

void NavPane::HookTreeScroll() {
    HWND tv = FindSysTree();
    if (!tv) return;
    if (treeHooked_ == tv) return;
    treeHooked_ = nullptr;
    if (SetWindowSubclass(tv, &NavPane::TreeSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
        treeHooked_ = tv;
    }
}

void NavPane::UnhookTreeScroll() {
    // 子类链随窗口一起消失，不要对可能已死的窗口 RemoveWindowSubclass。
    treeHooked_ = nullptr;
}

void NavPane::OnTreeGeometryChanged() {
    if (HWND tv = FindSysTree()) InvalidateRect(tv, nullptr, FALSE);
}

// ------------------------------------------------------------ 宿主窗口消息

LRESULT CALLBACK NavPane::HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<NavPane*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleHostMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT NavPane::HandleHostMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kNavShellChangeMsg: {
        LONG event = 0;
        PIDLIST_ABSOLUTE* pidls = nullptr;
        HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp),
                                                static_cast<DWORD>(lp), &pidls, &event);
        if (!lock) {
            ScheduleShellRefresh();
            RequestRootRediscovery(hwnd);
            if (HWND root = GetAncestor(hwnd, GA_ROOT))
                PostMessageW(root, WM_APP_VIEW_AUTO_REFRESH, 0, 0);
            return 0;
        }
        if (event & kNavShellEvents) {
            ScheduleShellRefresh();       // 已展开节点的即时重建
            RequestRootRediscovery(hwnd); // 根集（快速访问/命名空间）也重发现
        }
        // 文件/目录内容变化 → 回投主窗口刷新文件视图（M7）。
        // 只投一条消息，App 侧用合并定时器去抖；不在这里直接刷，
        // 因为 NavPane 不能反向依赖 App，且批量操作会连发几十条通知。
        if (event & kViewShellEvents) {
            HWND root = GetAncestor(hwnd, GA_ROOT);
            if (root) PostMessageW(root, WM_APP_VIEW_AUTO_REFRESH, 0, 0);
        }
        SHChangeNotification_Unlock(lock);
        return 0;
    }
    case WM_TIMER:
        if (wp == kNavRefreshTimer) {
            if (refreshTimer_) KillTimer(hwnd, refreshTimer_);
            refreshTimer_ = 0;
            RefreshExpandedItems();
            return 0;
        }
        break;
    case WM_APP_NAV_BLANK_MENU:
        // lParam 是发出右键时的屏幕坐标；此时 NSTC 的事件栈已经退出。
        ShowBlankMenu(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 空白区菜单内容与书签栏右键菜单走同一个构建器、同一个展示器：
// ui::Menu::Show -> ShowPopupList。差别只在动作号映射到命令总线。
void NavPane::ShowBlankMenu(POINT screen) {
    if (!ok() || !theme_ || !host_) return;

    enum : int { kRefresh = 0, kExpandAll = 1 };
    ui::Menu menu;
    menu.Add(L"刷新", kRefresh, ico::IconId::Refresh).Cmd(CmdId::NavRefresh);
    menu.Add(L"展开所有文件夹", kExpandAll, ico::IconId::Folder)
        .Cmd(CmdId::NavTreeExpandAll);

    ui::MenuOptions opt;
    opt.toggleKey = L"nav.blank";   // 同一锚点连点第二下只收起，不重开
    const int action = menu.Show(host_, *theme_, screen, opt);
    if (action < 0) return;

    CmdId id = CmdId::None;
    if (action == kRefresh) id = CmdId::NavRefresh;
    if (action == kExpandAll) id = CmdId::NavTreeExpandAll;
    if (id == CmdId::None) return;
    CommandBus::Get().Invoke(id, Args::Empty(), Actor::Ui());
}

// ------------------------------------------------------------ shell 变更刷新

void NavPane::Refresh() {
    if (!ok()) return;
    RefreshExpandedItems();
    RequestRootRediscovery(host_);
}

void NavPane::RegisterShellChanges() {
    if (shellReg_ || !host_) return;
    SHChangeNotifyEntry entry{};
    entry.pidl = nullptr;
    entry.fRecursive = TRUE;
    shellReg_ = SHChangeNotifyRegister(
        host_, SHCNRF_InterruptLevel | SHCNRF_ShellLevel | SHCNRF_RecursiveInterrupt |
                   SHCNRF_NewDelivery,
        static_cast<LONG>(kNavShellEvents | kViewShellEvents), kNavShellChangeMsg, 1, &entry);
    if (!shellReg_) {
        shellReg_ = SHChangeNotifyRegister(host_, SHCNRF_ShellLevel | SHCNRF_NewDelivery,
                                           static_cast<LONG>(kNavShellEvents | kViewShellEvents),
                                           kNavShellChangeMsg, 1, &entry);
    }
    if (!shellReg_) LogDbg(L"[navpane] SHChangeNotifyRegister 失败，自动刷新不可用");
}

void NavPane::UnregisterShellChanges() {
    if (shellReg_) {
        SHChangeNotifyDeregister(shellReg_);
        shellReg_ = 0;
    }
    if (refreshTimer_ && host_) KillTimer(host_, refreshTimer_);
    refreshTimer_ = 0;
}

void NavPane::ScheduleShellRefresh() {
    if (!tree_ || !host_ || refreshing_ || refreshTimer_) return;
    refreshTimer_ = SetTimer(host_, kNavRefreshTimer, kNavRefreshDelayMs, nullptr);
    if (!refreshTimer_) RefreshExpandedItems();
}

void NavPane::RefreshExpandedItems() {
    if (!tree_ || refreshing_) return;
    refreshing_ = true;

    // 只重建"已经物化且展开过的"节点。折叠目录的子项在 NSTC 里根本没建，
    // 重建它们没有任何可见收益，反而会把网盘/搜索类节点全枚举一遍。
    const std::vector<std::wstring> expanded = CaptureExpandedPaths();
    if (!expanded.empty()) {
        const bool prev = suppressClick_;
        suppressClick_ = true;
        const std::wstring oldSelection = selection_;
        RebuildRoots();
        RestoreExpandedPaths(expanded);
        selection_.clear();
        if (!oldSelection.empty()) SyncTo(oldSelection);
        suppressClick_ = prev;
        RepaintTreeColorsFromCache();
        ApplyLayoutToControl(false);
    }
    refreshing_ = false;
}

}  // namespace navpane
