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

#include "text_file.h"   // tf::Exists / tf::IsDirectory（区分"导航到目录"与"打开文件"）
#include "icon_resolve.h"

// ---------------------------------------------------------------- 执行
//
// 唯一的一次"导航 + 记录"路径：
//   1. 让历史栈决定这一跳该压栈还是原地收敛（Request）
//   2. 真的去导航
//   3. 失败则回滚栈（Fail），并让上层知道
bool App::NavigateActive(const std::wstring& path, bool push) {
    Tab* t = Active();
    if (!t || path.empty()) return false;

    // 目标是**文件**时不该走"导航"：IExplorerBrowser::BrowseToObject 对文件项
    // 返回失败，于是地址栏里敲 `C:\Windows\notepad.exe` 回车得到的是一句
    // "无法打开" —— 而资源管理器 / Opus / TC 在同一处的语义都是"打开这个文件"。
    //
    // 放在 nav.Request 之前：文件不是历史条目，不该压栈（否则后退会退到
    // "刚才打开的那个 exe"，而那从来不是一个目录）。
    // 命名空间（::{GUID} / shell:）不是文件系统路径，Exists 自然为 false，不受影响。
    if (tf::Exists(path) && !tf::IsDirectory(path)) {
        const HINSTANCE r = ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr,
                                          SW_SHOWNORMAL);
        const bool ok = reinterpret_cast<INT_PTR>(r) > 32;
        LogDbg(L"[nav] 目标是文件，交给 shell 打开：%s ok=%d", path.c_str(), ok ? 1 : 0);
        if (!ok)
            MessageBoxW(hwnd_, (L"无法打开：\n" + path).c_str(), id::kBrandShort, MB_ICONWARNING);
        return ok;
    }

    const bool shouldGo = t->nav.Request(path, push);
    if (!shouldGo) {
        // 目标就是当前目录且要求压栈：不重复导航（否则 Shell 白忙一次，
        // 状态栏闪一下、预览窗格重建）。但如果确实要重进同一路径
        // （push=false，例如强制刷新式重导航），下面照常执行。
        if (push) return true;
    }

    if (!t->view->Navigate(path)) {
        MessageBoxW(hwnd_, (L"无法打开：\n" + path).c_str(), id::kBrandShort, MB_ICONWARNING);
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

        // 改名/颜色是"按路径的标记"，不是"跟着这个标签走"的：
        // 标签导航到另一个目录后仍顶着旧名字（甚至旧颜色）会误导 —— 名字说的是
        // 那个目录，颜色标的是那组目录。所以路径变了就换成新路径的标记。
        // 比较用大小写不敏感：同一个目录的规范化可能只差大小写，那不是"换了目录"。
        const std::wstring before = tabSet_.Find(id) ? tabSet_.Find(id)->path : std::wstring();
        const std::wstring after = settled.empty() ? path : settled;
        // 命名空间解析串没有可靠的 post-hoc 显示名推导入口：Shell 回调此刻给到的
        // display 是权威值（第三方 NSE 尤其如此），登记给 ico::DisplayName 供
        // 历史/最近访问下拉复用；普通路径会被 RememberDisplay 自己过滤掉。
        if (!display.empty()) ico::RememberDisplay(after, display);
        const bool same = str::IEquals(before, after);

        tabSet_.SetPath(id, after);
        if (!same) {
            std::wstring alias;
            int color = 0;
            Config::Get().GetTabMeta(after, &alias, &color);
            tabSet_.SetAlias(id, alias);
            tabSet_.SetColor(id, color);
        }
        Config::Get().AddRecent(after);
    } else {
        t->nav.Complete(path);  // 无路径：只清 pending
    }

    t->split = SplitCache{};  // 换了目录：上一目录的分类缓存不能复用（"12 个项目"撞车时最坑）
    if (items) t->items = items;  // 回调里 ItemCount 恒 0，别把已取到的计数抹掉
    if (!display.empty()) t->display = display;
    const std::wstring cur = t->path();
    // 标题仍写 Shell 名：用户改名在 alias 里，DisplayTitle 会优先读它。
    // 千万不要在这里写 alias —— 那是"改名只在下次导航前有效"的成因。
    // Shell 回调对 "::{GUID}" 的 display 可能为空甚至就是解析串，回退到统一
    // 显示名入口，避免标签页也出现裸 CLSID。
    const std::wstring title = ico::DisplayName(cur, t->display);
    tabSet_.SetTitle(id, title);

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
