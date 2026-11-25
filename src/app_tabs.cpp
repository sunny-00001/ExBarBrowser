// 标签生命周期（App 的编排分片）。
//
// 只管"模型 + 视图"的成对变化：core::TabSet 记顺序/锁定，App::tabs_ 持视图与历史；
// 画与命中全在 TabStrip，本文件不碰 GDI。改这里只有一个理由：标签怎么开、关、切。
#include "app.h"
#include "str.h"
#include "text_file.h"   // tf::Exists（长路径安全的存在性判断，规则：不直连 GetFileAttributesW）

#include "icon_resolve.h"

namespace {

// SyncChrome 的指纹只记"外壳真正会画出来的输入"。字段带长度前缀，
// 避免路径/别名里出现分隔符时把两段拼成同一种签名。
void AppendChromeSigText(std::wstring& sig, const std::wstring& value) {
    sig.append(std::to_wstring(value.size()));
    sig.push_back(L':');
    sig.append(value);
    sig.push_back(L';');
}

void AppendChromeSigInt(std::wstring& sig, int value) {
    sig.append(std::to_wstring(value));
    sig.push_back(L';');
}

void AppendChromeSigBool(std::wstring& sig, bool value) {
    sig.push_back(value ? L'1' : L'0');
    sig.push_back(L';');
}

std::wstring MakeChromeSignature(const core::TabSet& tabSet, int activeId, int activeIndex,
                                 int activePane, bool activeRefFound,
                                 const std::wstring& windowTitle, const std::wstring& navPath,
                                 const std::wstring& navDisplay, const NavBar::NavState& ns,
                                 bool navPaneOk, bool tabStripReady, bool navBarReady) {
    size_t reserve = 256;
    for (const auto& ref : tabSet.items()) {
        reserve += ref.path.size() + ref.title.size() + ref.alias.size() + 64;
    }
    for (const auto& path : tabSet.closed()) reserve += path.size() + 16;
    reserve += windowTitle.size() + navPath.size() + navDisplay.size() + 64;

    std::wstring sig;
    sig.reserve(reserve);
    // 控件/导航树的就绪态也进签名：若首次 SyncChrome 早于它们创建，
    // 就绪后的那次 SyncChrome 必须再刷一次，不能被旧签名挡掉。
    AppendChromeSigBool(sig, navPaneOk);
    AppendChromeSigBool(sig, tabStripReady);
    AppendChromeSigBool(sig, navBarReady);
    AppendChromeSigBool(sig, activeRefFound);
    AppendChromeSigInt(sig, activeId);
    AppendChromeSigInt(sig, activeIndex);
    AppendChromeSigInt(sig, activePane);
    AppendChromeSigText(sig, windowTitle);
    // 标签条：标题、图标路径、锁/固定、着色与重复标记全由 TabRef 派生。
    AppendChromeSigInt(sig, static_cast<int>(tabSet.items().size()));
    for (const auto& ref : tabSet.items()) {
        AppendChromeSigInt(sig, ref.id);
        AppendChromeSigText(sig, ref.path);
        AppendChromeSigText(sig, ref.title);
        AppendChromeSigText(sig, ref.alias);
        AppendChromeSigInt(sig, ref.color);
        AppendChromeSigBool(sig, ref.locked);
        AppendChromeSigBool(sig, ref.pinned);
    }
    // 重开菜单读的是完整关闭列表，不只是"有没有"。
    AppendChromeSigInt(sig, static_cast<int>(tabSet.closed().size()));
    for (const auto& path : tabSet.closed()) AppendChromeSigText(sig, path);
    // 地址栏/面包屑与导航按钮可用态。
    AppendChromeSigText(sig, navPath);
    AppendChromeSigText(sig, navDisplay);
    AppendChromeSigBool(sig, ns.canBack);
    AppendChromeSigBool(sig, ns.canForward);
    AppendChromeSigBool(sig, ns.canUp);
    return sig;
}

}  // namespace

// ---------------------------------------------------------------- 标签
bool App::PathOpenable(const std::wstring& path) {
    if (path.empty()) return false;
    // 虚拟命名空间（此电脑/回收站/控制面板…）与 search-ms: 之类没有文件系统
    // 语义，不能拿 GetFileAttributes 判存在 —— 一律放行，交给 Shell。
    if (nav::IsNamespace(path)) return true;
    if (path.rfind(L"shell:", 0) == 0) return true;
    return tf::Exists(path);
}

int App::NewTab(const std::wstring& path, int index, bool activate) {
    std::wstring target = path.empty() ? HomePath() : path;
    auto tab = std::make_unique<Tab>();
    // 后台模式：从建第一个视图起就抑制它的"被激活"自报，否则 shell 在
    // EnsurePaneView 内的焦点回调会把新标签当成用户点的标签切过去。
    if (!activate) backgroundTab_ = tab.get();
    // 两个 Pane 先就位：右窗格只准备路径/历史，Shell 视图按需创建。
    // 双栏里左右目录可以不同，因此不能共用一个 nav::History。
    Pane* left = PaneAt(tab.get(), 0);
    Pane* rightPane = PaneAt(tab.get(), 1);
    left->nav.Reset(target);
    rightPane->nav.Reset(target);
    if (!EnsurePaneView(tab.get(), 0)) {
        LogDbg(L"[tab] 创建视图失败：%s", target.c_str());
        backgroundTab_ = nullptr;
        return 0;
    }
    left = PaneAt(tab.get(), 0);  // EnsurePaneView 可能触发回调；重取指针

    std::wstring initial = target;
    if (!left->view->path().empty()) initial = left->view->path();
    // Shell 可能把 target 规范化（大小写/尾斜杠）：历史栈与标签身份必须用同一个结果。
    left->nav.Reset(initial);
    if (str::IEquals(rightPane->path(), target)) rightPane->nav.Reset(initial);
    // 显示名跟着**窗格**走（Pane 拆分后 display 在 Pane 上，Tab 上没有了）。
    left->display = ico::DisplayName(initial, left->view->display());

    const int id = tabSet_.Add(initial, left->display, index);
    tab->id = id;
    ShellView* created = left->view.get();
    tabs_[id] = std::move(tab);

    for (const auto& locked : Config::Get().s().lockedPaths)
        if (str::IEquals(locked, initial)) tabSet_.SetLocked(id, true);
    // 固定状态也按路径恢复：否则"固定了几个常用目录"重启后全部失效。
    // 放在 SetLocked 之后、Activate 之前：SetPinned 会调整顺序，
    // 新标签本身就在固定区末尾，通常是无操作。
    for (const auto& pinned : Config::Get().s().pinnedPaths)
        if (str::IEquals(pinned, initial)) tabSet_.SetPinned(id, true);

    // 改名/颜色按路径持久化：恢复会话时同一个目录应当还是用户给它起的名字与颜色，
    // 否则"重命名 + 设色"在重启后全部丢失（那等于没做这两个功能）。
    std::wstring alias;
    int color = 0;
    Config::Get().GetTabMeta(initial, &alias, &color);
    if (!alias.empty()) tabSet_.SetAlias(id, alias);
    if (color) tabSet_.SetColor(id, color);

    // 新标签套用持久化的视图偏好（模式/排序/分组/窗格），而不是每次回到默认。
    // 必须等首个视图就绪后再调：Create() 内部的首个导航可能还没产出 IShellView。
    ApplyPersistedViewState(created);
    // 右窗格视图交给 Activate 按需创建；这里只恢复路径/历史（会话恢复时最后统一建活动标签）。

    if (activate) {
        Activate(id);
    } else {
        // 后台打开：标签已入 tabSet_、视图已导航，但不能切走当前视图。
        // Activate 里唯一的"显示 + 焦点 + activeId_"就是这三件，全部跳过；
        // 标签条仍要刷新（用户看得见新标签），几何也要排好（点它时才不会闪一下 0 尺寸）。
        if (left->view) left->view->SetVisible(false);
    }
    backgroundTab_ = nullptr;
    if (!activate) {
        SyncChrome();  // 标签条要看得见新标签，但不能让它切走当前视图
        Layout();
    }
    MarkSessionDirty();   // 2s 去抖落盘：崩溃也不丢刚开的标签
    return id;
}

int App::NewTabIndex() const {
    // 【一个取值 = 一种行为】以前 "right" 与 "after_current" 走同一行代码
    // （ActiveIndex()+1），也就是界面上的"最右"从来没生效过。
    // 现在：after_current 插在当前标签右侧；其余（end，以及读入时已归一的
    // 旧值 right）追加到最右。
    if (str::IEquals(Config::Get().s().tabNewPos.c_str(), L"after_current"))
        return ActiveIndex() + 1;
    return tabSet_.Count();
}

bool App::CloseTab(int id) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return false;
    if (ref->locked) {
        // 原因要跟着失败一起回传：调用方（AI/脚本/命令面板）拿到的不该只是
        // 笼统的"命令执行失败"——它无法区分"标签已锁定"和"实现出错"，
        // 也就无法决定下一步（解锁后重试，还是放弃）。
        CommandBus::Get().NoteFailure(L"标签页已锁定，未关闭");
        MessageBeep(MB_ICONWARNING);
        return false;   // 锁定标签没被关掉 —— 必须让调用方知道
    }
    // 焦点显式化：在销毁视图**之前**记录"焦点是否落在被关标签的视图里"。
    // 销毁后 GetFocus() 会变成 null 或父窗口，Activate 原来的 focusInView 判据
    // 恰好能搬回焦点纯属侥幸（null 走 !focus 分支）；把判据提前、显式搬回，
    // 才能不被"关闭路径上多一次 SetFocus/SendMessage"之类的改动悄悄破坏。
    HWND focus = GetFocus();
    Tab* closing = ById(id);
    bool focusInClosingView = false;
    if (closing) {
        for (const auto& pane : closing->panes) {
            if (pane && pane->view && pane->view->hwnd() &&
                (focus == pane->view->hwnd() || IsChild(pane->view->hwnd(), focus))) {
                focusInClosingView = true;
                break;
            }
        }
    }
    const int index = tabSet_.IndexOf(id);
    std::wstring path = ref->path;
    tabSet_.PushClosed(path);

    // 【先摘下来、藏起来，再延后销毁】探针 84：ShellView::Destroy = 20~69 ms per view
    // （IExplorerBrowser::Destroy + Release + DestroyWindow），关一个标签会同步销毁
    // 1~2 个 → 20~138 ms 卡顿。用户对"关闭"的期望是立刻消失，清理可以稍后做。
    // 顺序不能换（错了就是 use-after-free 或漏销毁）：move 出 unique_ptr -> 隐藏窗口
    // -> 移进 pendingDestroy_ -> tabs_.erase（已不含视图，所以很快）-> PostMessage。
    if (closing) {
        for (auto& pane : closing->panes) {
            if (!pane || !pane->view) continue;
            // 隐藏是立刻生效的那一步：只 ShowWindow(SW_HIDE)，不碰 shell 内部状态。
            if (HWND vh = pane->view->hwnd()) ShowWindow(vh, SW_HIDE);
            pendingDestroy_.push_back(std::move(pane->view));
        }
        if (!pendingDestroy_.empty()) PostMessageW(hwnd_, WM_APP_RECLAIM_VIEWS, 0, 0);
    }

    tabs_.erase(id);
    tabSet_.Remove(id);

    if (tabSet_.Count() == 0) {
        OnDestroy();
        DestroyWindow(hwnd_);
        return true;
    }
    if (activeId_ == id) {
        int next = std::min(index, tabSet_.Count() - 1);
        Activate(tabSet_.IdAt(next));
        // 焦点原本在刚销毁的视图里 → 必须显式还给新活动视图。
        // 不靠 Activate 里"focus 为空就搬"的隐式行为：那 behavior 依赖销毁次序，
        // 换个关闭入口（命令/AI）就可能落空。
        if (focusInClosingView) FocusActiveView();
    } else {
        SyncChrome();
    }
    Layout();
    MarkSessionDirty();
    return true;
}

int App::CloseOthers(int keepId) {
    // 必须先收集再关：CloseTab 会 erase items_，边遍历边关会让迭代器失效。
    std::vector<int> ids;
    int skipped = 0;
    for (const auto& t : tabSet_.items()) {
        if (t.id == keepId) continue;
        // 先过滤锁定/固定标签：CloseTab 会逐个 MessageBeep，批量关闭时那是一串提示音。
        if (t.locked || t.pinned) { ++skipped; continue; }
        ids.push_back(t.id);
    }
    int closed = 0;
    for (int id : ids) if (CloseTab(id)) ++closed;
    if (closed || skipped) Activate(keepId);
    if (skipped) {
        LogDbg(L"[tab] 关闭其他：跳过 %d 个锁定标签", skipped);
        MessageBeep(MB_ICONWARNING);   // 只响一次，用户能数清"有几个没关"
    }
    return closed;
}

bool App::ReopenClosed(int index) {
    const auto& closed = tabSet_.closed();
    if (index < 0 || index >= static_cast<int>(closed.size())) return false;
    const std::wstring path = closed[static_cast<size_t>(index)];
    // 先建标签、成功后才弹栈：否则路径已失效时（目录被删/盘符下线）
    // 关闭历史会被白白消耗掉，用户再点也没有第二条可试。
    if (!PathOpenable(path)) {
        CommandBus::Get().NoteFailure(L"无法重新打开：" + path + L"（目录可能已不存在）");
        return false;
    }
    if (!NewTab(path)) {
        CommandBus::Get().NoteFailure(L"无法重新打开：" + path + L"（视图创建失败）");
        return false;
    }
    tabSet_.PopClosedAt(index, nullptr);
    return true;
}

int App::CloseSide(int id, bool left) {
    const int index = tabSet_.IndexOf(id);
    if (index < 0) return 0;
    // 同样先收集：CloseTab 会改变 items_ 的下标。
    std::vector<int> ids;
    int skipped = 0;
    for (int i = 0; i < tabSet_.Count(); ++i) {
        if (!(left ? (i < index) : (i > index))) continue;
        const core::TabRef* t = tabSet_.Find(tabSet_.IdAt(i));
        if (t && (t->locked || t->pinned)) { ++skipped; continue; }
        ids.push_back(tabSet_.IdAt(i));
    }
    int closed = 0;
    for (int t : ids) if (CloseTab(t)) ++closed;
    if (closed || skipped) Activate(id);
    if (skipped) {
        LogDbg(L"[tab] 关闭%s：跳过 %d 个锁定标签", left ? L"左侧" : L"右侧", skipped);
        MessageBeep(MB_ICONWARNING);
    }
    return closed;
}

void App::Activate(int id) {
    if (!tabSet_.Find(id)) return;
    // 非循环激活 = 用户重新选了一个标签：结束 MRU 循环快照。
    // 循环内部的 Activate 由 mruCycling_ 标记，不能在这里清掉快照。
    if (!mruCycling_) tabSet_.ResetMruCycle();
    activeId_ = id;
    const bool dual = Config::Get().s().dualPane;
    // 双栏下右视图按需建：切到哪个标签才建那个标签的右视图，
    // 会话恢复期间由 Run() 在最后统一建活动标签的那一份。
    //
    // 【首帧之前不建】双栏启动时建第二个 ShellView 实测 375 ms（IExplorerBrowser
    // 的 CoCreateInstance + Initialize + 首次导航），这段落在窗口可见之前，
    // 用户只感觉到"启动慢"，右栏对他第一眼没有任何贡献。启动期跳过，
    // 由 Run 在首帧之后经 WM_APP_STARTUP_SETTLE 补建（见 app.h 的 startupPending_）。
    // 非启动路径（用户点标签/开双栏）照旧同步建 —— 那时窗口已经可见，不能拖。
    if (dual && !restoringSession_ && !startupPending_) {
        if (Tab* activeTab = ById(id)) (void)EnsurePaneView(activeTab, 1);
    }
    for (auto& kv : tabs_) {
        Tab* t = kv.second.get();
        const bool tabActive = kv.first == id;
        Pane* left = PaneAt(t, 0);
        Pane* right = PaneAt(t, 1);
        if (left && left->view) left->view->SetVisible(tabActive);
        if (right && right->view) right->view->SetVisible(tabActive && dual);
    }
    ApplyPaneChrome();
    tabSet_.Touch(id);
    // 每个标签都有自己的活动窗格；激活时按它的活动窗格刷新显示身份。
    SyncTabIdentity(ById(id));
    viewMode_ = ActiveViewModeIndex();
    SyncChrome();
    // 只有焦点原本在视图区（或还没有焦点）时才搬焦点。从标签栏点过来时保留在标签栏，
    // 否则"标签栏"作用域的裸键（Shift+←/→/Home/End）永远收不到。
    //
    // 【窗口还不可见时不要设焦点 —— 这是启动慢 344 ms 的直接原因】
    // 启动路径是 NewTab -> Activate -> 这里，而 ShowWindow 在更后面。给一个
    // **隐藏窗口**设焦点会让消息链一路走到 ShellView::ActivateFocus()：
    //   SetFocus -> WM_SETFOCUS -> view->UIActivate(SVUIA_ACTIVATE_FOCUS)
    // UIActivate 要等 shell 视图把菜单/命令栏/选中态全激活，实测同步阻塞 344 ms
    // （埋点：[+359] GetFocus -> [+703] Activate 返回），而这段时间窗口还没画出来 ——
    // 用户看到的是"双击后一秒才出窗口"。
    // 隐藏窗口本来也不该持有焦点，所以判据是 IsWindowVisible 而不是"启动中"：
    // 任何给不可见窗口设焦点的调用都是无意义的（用户看不见，键盘也收不到）。
    // 启动时的首次焦点由 Run() 在 ShowWindow 之后补一次 FocusActiveView()。
    HWND focus = GetFocus();
    const bool focusInView =
        !focus || focus == viewArea_ || (viewArea_ && IsChild(viewArea_, focus));
    const bool windowVisible = IsWindowVisible(hwnd_) != 0;
    HWND target = ActiveShellViewHost() ? ActiveShellViewHost()->hwnd() : nullptr;
    if (focusInView && windowVisible) {
        if (target) SetFocus(target);
        else if (viewArea_) SetFocus(viewArea_);
    }
    MarkSessionDirty();   // sessionActive 变了
}

void App::MoveTab(int id, int toIndex) {
    tabSet_.Move(id, toIndex);
    SyncChrome();
    Layout();
    MarkSessionDirty();
}

// 锁定状态的唯一写入点。**按值设定**而不是"切换"，是因为命令侧要能
// 表达"确保它是锁定的"（幂等），而切换语义下重复调用会来回翻转 ——
// 调两次等于没调，机器侧无法表达"我要它锁着"。
// 菜单仍然表达"切换"，但它只是这一个的一种用法（读现值再取反）。
void App::SetTabLocked(int id, bool locked) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    if (ref->locked == locked) return;   // 已是目标值：不写盘、不刷界面
    std::wstring path = ref->path;
    tabSet_.SetLocked(id, locked);
    auto& list = Config::Get().mutableSettings().lockedPaths;
    if (locked) {
        if (std::find(list.begin(), list.end(), path) == list.end()) list.push_back(path);
    } else {
        list.erase(std::remove(list.begin(), list.end(), path), list.end());
    }
    Config::Get().Save();
    SyncChrome();
}

void App::ToggleLock(int id) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    SetTabLocked(id, !ref->locked);
}

void App::SetTabPinned(int id, bool pinned) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    if (ref->pinned == pinned) return;   // 已是目标值：不写盘、不刷界面
    const std::wstring path = ref->path;
    tabSet_.SetPinned(id, pinned);
    auto& list = Config::Get().mutableSettings().pinnedPaths;
    if (pinned) {
        if (std::find(list.begin(), list.end(), path) == list.end()) list.push_back(path);
    } else {
        list.erase(std::remove(list.begin(), list.end(), path), list.end());
    }
    Config::Get().Save();
    SyncChrome();
}

void App::TogglePin(int id) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    SetTabPinned(id, !ref->pinned);
}

bool App::CycleMru(bool forward) {
    if (tabSet_.Count() < 2) return true;   // 幂等：一个标签时无事可做
    const int id = tabSet_.CycleMru(forward ? 1 : -1);
    if (!id) return false;
    mruCycling_ = true;          // 保护循环快照不被 Activate 重置
    Activate(id);
    mruCycling_ = false;
    return true;
}

// 改名/颜色的落盘口径只有一份：写进 Config 的内存态（落盘由退出流程统一 Save）。
void App::PersistTabMeta(int id) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    Config::Get().SetTabMeta(ref->path, ref->alias, ref->color);
}

void App::RenameTab(int id, const std::wstring& name) {
    if (!tabSet_.Find(id)) return;
    // 去首尾空白后再判断空串：全是空格的名字等于"没改名"，
    // 否则标签栏会出现一条空白标题（比"改不动"更难理解）。
    const std::wstring alias = str::Trim(name);
    tabSet_.SetAlias(id, alias);
    PersistTabMeta(id);
    // 改名要立刻进内存配置而不是等退出时才写：用户改完马上关窗口是常见操作。
    // （与 AddRecent 同一取舍 —— 只改内存，不在热路径上写文件。）
    LogDbg(L"[tab] 重命名 id=%d alias=%s", id, alias.c_str());
    SyncChrome();   // 标签栏 + 窗口标题都读 DisplayTitle，一次刷完
    Layout();       // 名字长度变化会改标签宽（布局按标题量字）
}

void App::SetTabColor(int id, int color) {
    if (!tabSet_.Find(id)) return;
    tabSet_.SetColor(id, color);
    PersistTabMeta(id);
    SyncChrome();   // 颜色只影响绘制，但 SetTabs 是唯一重灌通道
}

int App::CloseDuplicates(int keepId) {
    const core::TabRef* ref = tabSet_.Find(keepId);
    if (!ref) return 0;
    const std::vector<int> dups = tabSet_.DuplicatesOf(keepId);
    int closed = 0;
    for (int id : dups) {
        // 锁定/固定标签不关（CloseTab 会拒绝并响铃）：先过滤，避免这里响一串提示音。
        const core::TabRef* d = tabSet_.Find(id);
        if (!d || d->locked || d->pinned) continue;
        if (CloseTab(id)) ++closed;
    }
    if (closed) Activate(keepId);
    LogDbg(L"[tab] 关闭重复：留下 id=%d，关掉 %d 个", keepId, closed);
    return closed;
}

App::Tab* App::Active() { return ById(activeId_); }

App::Tab* App::ById(int id) {
    auto it = tabs_.find(id);
    return it == tabs_.end() ? nullptr : it->second.get();
}

int App::ActiveIndex() const { return tabSet_.IndexOf(activeId_); }

std::wstring App::ActivePath() {
    Pane* p = ActivePane();
    return p ? p->path() : std::wstring();
}

void App::SyncTabIdentity(Tab* t) {
    if (!t || !tabSet_.Find(t->id)) return;
    Pane* p = PaneAt(t, t->activePane);
    if (!p || p->path().empty()) return;

    const std::wstring path = p->path();
    tabSet_.SetPath(t->id, path);
    tabSet_.SetTitle(t->id, ico::DisplayName(path, p->display));

    // 改名/着色按"当前显示的路径"取，切窗格后标签显示的就是那个路径的元数据。
    std::wstring alias;
    int color = 0;
    Config::Get().GetTabMeta(path, &alias, &color);
    tabSet_.SetAlias(t->id, alias);
    tabSet_.SetColor(t->id, color);

    // 锁定是按路径持久化的：切到另一个视图后，锁图标必须反映那个视图的路径。
    // 固定态不在这里重算 —— 固定是标签本身的属性，重算会触发排序让标签跳位。
    const auto& locked = Config::Get().s().lockedPaths;
    const bool isLocked =
        std::find_if(locked.begin(), locked.end(), [&path](const std::wstring& x) {
            return str::IEquals(x, path);
        }) != locked.end();
    tabSet_.SetLocked(t->id, isLocked);
}

std::wstring App::FocusPaneOf(HWND focus) {
    if (!focus) return L"unknown";
    // 顺序 = 判定精度：自有窗口（书签栏/导航栏）用 hwnd 精确匹配，shell 提供的
    // 类名次之，最后才是视图容器子孙。都匹配不上就 unknown —— 猜错比不答更糟
    // （AI 会拿着错误焦点做决策）。
    if (bookmarkBar_.hwnd() &&
        (focus == bookmarkBar_.hwnd() || IsChild(bookmarkBar_.hwnd(), focus)))
        return L"bookmark_bar";
    if (navBar_.hwnd() && (focus == navBar_.hwnd() || IsChild(navBar_.hwnd(), focus))) {
        // 搜索框与地址框分开报：两者都在导航栏里，但"聚焦搜索框"与
        // "聚焦地址栏"是两件不同的事。合成一个值的话，
        // 调用方无法确认 win.focus.search 到底生效了没有。
        if (navBar_.searchHwnd() && focus == navBar_.searchHwnd()) return L"search_box";
        return L"address_bar";
    }
    wchar_t cls[64] = {};
    GetClassNameW(focus, cls, _countof(cls));
    if (lstrcmpW(cls, L"SysTreeView32") == 0) return L"nav_tree";
    if (Tab* t = Active()) {
        // 双栏下分辨焦点在左视图还是右视图：两个视图都归为 file_view 的话，
        // AI 无法知道当前命令会打到哪一栏，也就无法据焦点做决策。
        const bool dual = Config::Get().s().dualPane;
        for (int i = 0; i < 2; ++i) {
            const Pane* pane = t->panes[static_cast<size_t>(i)].get();
            if (!pane || !pane->view || !pane->view->hwnd()) continue;
            if (focus == pane->view->hwnd() || IsChild(pane->view->hwnd(), focus))
                return dual ? (i == 0 ? L"file_view_left" : L"file_view_right") : L"file_view";
        }
    }
    return L"unknown";
}

int App::FindTab(ShellView* view) const {
    Tab* tab = nullptr;
    int pane = 0;
    return FindPane(view, &tab, &pane) && tab ? tab->id : 0;
}

void App::SyncChrome() {
    Tab* t = Active();
    if (!t) return;

    // 标题三条来源的唯一口径在 core::DisplayTitle（用户改名 > Shell 名 > 路径末级）。
    // 以前窗口标题和标签栏各写一遍 "display.empty() ? LeafOf(p) : display"，
    // 改名的标签就会出现"标签栏新名、标题栏旧名"。
    const core::TabRef* ref = tabSet_.Find(activeId_);
    std::wstring title = ref ? core::DisplayTitle(*ref) : std::wstring();
    Pane* ap = ActivePane();
    if (title.empty() && ap) title = ico::DisplayName(ap->path(), ap->display);
    if (title.empty()) title = id::kBrandShort;

    // 地址栏与标签条同源：都描述活动窗格。右窗格激活时后退/前进也必须退右窗格，
    // 否则用户看到的是 A、按后退却退的是 B。
    const std::wstring navPath = ap ? ap->path() : t->path();
    const std::wstring navDisplay = ap ? ap->display : PaneAt(t, 0)->display;
    const std::wstring navDisplayName = ico::DisplayName(navPath, navDisplay);

    NavBar::NavState ns;
    ns.canBack = ap && ap->nav.canBack();
    ns.canForward = ap && ap->nav.canForward();
    ns.canUp = nav::CanNavigateUp(navPath);

    const std::wstring sig =
        MakeChromeSignature(tabSet_, activeId_, ActiveIndex(), t->activePane, ref != nullptr,
                            title, navPath, navDisplayName, ns, navPane_.ok(),
                            tabStrip_.hwnd() != nullptr, navBar_.hwnd() != nullptr);
    if (sig == lastChromeSignature_) {
        // 选中集/文件计数不在这份签名里：它们只影响状态栏，必须每次照常取。
        MarkStatsDirty(); RefreshStats();
        return;
    }

    SetWindowTextW(hwnd_, (title + L" — " + id::kBrandShort).c_str());

    // 只喂模型：标题兜底、图标解析/缓存都在 TabStrip 内（App 不持有 HICON）。
    tabStrip_.SetTabs(tabSet_.items(), ActiveIndex(), !tabSet_.closed().empty(),
                      tabSet_.closed());
    navBar_.SetPath(navPath, navDisplayName);
    navBar_.SetNavState(ns);
    // 全局导航窗格的高亮跟随活动窗格路径（窗格只有一棵，与窗格数无关）。
    // 这里是唯一的同步点：激活标签、切窗格、导航、后退/前进都会走到 SyncChrome，
    // 树的高亮就不会出现"看的是 A、树还停在 B"。
    if (navPane_.ok() && !navPath.empty()) navPane_.SyncTo(navPath);
    lastChromeSignature_ = sig;
    MarkStatsDirty(); RefreshStats();
}

// 真正销毁"关标签时摘下来的视图"（WM_APP_RECLAIM_VIEWS）。
// 探针 84：一个 ShellView::Destroy = 20~69 ms，全在 UI 线程；所以 CloseTab 只做
// 便宜的两件事（隐藏 + move 出 Pane），析构放到消息循环里。此刻标签已从
// tabSet_/tabs_ 消失，队列中的视图没有任何代码还能拿到（App 独占），所以安全。
// 幂等：队列空就返回。清理期间若又关了标签，那条路径会再投一条消息。
void App::ReclaimPendingViews() {
    if (pendingDestroy_.empty()) return;
    // 换出来再销毁：销毁过程可能触发 shell 回调（如 WM_DESTROY 链上的
    // 重入），若这时往同一个 vector 里 push 会导致迭代器失效。
    std::vector<std::unique_ptr<ShellView>> doomed;
    doomed.swap(pendingDestroy_);
    doomed.clear();   // 每个 ShellView 的析构就是那 20~69 ms
}
