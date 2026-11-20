// 导航编排：所有"去哪"的入口收口到这里（App 的分片）。
//
// 分层：
//   · 路径语义（父级/叶子/向上目标）→ nav_path.h（纯字符串）
//   · 历史栈事务（压栈/收敛/回滚）  → nav::History（每个标签一份）
//   · 真正执行导航                  → ShellView::Navigate
//
// 本文件只做编排：把"用户的意图"翻译成"栈操作 + 一次 Navigate 调用"。
// 所有入口（地址栏、面包屑、书签、双击目录、命令总线、工具栏按钮）都必须
// 经过 NavigateActive / GoUp / GotoHistory 这三个出口之一 —— 不允许自己
// 拼 hist/push_back，那是历史上导致"后退要按两次"的直接原因。
#include "app.h"

// ---------------------------------------------------------------- 执行
//
// 唯一的一次"导航 + 记录"路径：
//   1. 让历史栈决定这一跳该压栈还是原地收敛（Request）
//   2. 真的去导航
//   3. 失败则回滚栈（Fail），并让上层知道
bool App::NavigateActive(const std::wstring& path, bool push) {
    Tab* t = Active();
    if (!t || path.empty()) return false;

    const bool shouldGo = t->nav.Request(path, push);
    if (!shouldGo) {
        // 目标就是当前目录且要求压栈：不重复导航（否则 Shell 白忙一次，
        // 状态栏闪一下、预览窗格重建）。但如果确实要重进同一路径
        // （push=false，例如强制刷新式重导航），下面照常执行。
        if (push) return true;
    }

    if (!t->view->Navigate(path)) {
        MessageBoxW(hwnd_, (L"无法打开：\n" + path).c_str(), L"ExBar", MB_ICONWARNING);
        OnTabNavFailed(t->view.get());
        return false;
    }
    return true;
}

// 历史游标移动：先改游标，再导航；失败退回 —— 避免"地址栏显示 A、视图还是 B"。
void App::GotoHistory(int target) {
    Tab* t = Active();
    if (!t) return;
    if (!t->nav.Goto(target)) return;  // 越界：静默忽略（按钮灰显已经挡过一层）
    if (!t->view->Navigate(t->nav.pending().path)) OnTabNavFailed(t->view.get());
}

void App::GoBack() {
    Tab* t = Active();
    if (t && t->nav.canBack()) GotoHistory(t->nav.idx() - 1);
}

void App::GoForward() {
    Tab* t = Active();
    if (t && t->nav.canForward()) GotoHistory(t->nav.idx() + 1);
}

// 上一级：目标由 nav::UpTarget 唯一决定（盘符根 -> 此电脑，命名空间/UNC 根 -> 无处可去）。
// 这里不写任何路径判断 —— 否则又会变成"命令前置检查"和"实际执行"两套口径。
void App::GoUp() {
    Tab* t = Active();
    if (!t) return;
    const std::wstring target = nav::UpTarget(t->path());
    if (!target.empty()) NavigateActive(target, true);
}

void App::GoHome() { NavigateActive(HomePath(), true); }

void App::RefreshActive() {
    Tab* t = Active();
    if (t && t->view) t->view->Refresh();
}

std::vector<std::wstring> App::HistoryList(bool backward) const {
    Tab* t = const_cast<App*>(this)->Active();
    if (!t) return {};
    return backward ? t->nav.backward() : t->nav.forward();
}

void App::JumpHistory(int depth, bool backward) {
    Tab* t = Active();
    if (!t) return;
    const int target = backward ? t->nav.idx() - 1 - depth : t->nav.idx() + 1 + depth;
    GotoHistory(target);
}

void App::ClearHistory() {
    Tab* t = Active();
    if (!t) return;
    const std::wstring cur = t->path();
    t->nav.ClearKeepingHere();

    // 下拉里还混着"最近访问"（Config::recentPaths）—— 不清它的话，用户点了
    // "清除历史记录"会发现列表下半截原样还在，像是没生效。
    auto& recent = Config::Get().mutableSettings().recentPaths;
    const size_t recentN = recent.size();
    recent.clear();
    Config::Get().Save();

    LogDbg(L"[nav] 清除历史：标签栈保留当前=%s，最近访问清掉 %zu 条", cur.c_str(), recentN);
    SyncChrome();  // 重新算导航栏/标签栏状态（后退前进可用态跟着变）
}

// ---------------------------------------------------------------- 回调
//
// Shell 完成的回报。历史栈的就地收敛（重定向 / 规范化）全部由 History::Complete
// 处理，这里只管"壳层"的事：标签标题、最近访问、状态栏计数。
void App::OnTabNav(ShellView* view, const std::wstring& path, const std::wstring& display,
                   unsigned items) {
    int id = FindTab(view);
    if (!id) return;
    Tab* t = ById(id);
    if (!t) return;

    if (!path.empty()) {
        const std::wstring settled = t->nav.Complete(path);
        tabSet_.SetPath(id, settled.empty() ? path : settled);
        Config::Get().AddRecent(settled.empty() ? path : settled);
    } else {
        t->nav.Complete(path);  // 无路径：只清 pending
    }

    t->split = SplitCache{};  // 换了目录：上一目录的分类缓存不能复用（"12 个项目"撞车时最坑）
    if (items) t->items = items;  // 回调里 ItemCount 恒 0，别把已取到的计数抹掉
    if (!display.empty()) t->display = display;
    const std::wstring cur = t->path();
    tabSet_.SetTitle(id, t->display.empty() ? LeafOf(cur) : t->display);

    // 前台/后台标签都走同一条刷新路径：曾经这里有一份"后台标签只重建标签栏"的
    // 复制粘贴版本，图标只从缓存取，新出现的路径永远没有图标。
    SyncChrome();
}

// 导航失败（同步失败或 Shell 回调 OnNavigationFailed）：把乐观写入的历史回滚掉。
// 不回滚的话地址栏/面包屑停在目标路径、视图还停在原目录，后退栈里也多一条死条目。
void App::OnTabNavFailed(ShellView* view) {
    int id = FindTab(view);
    Tab* t = id ? ById(id) : nullptr;
    if (!t) return;
    LogDbg(L"[nav] failed path=%s", t->nav.pending().path.c_str());

    t->nav.Fail();
    t->split = SplitCache{};  // 换了目录：上一目录的分类缓存不能复用（"12 个项目"撞车时最坑）
    SyncChrome();
}
