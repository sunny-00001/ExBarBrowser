// 导航编排：所有"去哪"的入口收口到这里（App 的分片）。
//
// 分层：
//   · 路径语义（父级/叶子/向上目标）→ nav_path.h（纯字符串）
//   · 历史栈事务（压栈/收敛/回滚）  → nav::History（每个窗格一份）
//   · 真正执行导航                  → ShellView::Navigate
//
// 本文件只做编排：把"用户的意图"翻译成"栈操作 + 一次 Navigate 调用"。
// 所有入口（地址栏、面包屑、书签、双击目录、命令总线、工具栏按钮）都必须
// 经过 NavigateActive / GoUp / GotoHistory 这三个出口之一 —— 不允许自己
// 拼 hist/push_back，那是历史上导致"后退要按两次"的直接原因。
#include "app.h"

#include "text_file.h"   // tf::Exists / tf::IsDirectory（区分"导航到目录"与"打开文件"）
#include "icon_resolve.h"
#include "search_folder.h"  // searchfolder::Create —— 原生搜索文件夹（系统搜索协议）
#include "search_scope.h"   // searchscope::Resolve —— 搜索范围 → 作用域的唯一真源
#include "search_url.h"  // searchurl::Trim —— 搜索词规范化

// ---------------------------------------------------------------- 执行
//
// 唯一的一次"导航 + 记录"路径：

namespace {

// 导航失败时的统一出口。
//
// 【为什么必须分流】AI 命令在 **UI 线程**执行（ai::PumpRequests 由 WM_TIMER 驱动）。
// MessageBoxW 会进消息循环，于是管道永远读不到这次调用的响应 —— 调用方只能等到
// 10 秒超时，而且这段时间里**整个消息泵被模态框占住**，界面不重绘、别的请求也发不出去。
//
// 实测（2026-11-21）：`fs.open {"path":"C://Windows"}` → {"ok":false,"error":"timeout"}，
// 10.5 秒。日志里对应 `[nav] parse 'C:\\Windows' hr=0x80070057`。
//
// 【判据】CommandBus::fromMachine() —— 看 ActorKind，不看来源细分标签。
// 机器侧改成 NoteFailure（把原因如实交出去，调用方拿 errorCode/error）；
// 人侧保留弹窗（这是交互式行为，该让人看见）。
//
// 【为什么不在这里硬编码"机器"两个字】Actor 是闭集枚举（ActorKind::Human/Ai），
// 新增来源时不必改这里 —— 判据在 CommandBus 里是唯一一处。
void ReportNavFailure(HWND owner, const std::wstring& path) {
    if (CommandBus::Get().fromMachine()) {
        CommandBus::Get().NoteFailure(L"无法打开：" + path);
        return;
    }
    MessageBoxW(owner, (L"无法打开：\n" + path).c_str(), id::kBrandShort, MB_ICONWARNING);
}

// 搜索结果视图的标题前缀。
//
// 【为什么它同时是"当前视图是不是搜索视图"的判据】原生搜索文件夹的解析名
// 就是我们给的显示名（"搜索: <词>"）—— 实测状态快照里 activePath 就是这个串。
// Windows 文件名里不能有冒号，命名空间路径也不会长这样，所以这个前缀不会
// 和真实目录撞上，拿它判定是可靠的。
constexpr wchar_t kSearchTitlePrefix[] = L"搜索: ";

// 路径是不是搜索结果视图的路径。
bool IsSearchTitle(const std::wstring& path) {
    return path.rfind(kSearchTitlePrefix, 0) == 0;
}

}  // namespace

//   1. 让历史栈决定这一跳该压栈还是原地收敛（Request）
//   2. 真的去导航
//   3. 失败则回滚栈（Fail），并让上层知道
bool App::NavigateActive(const std::wstring& path, bool push) {
    Tab* t = Active();
    Pane* p = t ? ActivePane() : nullptr;
    if (!p || path.empty()) return false;

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
            ReportNavFailure(hwnd_, path);
        return ok;
    }

    const bool shouldGo = p->nav.Request(path, push);
    if (!shouldGo) {
        // 目标就是当前目录且要求压栈：不重复导航（否则 Shell 白忙一次，
        // 状态栏闪一下、预览窗格重建）。但如果确实要重进同一路径
        // （push=false，例如强制刷新式重导航），下面照常执行。
        if (push) return true;
    }

    if (!p->view || !p->view->Navigate(path)) {
        ReportNavFailure(hwnd_, path);
        if (p->view) OnTabNavFailed(p->view.get());
        return false;
    }
    return true;
}

// 历史游标移动：先改游标，再导航；失败退回 —— 避免"地址栏显示 A、视图还是 B"。
void App::GotoHistory(int target) {
    Pane* p = ActivePane();
    if (!p) return;
    if (!p->nav.Goto(target)) return;  // 越界：静默忽略（按钮灰显已经挡过一层）
    if (!p->view || !p->view->Navigate(p->nav.pending().path))
        if (p->view) OnTabNavFailed(p->view.get());
}

void App::GoBack() {
    Pane* p = ActivePane();
    if (p && p->nav.canBack()) GotoHistory(p->nav.idx() - 1);
}

void App::GoForward() {
    Pane* p = ActivePane();
    if (p && p->nav.canForward()) GotoHistory(p->nav.idx() + 1);
}

// 上一级：目标由 nav::UpTarget 唯一决定（盘符根 -> 此电脑，命名空间/UNC 根 -> 无处可去）。
// 这里不写任何路径判断 —— 否则又会变成"命令前置检查"和"实际执行"两套口径。
void App::GoUp() {
    Pane* p = ActivePane();
    if (!p) return;
    // 搜索结果视图不是文件系统目录，没有"上一级"：往上 = 回到搜索来源目录。
    if (!p->searchFrom.empty()) {
        const std::wstring back = p->searchFrom;
        p->searchFrom.clear();
        p->searchQuery.clear();
        NavigateActive(back, true);
        return;
    }
    const std::wstring target = nav::UpTarget(p->path());
    if (!target.empty()) NavigateActive(target, true);
}

void App::GoHome() { NavigateActive(HomePath(), true); }

// 搜索：导航栏搜索框与 search.find 命令的唯一实现。
//
// 【为什么走原生搜索文件夹而不是 search-ms: URL】search-ms: 是**协议处理器
// URL**，本宿主（IExplorerBrowser，没有搜索带）的 ShellView::DoNavigate 明确
// 拒绝它 —— BrowseToObject 返回 S_OK 却把视图留在原地，"搜索成功"成了一句假话。
// 原生搜索文件夹（ISearchFolderItemFactory 造的 PIDL）是**真的文件夹**，以 PIDL
// 形式导航过去就是搜索结果视图：排序、右键、拖放照旧可用。
// 同一套做法见 D:\Windhawk\exbar.cpp:26215 RunSystemSearchFolder。
//
// 空串 = 取消搜索：搜索视图没有"上一级"，只能记住来源目录再退回去。
bool App::RunSearch(const std::wstring& query, std::wstring* title) {
    Pane* p = ActivePane();
    if (!p || !p->view) return false;
    // 只打空格＝没打：搜索框原样把输入交过来，规范化在这里做（唯一入口）。
    const std::wstring q = searchurl::Trim(query);
    if (q.empty()) {
        if (p->searchFrom.empty()) return true;  // 本窗格本来就不在搜索里
        const std::wstring back = p->searchFrom;
        p->searchFrom.clear();
        p->searchQuery.clear();
        return NavigateActive(back, true);
    }
    // 记搜索历史：记的是"用户提交过的词"，与这次搜索**是否成功**无关 ——
    // 否则搜索失败时历史永远是空的，右带的「搜索历史」就成了死功能。
    Config::Get().AddSearchHistory(q);
    // 已经是搜索视图时，来源目录要用最早的（否则"在搜索结果里再搜"会搜到 0 项）。
    const std::wstring scope = p->searchFrom.empty() ? p->path() : p->searchFrom;
    // 范围（搜索框左带选的那一档）决定搜哪儿：仅本层 / 含子目录 / 此电脑 /
    // 自定义路径。四档各自落到不同的作用域 —— 没有"看着像换了档、实际搜的是
    // 同一个范围"这种事（范围语义只在 searchscope::Resolve 一处）。
    const searchscope::Target t =
        searchscope::Resolve(SearchRange(), scope, Config::Get().s().searchRangePath);
    if (!t.valid) {
        // 「此电脑」与「自定义路径没设」是两回事，文案不能混：
        // 前者是本宿主的能力缺口（作用域只能是一个普通目录），后者是配置没填。
        const bool whole = SearchRange() == searchscope::kThisPC;
        const std::wstring why =
            whole ? L"整台电脑搜索在本宿主暂不可用：Windows Search 的整机范围要 Explorer 进程内的 search-ms 宿主，本宿主只能以单个索引目录建搜索文件夹。请改用「当前目录（含子目录）」或「自定义路径」"
                  : L"搜索范围还不完整（自定义路径未设置）";
        LogDbg(L"[search] %s", why.c_str());
        CommandBus::Get().NoteFailure(why);
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    // 作用域必须是真实磁盘目录：命名空间（此电脑/回收站…）不是索引范围，
    // 硬塞给搜索索引只会得到"搜不到"，或者悄悄把范围扩大到整台电脑。
    if (!t.scopeDir.empty() && !tf::IsDirectory(t.scopeDir)) {
        const std::wstring why =
            L"当前所在位置不能作为搜索范围（它不是磁盘目录）；请先进入一个文件夹，或改用「自定义路径」";
        LogDbg(L"[search] %s scope=%s", why.c_str(), t.scopeDir.c_str());
        CommandBus::Get().NoteFailure(why);
        MessageBeep(MB_ICONWARNING);
        return false;
    }

    // 标题：搜索词 + 范围落点。
    //
    // 【为什么标题必须随范围变】原生搜索文件夹的**解析名就是显示名**，而 shell
    // 靠解析名认"是不是同一个文件夹"：两次搜索标题相同 → shell 直接复用上一次
    // 的结果集，连换范围也不刷新（实测：换到"仅本层"看到的还是递归的全量结果，
    // 1259 条 vs 18 条）。带上映射范围后标题互不相同，shell 才会重建文件夹。
    // 这同时也是给用户看的"在哪儿搜的" —— 与资源管理器地址栏的
    // "搜索: xxx  在 <目录> 中" 同一路数。
    const std::wstring shortLabel = searchscope::RangeShortLabel(SearchRange());
    const std::wstring scopeText =
        t.scopeDir.empty() ? shortLabel : t.scopeDir + L"（" + shortLabel + L"）";
    const std::wstring name = std::wstring(kSearchTitlePrefix) + q + L" · " + scopeText;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!searchfolder::Create(q, t.scopeDir, t.onlyThisLayer, name, &pidl)) {
        const std::wstring why = L"系统搜索不可用（这台机器没有可用的 Windows Search 索引）";
        LogDbg(L"[search] %s", why.c_str());
        CommandBus::Get().NoteFailure(why);
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    if (!p->view->NavigatePid(pidl)) {
        CoTaskMemFree(pidl);
        const std::wstring why = L"搜索失败（宿主没能打开搜索结果视图）";
        LogDbg(L"[search] %s", why.c_str());
        CommandBus::Get().NoteFailure(why);
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    // 搜索结果视图默认详细信息：原生搜索文件夹的视图模板默认"内容视图"。
    p->view->EnsureSearchDetailsView(pidl);
    CoTaskMemFree(pidl);
    p->searchFrom = scope;
    p->searchQuery = q;
    if (title) *title = name;
    return true;
}

void App::RefreshActive() {
    Pane* p = ActivePane();
    if (p && p->view) p->view->Refresh();
}

void App::ScheduleViewAutoRefresh() {
    // 重复 SetTimer 只重置间隔、不叠加（同一 hwnd + 同一 id）。
    // shell 批量操作连发几十条通知 → 只有最后一条之后 300ms 刷一次。
    SetTimer(hwnd_, kViewRefreshTimer, 300, nullptr);
}

std::vector<std::wstring> App::HistoryList(bool backward) const {
    const Tab* t = const_cast<App*>(this)->Active();
    if (!t) return {};
    const int idx = t->activePane == 1 ? 1 : 0;
    const Pane* p = t->panes[static_cast<size_t>(idx)].get();
    if (!p) return {};
    return backward ? p->nav.backward() : p->nav.forward();
}

void App::JumpHistory(int depth, bool backward) {
    Pane* p = ActivePane();
    if (!p) return;
    const int target = backward ? p->nav.idx() - 1 - depth : p->nav.idx() + 1 + depth;
    GotoHistory(target);
}

int App::ClearHistory() {
    Pane* p = ActivePane();
    if (!p) return 0;
    const std::wstring cur = p->path();
    p->nav.ClearKeepingHere();

    // 下拉里还混着"最近访问"（Config::recentPaths）—— 不清它的话，用户点了
    // "清除历史记录"会发现列表下半截原样还在，像是没生效。
    auto& recent = Config::Get().mutableSettings().recentPaths;
    const int recentN = static_cast<int>(recent.size());
    recent.clear();
    Config::Get().Save();

    LogDbg(L"[nav] 清除历史：标签栈保留当前=%s，最近访问清掉 %d 条", cur.c_str(), recentN);
    SyncChrome();  // 重新算导航栏/标签栏状态（后退前进可用态跟着变）
    return recentN;
}

// ---------------------------------------------------------------- 回调
//
// Shell 完成的回报。历史栈的就地收敛（重定向 / 规范化）全部由 History::Complete
// 处理，这里只管"壳层"的事：标签标题、最近访问、状态栏计数。
void App::OnTabNav(ShellView* view, const std::wstring& path, const std::wstring& display,
                   unsigned items) {
    Tab* t = nullptr;
    int paneIndex = -1;
    if (!FindPane(view, &t, &paneIndex) || !t) return;
    Pane* p = PaneAt(t, paneIndex);
    if (!p) return;

    if (!path.empty()) {
        const std::wstring settled = p->nav.Complete(path);
        const std::wstring after = settled.empty() ? path : settled;
        if (!display.empty()) ico::RememberDisplay(after, display);
        Config::Get().AddRecent(after);
    } else {
        p->nav.Complete(path);  // 无路径：只清 pending
    }

    // 离开搜索视图 → 清掉本窗格的搜索状态。
    //
    // 【为什么必须清】searchFrom/searchQuery 记的是"本窗格正在看搜索结果、
    // 来源目录是哪个"。用户从搜索结果导航去别处（地址栏、书签、双击目录…）
    // 之后如果还留着：
    //   ① 状态快照里 searching 永远是 true（窗格其实早就不在搜索里了）；
    //   ② 下一次搜索会拿**旧的来源目录**当范围 —— 在 E:\ 搜完、切到 F:\ 再搜，
    //      搜的还是 E:\ 的范围。
    // 实测踩过：搜完没清，scope 停在旧的 ThisPC CLSID 上，于是"当前目录/含子目录"
    // 两档直接报"它不是磁盘目录"，而视图明明停在 E:\ExBarBrowser。
    //
    // 判据是完成路径的前缀：搜索视图的解析名就是标题（见 kSearchTitlePrefix）。
    if (!p->searchFrom.empty() && !path.empty() && !IsSearchTitle(path)) {
        p->searchFrom.clear();
        p->searchQuery.clear();
    }

    // 会话选中恢复：首次导航完成即消费。目标目录匹配 → 恢复选中；
    // 用户启动后立刻导航去了别处 = 放弃恢复（不追着旧目录补选中）。
    if (!pendingSelItems_.empty()) {
        if (str::IEquals(path, pendingSelPath_)) view->SelectPaths(pendingSelItems_);
        pendingSelPath_.clear();
        pendingSelItems_.clear();
    }

    p->split = SplitCache{};   // 换了目录：分类缓存不能跨目录复用
    if (items) p->items = items;
    if (!display.empty()) p->display = display;
    // 标签条显示的是"这个标签当前正在看的窗格"。后台窗格导航（如会话恢复时
    // 创建右视图）不该把标签改成另一个目录；只有活动窗格才改写标签身份。
    if (paneIndex == t->activePane) SyncTabIdentity(t);
    SyncChrome();
    MarkSessionDirty();
}

// 导航失败（同步失败或 Shell 回调 OnNavigationFailed）：把乐观写入的历史回滚掉。
// 不回滚的话地址栏/面包屑停在目标路径、视图还停在原目录，后退栈里也多一条死条目。
void App::OnTabNavFailed(ShellView* view) {
    Tab* t = nullptr;
    int paneIndex = -1;
    if (!FindPane(view, &t, &paneIndex) || !t) return;
    Pane* p = PaneAt(t, paneIndex);
    if (!p) return;
    LogDbg(L"[nav] failed pane=%d path=%s", paneIndex, p->nav.pending().path.c_str());

    p->nav.Fail();
    p->split = SplitCache{};
    SyncChrome();
}
