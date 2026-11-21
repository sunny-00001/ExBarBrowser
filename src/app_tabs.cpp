// 标签生命周期（App 的编排分片）。
//
// 只管"模型 + 视图"的成对变化：core::TabSet 记顺序/锁定，App::tabs_ 持视图与历史；
// 画与命中全在 TabStrip，本文件不碰 GDI。改这里只有一个理由：标签怎么开、关、切。
#include "app.h"
#include "str.h"

#include "icon_resolve.h"

// ---------------------------------------------------------------- 标签
int App::NewTab(const std::wstring& path, int index) {
    std::wstring target = path.empty() ? HomePath() : path;
    auto tab = std::make_unique<Tab>();
    tab->view = std::make_unique<ShellView>(theme_);
    ShellView* raw = tab->view.get();
    // 先接输入汇流再 Create：首次 Shell 视图可能在 Create() 内部就创建完成。
    tab->view->SetMouseSink([this](int target, UINT msg, int button, int mods, POINT pt) {
        return OnChromeMouse(target, msg, button, mods, pt);
    });
    if (!tab->view->Create(viewArea_, target)) {
        // 不在这里弹模态：启动恢复会话时可能连续失败多个标签，弹窗会一串串冒出来。
        LogDbg(L"[tab] 创建视图失败：%s", target.c_str());
        return 0;
    }
    tab->view->SetNavFn([this, raw](const std::wstring& p, const std::wstring& d, unsigned n) {
        OnTabNav(raw, p, d, n);
    });
    tab->view->SetFailFn([this, raw] { OnTabNavFailed(raw); });

    std::wstring initial = target;
    if (!tab->view->path().empty()) initial = tab->view->path();
    tab->display = ico::DisplayName(initial, tab->view->display());
    tab->nav.Reset(initial);

    const int id = tabSet_.Add(initial, tab->display, index);
    tab->id = id;
    ShellView* created = tab->view.get();
    tabs_[id] = std::move(tab);

    for (const auto& locked : Config::Get().s().lockedPaths)
        if (str::IEquals(locked, initial)) tabSet_.SetLocked(id, true);

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

    Activate(id);
    Layout();
    return id;
}

int App::NewTabIndex() const {
    const std::wstring& pos = Config::Get().s().tabNewPos;
    if (str::IEquals(pos.c_str(), L"end")) return tabSet_.Count();
    return ActiveIndex() + 1;
}

bool App::CloseTab(int id) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return false;
    if (ref->locked) {
        MessageBeep(MB_ICONWARNING);
        return false;   // 锁定标签没被关掉 —— 必须让调用方知道
    }
    // 焦点显式化：在销毁视图**之前**记录"焦点是否落在被关标签的视图里"。
    // 销毁后 GetFocus() 会变成 null 或父窗口，Activate 原来的 focusInView 判据
    // 恰好能搬回焦点纯属侥幸（null 走 !focus 分支）；把判据提前、显式搬回，
    // 才能不被"关闭路径上多一次 SetFocus/SendMessage"之类的改动悄悄破坏。
    HWND focus = GetFocus();
    Tab* closing = ById(id);
    const bool focusInClosingView =
        closing && closing->view && closing->view->hwnd() &&
        (focus == closing->view->hwnd() || IsChild(closing->view->hwnd(), focus));
    const int index = tabSet_.IndexOf(id);
    std::wstring path = ref->path;
    tabSet_.PushClosed(path);
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
    return true;
}

int App::CloseOthers(int keepId) {
    // 必须先收集再关：CloseTab 会 erase items_，边遍历边关会让迭代器失效。
    std::vector<int> ids;
    int skipped = 0;
    for (const auto& t : tabSet_.items()) {
        if (t.id == keepId) continue;
        // 先过滤锁定标签：CloseTab 会逐个 MessageBeep，批量关闭时那是一串提示音。
        if (t.locked) { ++skipped; continue; }
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

int App::CloseSide(int id, bool left) {
    const int index = tabSet_.IndexOf(id);
    if (index < 0) return 0;
    // 同样先收集：CloseTab 会改变 items_ 的下标。
    std::vector<int> ids;
    int skipped = 0;
    for (int i = 0; i < tabSet_.Count(); ++i) {
        if (!(left ? (i < index) : (i > index))) continue;
        const core::TabRef* t = tabSet_.Find(tabSet_.IdAt(i));
        if (t && t->locked) { ++skipped; continue; }
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
    activeId_ = id;
    for (auto& kv : tabs_) kv.second->view->SetVisible(kv.first == id);
    tabSet_.Touch(id);
    SyncChrome();
    // 只有焦点原本在视图区（或还没有焦点）时才搬焦点。从标签栏点过来时保留在标签栏，
    // 否则"标签栏"作用域的裸键（Shift+←/→/Home/End）永远收不到。
    Tab* t = Active();
    if (!t || !t->view) return;
    HWND focus = GetFocus();
    const bool focusInView =
        !focus || focus == viewArea_ || (viewArea_ && IsChild(viewArea_, focus));
    if (focusInView && t->view->hwnd()) SetFocus(t->view->hwnd());
}

void App::MoveTab(int id, int toIndex) {
    tabSet_.Move(id, toIndex);
    SyncChrome();
    Layout();
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
        // 锁定标签不关（CloseTab 会拒绝并响铃）：先过滤，避免这里响一串提示音。
        const core::TabRef* d = tabSet_.Find(id);
        if (!d || d->locked) continue;
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

std::wstring App::ActivePath() const {
    const core::TabRef* t = tabSet_.Find(activeId_);
    return t ? t->path : std::wstring();
}

int App::FindTab(ShellView* view) const {
    for (const auto& kv : tabs_)
        if (kv.second->view.get() == view) return kv.first;
    return 0;
}

void App::SyncChrome() {
    Tab* t = Active();
    if (!t) return;
    // 标题三条来源的唯一口径在 core::DisplayTitle（用户改名 > Shell 名 > 路径末级）。
    // 以前窗口标题和标签栏各写一遍 "display.empty() ? LeafOf(p) : display"，
    // 改名的标签就会出现"标签栏新名、标题栏旧名"。
    const core::TabRef* ref = tabSet_.Find(activeId_);
    std::wstring title = ref ? core::DisplayTitle(*ref) : std::wstring();
    if (title.empty()) title = ico::DisplayName(t->path(), t->display);
    if (title.empty()) title = id::kBrandShort;
    SetWindowTextW(hwnd_, (title + L" — " + id::kBrandShort).c_str());

    // 只喂模型：标题兜底、图标解析/缓存都在 TabStrip 内（App 不持有 HICON）。
    tabStrip_.SetTabs(tabSet_.items(), ActiveIndex(), !tabSet_.closed().empty());
    navBar_.SetPath(t->path(), ico::DisplayName(t->path(), t->display));
    // 后退/前进的可用态只有历史栈知道。以前这两颗按钮恒亮 ——
    // SyncChrome 是"任何导航完成/失败都会经过"的唯一位置，所以它是最合适的喂点
    // （比在 GoBack/GoForward 里各改一次更稳：视图内部发起的导航也要更新）。
    NavBar::NavState ns;
    ns.canBack = t->nav.canBack();
    ns.canForward = t->nav.canForward();
    ns.canUp = nav::CanNavigateUp(t->path());
    navBar_.SetNavState(ns);
    RefreshStats();
}
