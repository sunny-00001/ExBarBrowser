// 状态探测与 AI 快照接线（App 的编排分片）。
//
// 两件事共用一份观测口径：命令前置条件（CommandState）与 AI 状态快照都从
// 活动窗格现取。放在单独分片里，是为了不让 app.cpp 继续变成"什么都往里塞"的
// 上帝文件；这里只做取数，不改写任何状态。
#include "app.h"

#include "ai_api.h"
#include "menu.h"            // ui::Menu::SetShortcutResolver（菜单右端键位文案）
#include "search_scope.h"    // searchscope::RangeId（搜索范围档位 → id）
#include "shell_columns.h"   // 属性键文本格式的唯一来源（{GUID}:pid）
#include "shell_ops.h"
#include "view_modes.h"      // viewmode::ModeIndexFor（视图档位从 Shell 现读）

int App::ActiveViewModeIndex() {
    if (ShellView* host = ActiveShellViewHost()) {
        int fvm = 0, iconPx = 0;
        if (host->GetViewMode(&fvm, &iconPx)) {
            const int idx = viewmode::ModeIndexFor(fvm, iconPx);
            if (idx >= 0) return idx;
        }
    }
    return viewMode_;  // 视图还没就绪时退回用户意图，不留空白
}

void App::RegisterStateProviders() {
    // 菜单行右端的快捷键文案：把解析器接到已加载的键位表上（唯一取数口）。
    //
    // 【为什么放在这里】它与下面两份 provider 是同一类东西 —— "把 App 的状态
    // 接给一个不依赖 App 的组件"。ui::Menu 是纯 UI，不能 include input_router；
    // 而"这条命令现在绑了什么键"只有 InputRouter 知道（用户覆盖、保留键剔除、
    // 先到先得都在 Reload 里算过）。
    //
    // 【为什么可以只注入一次】解析器每次 Show 现取（不缓存），用户改键只走
    // InputRouter::Reload，下一次弹菜单看到的就是新键 —— 这就是"自动"。
    //
    // 两个入口：首选按**命令身份**（菜单行的常规路径），备用按命令规格字符串
    // （纯 UI 局部动作的例外通道）。两者都汇到 keybinding 那一份推导。
    ui::Menu::SetShortcutResolver(
        [this](CmdId id, const std::wstring& arg) { return input_.ShortcutText(id, arg); },
        [this](const std::wstring& spec) { return input_.ShortcutText(spec); });

    // 命令前置条件的状态探测（业界 P4：运行前判定 + 可读原因）。
    // 与 ai::SetStateProvider 同一个原则：命令层不反向依赖 App，状态从外面注入。
    SetStateProbe([this] {
        CommandState st;
        st.tabCount = tabSet_.Count();
        if (Pane* p = ActivePane()) {
            st.historyBack = p->nav.backCount();     // idx 之前都可后退
            st.historyForward = p->nav.forwardCount();
            // 能否向上：唯一判定在 nav::CanNavigateUp（盘符根算"可向上"= 去此电脑）。
            st.canGoUp = nav::CanNavigateUp(p->path());
        }
        st.hasSelection = false;
        if (ShellView* host = ActiveShellViewHost()) {
            st.selectionCount = static_cast<int>(host->SelectedCount());
            st.hasSelection = st.selectionCount > 0;
        }
        st.hasClipboard = IsClipboardFormatAvailable(CF_HDROP) != FALSE;
        // 当前标签位置：用于判断"能不能左右移"（在最左/最右时该灰掉而不是静默无动作）。
        st.activeTabIndex = ActiveIndex();
        // 有没有可重新打开的已关闭标签。
        st.hasClosedTabs = !tabSet_.closed().empty();
        return st;
    });

    // AI 状态快照（架构规则 R19：与 UI 同源，不另存副本）。
    // 每次调用现取，避免"AI 看到的是十秒前的目录"。
    ai::SetStateProvider([this] {
        ai::StateSnapshot s;
        s.activeTab = ActiveIndex();
        s.activePath = ActivePath();
        s.viewMode = ActiveViewModeIndex();
        for (const auto& it : tabSet_.items()) s.tabPaths.push_back(it.path);
        Pane* activePane = ActivePane();
        if (activePane) s.itemCount = activePane->items;

        // 可写状态的回读（见 ai::StateSnapshot 的注释：能改就必须能读）。
        const Settings& cfg = Config::Get().s();
        s.sortMode = cfg.sortMode;
        s.groupBy = cfg.groupBy;
        s.bookmarkBarVisible = cfg.showBookmark;
        s.navPaneVisible = cfg.showNavPane;
        s.previewPaneVisible = cfg.showPreviewPane;
        s.dualPane = cfg.dualPane;
        s.activePane = activePane ? (Active() ? Active()->activePane : 0) : 0;
        s.paneSplit = cfg.paneSplit;
        s.paneVertical = cfg.paneVertical;
        // 两个窗格各自的目录：双栏下两栏可以完全不同，只报 activePath 的话
        // 调用方无法复查"另一栏在哪"，也就无法验证导航只打到了活动窗格。
        // 字段名 leftPanePath/rightPanePath 是**槽位**语义（0/1），
        // 左右模式=左/右，上下模式=上/下 —— 名字不改是为了不动 AI 面的字段契约。
        if (Tab* t = Active()) {
            const Pane* lp = t->panes[0].get();
            const Pane* rp = t->panes[1].get();
            if (lp) s.leftPanePath = lp->path();
            if (rp) s.rightPanePath = rp->path();
        }

        // 搜索态：source/query 取活动窗格自己的状态（R19 同源）。
        s.searching = activePane && !activePane->searchFrom.empty();
        s.searchScope = activePane ? activePane->searchFrom : std::wstring();
        s.searchQuery = activePane ? activePane->searchQuery : std::wstring();

        // 搜索域的**设置面**（与上面的搜索态不是一回事，别混）：
        s.searchRange = searchscope::RangeId(cfg.searchRange);
        s.searchRangePath = cfg.searchRangePath;
        s.searchHistoryCount = static_cast<int>(cfg.searchHistory.size());
        s.searchInput = navBar_.SearchText();

        // AI 面板可见性：与 ai.panel.toggle 的断言同源（面板自己就是真源，
        // 不去另存一个"我们以为开着"的副本 —— 那会在创建失败时说谎）。
        s.aiPanelVisible = aiPanel_.visible();
        // 与 chip 同源：原生启动态和真实页面状态都从这里回读。
        s.aiState = aiStatusState_;
        // 与 chip 上的圆点同源：AI 读回状态时看到的就是用户看到的。
        s.aiUnread = aiUnread_;

        // 面板排查面：端口 / 原因 / 视图三态。白屏时靠这三个字段定位，
        // 而不是让人去翻 %TEMP% 的日志（见 ai_api.h 的字段说明）。
        s.aiPanelPort = aiPanel_.servicePort();
        s.aiPanelError = aiPanel_.lastError();
        s.aiPanelWebView = aiPanel_.webViewState();

        // 键位覆盖表：hotkey.set/clear/reset 的回读面（与 Config 同一份真相）。
        s.hotkeyOverrides = cfg.hotkeyOverrides;

        // ---- 选中与焦点（观测面）----
        if (ShellView* host = ActiveShellViewHost()) {
            // 排序/分组键从 shell 现读：bykey 改的是 PROPERTYKEY，不能从 int 档位反推。
            if (IShellView* v = host->CurrentView()) {
                PROPERTYKEY sk{};
                bool asc = false;
                if (shellops::GetSortKey(v, &sk, &asc))
                    s.sortKey = shellcolumns::ColumnIdToString(sk);
                PROPERTYKEY gk{};
                if (shellops::GetGroupKey(v, &gk) && gk.fmtid != GUID_NULL)
                    s.groupKey = shellcolumns::ColumnIdToString(gk);
                v->Release();
            }
            constexpr unsigned kSelectionCap = 256;
            std::vector<ShellView::SelectionItem> sel;
            s.selectionCount = host->SelectionPaths(kSelectionCap, sel);
            s.selectionTruncated = sel.size() < s.selectionCount;
            for (const auto& it : sel) {
                s.selectionPaths.push_back(it.path);
                s.selectionFolders.push_back(it.isFolder);
            }
        }
        GUITHREADINFO gi = {sizeof(gi)};
        s.focus = (GetGUIThreadInfo(GetCurrentThreadId(), &gi) && gi.hwndFocus)
                      ? FocusPaneOf(gi.hwndFocus)
                      : L"unknown";

        // ---- 标签组 / 书签 / 会话的可回读面 ----
        s.tabCount = tabSet_.Count();
        if (const core::TabRef* ref = tabSet_.Find(activeId_)) {
            s.activeTabTitle = core::DisplayTitle(*ref);
            s.activeTabAlias = ref->alias;
        }
        s.activeTabIsLast = s.tabCount > 0 && s.activeTab == s.tabCount - 1;
        s.bookmarkCount = static_cast<int>(BookmarkStore::Get().CountLeaves());
        s.bookmarkTotalCount = static_cast<int>(BookmarkStore::Get().CountNodes());
        s.sessionSaved = !Config::Get().s().sessionPaths.empty();
        s.namedSessionCount = static_cast<int>(Config::Get().s().namedSessions.size());

        // ---- shell 隐藏项设置 ----
        const shellops::HiddenFlagsState hf = shellops::GetHiddenFlags();
        s.showHiddenFiles = hf.showAllObjects;
        s.showSystemFiles = hf.showSuperHidden;
        s.showExtensions = hf.showExtensions;
        s.shellHiddenState = hf.triState;
        return s;
    });
}
