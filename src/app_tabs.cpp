// 标签生命周期（App 的编排分片）。
//
// 只管"模型 + 视图"的成对变化：core::TabSet 记顺序/锁定，App::tabs_ 持视图与历史；
// 画与命中全在 TabStrip，本文件不碰 GDI。改这里只有一个理由：标签怎么开、关、切。
#include "app.h"

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
    tab->display = tab->view->display().empty() ? LeafOf(initial) : tab->view->display();
    tab->nav.Reset(initial);

    const int id = tabSet_.Add(initial, tab->display, index);
    tab->id = id;
    ShellView* created = tab->view.get();
    tabs_[id] = std::move(tab);

    for (const auto& locked : Config::Get().s().lockedPaths)
        if (_wcsicmp(locked.c_str(), initial.c_str()) == 0) tabSet_.SetLocked(id, true);

    // 新标签套用持久化的视图偏好（模式/排序/分组/窗格），而不是每次回到默认。
    // 必须等首个视图就绪后再调：Create() 内部的首个导航可能还没产出 IShellView。
    ApplyPersistedViewState(created);

    Activate(id);
    Layout();
    return id;
}

int App::NewTabIndex() const {
    const std::wstring& pos = Config::Get().s().tabNewPos;
    if (_wcsicmp(pos.c_str(), L"end") == 0) return tabSet_.Count();
    return ActiveIndex() + 1;
}

void App::CloseTab(int id) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    if (ref->locked) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const int index = tabSet_.IndexOf(id);
    std::wstring path = ref->path;
    tabSet_.PushClosed(path);
    tabs_.erase(id);
    tabSet_.Remove(id);

    if (tabSet_.Count() == 0) {
        OnDestroy();
        DestroyWindow(hwnd_);
        return;
    }
    if (activeId_ == id) {
        int next = std::min(index, tabSet_.Count() - 1);
        Activate(tabSet_.IdAt(next));
    } else {
        SyncChrome();
    }
    Layout();
}

void App::CloseOthers(int keepId) {
    std::vector<int> ids;
    for (const auto& t : tabSet_.items())
        if (t.id != keepId) ids.push_back(t.id);
    for (int id : ids) CloseTab(id);
    Activate(keepId);
}

void App::CloseSide(int id, bool left) {
    const int index = tabSet_.IndexOf(id);
    if (index < 0) return;
    std::vector<int> ids;
    for (int i = 0; i < tabSet_.Count(); ++i) {
        if (left ? (i < index) : (i > index)) ids.push_back(tabSet_.IdAt(i));
    }
    for (int t : ids) CloseTab(t);
    Activate(id);
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

void App::ToggleLock(int id) {
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    bool locked = !ref->locked;
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
    std::wstring p = t->path();
    std::wstring title = t->display.empty() ? LeafOf(p) : t->display;
    if (title.empty()) title = id::kBrandShort;
    SetWindowTextW(hwnd_, (title + L" — " + id::kBrandShort).c_str());

    // 只喂模型：标题兜底、图标解析/缓存都在 TabStrip 内（App 不持有 HICON）。
    tabStrip_.SetTabs(tabSet_.items(), ActiveIndex());
    navBar_.SetPath(p, t->display);
    RefreshStats();
}
