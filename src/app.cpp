#include "app.h"

#include "platform_win.h"

#include "ai_api.h"
#include "ai_server.h"
#include "app_commands.h"   // RunTerminal（面包屑段菜单的"在此打开终端"）
#include "commands.h"
#include "mcp_stdio.h"      // --mcp-stdio：stdio ⇄ 命名管道 桥接
#include "text_file.h"
#include "mouse_rules.h"
#include "shell_ops.h"
#include "shell_columns.h"   // 属性键文本格式的唯一来源（{GUID}:pid）
#include "view_modes.h"      // ModeIndexFor：从 shell 现读档位（D2）
#include "view_stats.h"

namespace {

// 主图标资源 ID（见 resources/app.rc：1 ICON "resources/app.ico"）
constexpr int kAppIconId = 1;

// 从 exe 资源加载应用图标；加载失败时回退到系统默认，保证窗口一定可显示。
HICON LoadAppIcon() {
    HICON h = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(kAppIconId));
    return h ? h : LoadIconW(nullptr, IDI_APPLICATION);
}

}  // namespace

App& App::Get() {
    static App instance;
    return instance;
}

std::wstring App::HomePath() {
    PWSTR profile = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) {
        path = profile;
        CoTaskMemFree(profile);
    }
    return path;
}

bool App::CreateMainWindow(HINSTANCE hInst) {
    win::RegisterClassOnce(kMainClass, WndProc, 0, LoadAppIcon());

    const Settings& s = Config::Get().s();
    int w = s.hasGeometry ? s.winW : 1100;
    int h = s.hasGeometry ? s.winH : 760;
    int x = s.hasGeometry ? s.winX : CW_USEDEFAULT;
    int y = s.hasGeometry ? s.winY : CW_USEDEFAULT;

    // 几何校验：显示器可能换过/分辨率变过，把窗口夹回工作区（至少留 240x160 可见）
    if (s.hasGeometry) {
        RECT want{x, y, x + w, y + h};
        HMONITOR mon = MonitorFromRect(&want, MONITOR_DEFAULTTONULL);
        if (!mon) mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            const RECT& work = mi.rcWork;
            w = std::min(w, static_cast<int>(work.right - work.left));
            h = std::min(h, static_cast<int>(work.bottom - work.top));
            // 整个窗口夹回工作区：地址框右端不再跑出屏幕，弹层才能与它对齐
            x = std::max(static_cast<int>(work.left),
                         std::min(x, static_cast<int>(work.right) - w));
            y = std::max(static_cast<int>(work.top),
                         std::min(y, static_cast<int>(work.bottom) - h));
        }
    }

    hwnd_ = CreateWindowExW(0, kMainClass, id::kBrandShort, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w,
                            h, nullptr, nullptr, hInst, nullptr);
    if (hwnd_) {
        // 类 hIcon 负责标题栏/任务栏；显式送两档让 Alt-Tab、缩略图也用上新图标
        HICON appIcon = LoadAppIcon();
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
        if (s.hasGeometry && s.maximized) ShowWindow(hwnd_, SW_MAXIMIZE);
    }
    return hwnd_ != nullptr;
}

void App::BuildSystemMenu() {
    // WS_OVERLAPPEDWINDOW 已含 WS_SYSMENU：标准的"还原/移动/大小/最小化/最大化/
    // 关闭"一直在。这里补的是**应用自己的入口** —— 视图菜单、命令面板、首选项、
    // 快捷键管理器、退出，让它们像 explore 一样可从标题栏右键 / Alt+Space 到达。
    // 之前这些只有导航栏按钮与命令面板两个入口，系统菜单完全没有。
    HMENU sys = GetSystemMenu(hwnd_, FALSE);
    if (!sys) return;
    // 幂等：主题切换/重建窗口时可能重进；用自定义项的 ID 存在性判重，
    // 避免每次 OnCreate 都追加一份重复菜单项。
    MENUITEMINFOW probe{};
    probe.cbSize = sizeof(probe);
    if (GetMenuItemInfoW(sys, kSysViewMenu, FALSE, &probe)) return;

    AppendMenuW(sys, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sys, MF_STRING, kSysViewMenu, L"视图菜单(&V)");
    AppendMenuW(sys, MF_STRING, kSysPalette, L"命令面板(&C)");
    AppendMenuW(sys, MF_STRING, kSysPrefs, L"首选项(&P)");
    AppendMenuW(sys, MF_STRING, kSysHotkeys, L"快捷键管理器(&K)");
    AppendMenuW(sys, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sys, MF_STRING, kSysQuit, L"退出(&X)");
}

void App::OnCreate() {
    theme_.Init(hwnd_);
    win::RegisterClassOnce(kViewAreaClass, DefWindowProcW);

    viewArea_ = CreateWindowExW(0, kViewAreaClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0,
                                0, 10, 10, hwnd_, nullptr, hInst_, nullptr);
    theme_.ApplyToViewArea(viewArea_);

    tabStrip_.Create(hwnd_, theme_);
    navBar_.Create(hwnd_, theme_);
    bookmarkBar_.Create(hwnd_, theme_);
    statusBar_.Create(hwnd_, theme_);

    navBar_.SetNavFn([this](const std::wstring& p) { NavigateActive(p, true); });
    // 离开地址编辑（Enter/Esc/Tab）后把焦点还回视图，否则热键/type-ahead 全哑。
    navBar_.SetRefocusFn([this] { FocusActiveView(); });
    // 导航按钮一律经 CommandBus，与双击/热键/序列走**同一条**路径：
    // 前置检查（CanExecute）、审计、前置拦截只有一个入口。以前这里直接调
    // GoUp()/GoBack()，绕过命令总线 —— 于是同一个"上一级"按钮能跳、双击没反应
    // （命令被 Req::CanGoUp 拦掉），两种行为分裂。
    navBar_.SetBackFn([] { CommandBus::Get().Invoke(CmdId::NavBack, Args::Empty(), Actor::Ui()); });
    navBar_.SetForwardFn([] { CommandBus::Get().Invoke(CmdId::NavForward, Args::Empty(), Actor::Ui()); });
    navBar_.SetUpFn([] { CommandBus::Get().Invoke(CmdId::NavUp, Args::Empty(), Actor::Ui()); });
    navBar_.SetRefreshFn(
        [] { CommandBus::Get().Invoke(CmdId::NavRefresh, Args::Empty(), Actor::Ui()); });
    // 视图菜单转投命令，不直接调 SetViewMode：
    // 命令 handler 会同时更新 viewMode_ 并写 settings.json，
    // 直接操作视图会让"用户点了详细信息、重启后还是小图标"。
    // 菜单给的是 fvm+icon 组合，这里反查回预留下标再走命令。
    navBar_.SetViewFn([](int fvm, int iconPx) {
        const int preset = ViewModeIndexFor(fvm, iconPx);
        if (preset >= 0)
            CommandBus::Get().InvokeLegacy(CmdId::ViewMode, std::to_wstring(preset), Actor::Ui());
    });
    navBar_.SetHistoryFn([this](bool back) { return HistoryList(back); },
                         [this](int depth, bool back) { JumpHistory(depth, back); });
    navBar_.SetSettingsFn([this](POINT at) { ShowSettingsPanel(at); });
    navBar_.SetColumnsFn([this](POINT at) { ShowColumnMenu(at); });
    navBar_.SetZoomFn([this](POINT at) { ShowZoomMenu(at); });
    navBar_.SetRecentFn([] { return Config::Get().s().recentPaths; });
    navBar_.SetClearHistoryFn([this] { (void)ClearHistory(); });
    // 视图菜单打勾用：档位/排序/分组/方向**全部**从 shell 视图现读（D2）。
    // 以前 mode 抄 viewMode_（用户意图）、group/sort 抄 Config（上次落盘），
    // 于是用户在 shell 里点列头、改分组、换视图档位后，菜单打勾停在旧值 ——
    // 用户看到的是"点了没反应"，其实是菜单在说谎。配置只是"意图"，
    // 菜单要回答"现在是什么"，两者不能混用（R19 同源）。
    navBar_.SetViewStateFn([this] {
        NavBar::ViewState vs;
        vs.mode = viewMode_;       // 读不到视图时退回用户意图，不留空白
        vs.sort = Config::Get().s().sortMode;
        vs.group = Config::Get().s().groupBy;
        vs.ascending = Config::Get().s().sortAscending;
        if (Tab* t = Active(); t && t->view) {
            if (IShellView* v = t->view->CurrentView()) {
                int gotMode = -1;
                bool gotAsc = true;
                if (shellops::GetSort(v, &gotMode, &gotAsc)) {
                    if (gotMode >= 0 && gotMode < 4) vs.sort = gotMode;
                    vs.ascending = gotAsc;
                }
                // 任意列排序 GetSort 认不出 → 保留配置档位；菜单小结用 sortKey
                // （下面现读）显示真实列，不给用户看一个打在错误项上的勾。
                PROPERTYKEY sk{};
                if (shellops::GetSortKey(v, &sk, &gotAsc)) {
                    vs.sortKey = shellcolumns::ColumnIdToString(sk);
                    vs.ascending = gotAsc;
                }
                // 分组同理：内置四档映射到下标，任意键留 -1（小结用 groupKey）。
                PROPERTYKEY gk{};
                if (shellops::GetGroupKey(v, &gk) && gk.fmtid != GUID_NULL) {
                    vs.groupKey = shellcolumns::ColumnIdToString(gk);
                    const std::wstring id = vs.groupKey;
                    for (int i = 0; i < 4; ++i) {
                        PROPERTYKEY known{};
                        // 内置档位与 shell_ops::SetGroup 的 0..3 映射同源。
                        const PROPERTYKEY* p = nullptr;
                        if (i == 1) p = &PKEY_ItemTypeText;
                        else if (i == 2) p = &PKEY_DateModified;
                        else if (i == 3) p = &PKEY_Size;
                        if (p && shellcolumns::ColumnIdFromString(id, &known) &&
                            IsEqualPropertyKey(known, *p)) { vs.group = i; break; }
                    }
                } else {
                    vs.group = 0;  // shell 明确说没分组
                }
                v->Release();
            }
            int fvm = 0, iconPx = 0;
            if (t->view->GetViewMode(&fvm, &iconPx)) {
                const int idx = viewmode::ModeIndexFor(fvm, iconPx);
                if (idx >= 0) vs.mode = idx;
            }
        }
        return vs;
    });
    // 同视图菜单：转投命令，否则排序/分组的选择不会落盘。
    navBar_.SetSortGroupFn([](int kind, int arg) {
        if (kind == 0)
            CommandBus::Get().InvokeLegacy(CmdId::ViewSort, std::to_wstring(arg), Actor::Ui());
        else
            CommandBus::Get().InvokeLegacy(CmdId::ViewGroup, std::to_wstring(arg), Actor::Ui());
    });

    bookmarkBar_.SetNavigateFn([this](const std::wstring& p) { NavigateActive(p, true); });
    // 书签落盘失败（被外部改动 / 加载失败 / 文件被占用）时统一提示，
    // 见 App::NotifyBookmarkSave。回执挂在 Store 层，覆盖所有 Save 调用点
    // （书签栏 / 拖放 / 导入 / 命令面板），不再依赖某个控件转交。
    BookmarkStore::Get().SetSaveNotifyFn([this](bool ok) { NotifyBookmarkSave(ok); });
    // 书签栏的键盘态按 Esc 退出后要把焦点还给视图，否则 Delete / type-ahead 全哑。
    bookmarkBar_.SetRefocusFn([this] { FocusActiveView(); });
    // 导航栏搜索框提交：用 shell 自己的 search-ms 协议在当前目录里搜。
    //
    // 为什么不自建扫描：那要么阻塞 UI（大目录几十万项），要么起线程/helper
    // （另一套索引与用户看到的视图不一致）。search-ms 是资源管理器自己走的路，
    // 结果就在同一个 IShellView 里呈现 —— 排序、右键、拖放全部照旧可用。
    //
    // 空串 = 取消搜索：回到原目录（搜索视图没有"上一级"，只能记住来源）。
    navBar_.SetSearchFn([this](const std::wstring& query) { RunSearch(query, nullptr); });

    // 面包屑段右键：动作作用于"右键的那一段"，不是当前目录。
    // 能点名目标的动作一律转投命令（前置检查/审计/面板/AI 同源）：
    //   · 新标签 → tab.new {path}     · 新窗口 → fs.open.window {path}
    //   · 属性   → fs.properties {target}
    //   · 复制   → clipboard.copy {text}
    //   · 终端   → ext.terminal.at {path}
    // 这五项在批次 2 全部转成 CommandBus，右键菜单里不再直接碰剪贴板/进程。
    navBar_.SetSegmentCmdFn([this](const std::wstring& path, NavBar::SegCmd action) {
        auto withPath = [&](const wchar_t* key) {
            json::Value a = json::Value::MakeObject();
            a.Set(key, json::Value::MakeStr(path));
            return Args::FromJson(a);
        };
        switch (action) {
        case NavBar::kSegOpenNewTab:
            CommandBus::Get().Invoke(CmdId::TabNew, withPath(L"path"), Actor::Ui());
            break;
        case NavBar::kSegOpenNewWindow:
            CommandBus::Get().Invoke(CmdId::FsOpenInNewWindow, withPath(L"path"), Actor::Ui());
            break;
        case NavBar::kSegCopyPath:
            CommandBus::Get().Invoke(CmdId::ClipboardCopy, withPath(L"text"), Actor::Ui());
            break;
        case NavBar::kSegTerminal:
            CommandBus::Get().Invoke(CmdId::ExtTerminalAt, withPath(L"path"), Actor::Ui());
            break;
        case NavBar::kSegProperties:
            CommandBus::Get().Invoke(CmdId::FsProperties, withPath(L"target"), Actor::Ui());
            break;
        }
    });

    // 命令前置条件的状态探测（业界 P4：运行前判定 + 可读原因）。
    // 与 ai::SetStateProvider 同一个原则：命令层不反向依赖 App，状态从外面注入。
    SetStateProbe([this] {
        CommandState st;
        st.tabCount = tabSet_.Count();
        if (Tab* t = Active()) {
            st.historyBack = t->nav.backCount();     // idx 之前都可后退
            st.historyForward = t->nav.forwardCount();
            // 能否向上：唯一判定在 nav::CanNavigateUp（盘符根算"可向上"= 去此电脑）。
            // 这里不再自己写 if —— 命令前置检查与 GoUp 的执行必须是同一份口径，
            // 否则会出现"按钮能点、双击没反应"。
            st.canGoUp = nav::CanNavigateUp(t->path());
        }
        st.hasSelection = false;
        if (Tab* t = Active()) {
            if (t->view) {
                // SelectedCount 返回 unsigned，selectionCount 是 int（HasSelectionSingle
                // 用）。选中项数不可能接近 INT_MAX（那是同时选中 21 亿个文件），
                // 显式转换表达"这里的有损是安全的"，而不是靠隐式转换混过去。
                st.selectionCount = static_cast<int>(t->view->SelectedCount());
                st.hasSelection = st.selectionCount > 0;
            }
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
        s.viewMode = viewMode_;
        for (const auto& it : tabSet_.items()) s.tabPaths.push_back(it.path);
        if (Tab* t = Active()) s.itemCount = t->items;

        // 可写状态的回读（见 ai::StateSnapshot 的注释：能改就必须能读）。
        // 一律取**同一份**真源 —— 排序/分组/窗格/书签栏都在 Settings 里，
        // 不在这里另存副本（R19）。另存副本会让"命令改了 A、回读读到 B"这种
        // 分叉只在用户发现界面与 AI 说法不一致时才暴露。
        const Settings& cfg = Config::Get().s();
        s.sortMode = cfg.sortMode;
        s.groupBy = cfg.groupBy;
        s.bookmarkBarVisible = cfg.showBookmark;
        s.navPaneVisible = cfg.showNavPane;
        s.previewPaneVisible = cfg.showPreviewPane;

        // 搜索态：source/query 是 RunSearch 的同一份状态（R19 同源），
        // 不另存副本。空 scope = 不在搜索里。
        s.searching = !searchFrom_.empty();
        s.searchScope = searchFrom_;
        s.searchQuery = searchQuery_;

        // ---- 选中与焦点（观测面）----
        // 与命令前置同一来源（活动标签的 shell 视图），R19 同源不另存副本。
        // 选中路径走 shell 枚举（IFolderView::Items），快照带上限防响应爆炸。
        if (Tab* t = Active(); t && t->view) {
            // 排序/分组键从 shell 现读（R19 同源）：bykey 命令改的是 shell 的
            // PROPERTYKEY，回读必须读同一处，不能从 int 档位反推 —— 任意属性
            // 本来就不在 0..3 里，反推会得到空的假值。
            if (IShellView* v = t->view->CurrentView()) {
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
            s.selectionCount = t->view->SelectionPaths(kSelectionCap, sel);
            s.selectionTruncated = sel.size() < s.selectionCount;
            for (const auto& it : sel) {
                s.selectionPaths.push_back(it.path);
                s.selectionFolders.push_back(it.isFolder);
            }
        }
        GUITHREADINFO gi = { sizeof(gi) };
        s.focus = (GetGUIThreadInfo(GetCurrentThreadId(), &gi) && gi.hwndFocus)
                      ? FocusPaneOf(gi.hwndFocus)
                      : L"unknown";

        // ---- 标签组 / 书签 / 会话的可回读面（自断言要用）----
        // 全部取**同一份真源**：标签数与标题来自 tabSet_（标签栏画的就是它），
        // 书签计数来自 BookmarkStore 的扁平叶子数（与 bmk.list 同口径），
        // 会话状态来自 Settings（与 session.list 同口径）。
        s.tabCount = tabSet_.Count();
        if (const core::TabRef* ref = tabSet_.Find(activeId_)) {
            s.activeTabTitle = core::DisplayTitle(*ref);
            s.activeTabAlias = ref->alias;
        }
        // "是不是最后一个"是派生布尔：断言语言没有算术，
        // 而 tab.activate.last 的判据恰好是它（见 ai_api.h 的说明）。
        s.activeTabIsLast = s.tabCount > 0 && s.activeTab == s.tabCount - 1;
        s.bookmarkCount = static_cast<int>(BookmarkStore::Get().CountLeaves());
        s.bookmarkTotalCount = static_cast<int>(BookmarkStore::Get().CountNodes());
        s.sessionSaved = !Config::Get().s().sessionPaths.empty();
        s.namedSessionCount = static_cast<int>(Config::Get().s().namedSessions.size());

        // ---- shell 隐藏项设置（view.toggle.* 与 view.cycle.hidden 的回读面）----
        // 与写侧同一份 SHELLSTATE、同一掩码（shell_ops::GetHiddenFlags）——
        // 另存副本会让"读到的不是刚改的那个开关"。
        const shellops::HiddenFlagsState hf = shellops::GetHiddenFlags();
        s.showHiddenFiles = hf.showAllObjects;
        s.showSystemFiles = hf.showSuperHidden;
        s.showExtensions = hf.showExtensions;
        s.shellHiddenState = hf.triState;
        return s;
    });

    MouseSink sink = [this](int target, UINT msg, int button, int mods, POINT pt) {
        return OnChromeMouse(target, msg, button, mods, pt);
    };
    tabStrip_.SetMouseSink(sink);
    navBar_.SetMouseSink(sink);
    bookmarkBar_.SetMouseSink(sink);
    statusBar_.SetMouseSink(sink);

    statusBar_.SetClickFn([](int seg) {
        switch (seg) {
        case Seg_Chk: CommandBus::Get().InvokeLegacy(CmdId::CmdSelectAll, L"", Actor::Ui()); break;
        case Seg_Date: CommandBus::Get().InvokeLegacy(CmdId::ViewSort, L"1", Actor::Ui()); break;
        case Seg_Path: CommandBus::Get().InvokeLegacy(CmdId::PathCopyDir, L"", Actor::Ui()); break;
        case Seg_Size: CommandBus::Get().InvokeLegacy(CmdId::CmdProps, L"", Actor::Ui()); break;
        case Seg_Disk: CommandBus::Get().InvokeLegacy(CmdId::GoThisPC, L"", Actor::Ui()); break;
        case Seg_Eye: CommandBus::Get().InvokeLegacy(CmdId::CycleHiddenSeg, L"", Actor::Ui()); break;
        default: break;
        }
    });
    statusBar_.SetContextFn([this](POINT pt) { ShowStatusMenu(pt); });

    input_.Init(hwnd_, theme_);
    RegisterCommands();

    Config& cfg = Config::Get();
    statusBar_.SetEnabledSegments(cfg.s().statusSegs);
    BookmarkStore::Get().Load(cfg.BookmarksPath());
    bookmarkBar_.Reload();

    SetTimer(hwnd_, kStatsTimer, 800, nullptr);
    ApplyDarkTitleBar(hwnd_, theme_.dark);
    BuildSystemMenu();
}

int App::Run(HINSTANCE hInst, const std::wstring& initialPath) {
    hInst_ = hInst;
    Config::Get().Load();
    if (!CreateMainWindow(hInst)) return 1;

    // 关闭历史是"可重开"的跨会话状态：以前只保存不读取，Ctrl+Shift+T 重启后永远空。
    tabSet_.SetClosed(Config::Get().s().closedTabs);
    viewMode_ = Config::Get().s().viewMode;

    // 启动标签：显式路径 > 上次会话 > 主目录
    if (!initialPath.empty()) {
        NewTab(initialPath);
    } else {
        const Settings& s = Config::Get().s();
        if (!s.sessionPaths.empty()) {
            for (size_t i = 0; i < s.sessionPaths.size(); ++i) {
                const std::wstring& p = s.sessionPaths[i];
                if (!(tf::Exists(p) || p.rfind(L"::", 0) == 0)) continue;
                const int id = NewTab(p);
                // 每标签历史：对齐 sessionPaths 的下标（跳过已失效路径时也不移位）。
                // Restore 只装填栈，不重放导航 —— 打开成本仍是"一次"。
                if (id && i < s.sessionHistory.size()) {
                    const TabHistory& h = s.sessionHistory[i];
                    if (Tab* t = ById(id); t && !h.paths.empty()) {
                        t->nav.Restore(h.paths, h.idx);
                        SyncChrome();
                    }
                }
            }
            int idx = s.sessionActive;
            if (idx >= 0 && idx < tabSet_.Count()) Activate(tabSet_.IdAt(idx));
            // 会话里的目录可能全被删了：别开一个没有标签的空窗口。
            if (tabSet_.Count() == 0) NewTab(HomePath());
        } else {
            NewTab(HomePath());
        }
        // 会话选中恢复：挂到**活动标签**的目标目录上，等首次导航完成时消费
        // （视图就绪前 SelectItem 无效）。用户立刻导航去别处 = 放弃恢复。
        if (s.sessionActive >= 0 && s.sessionActive < static_cast<int>(s.sessionPaths.size())
            && !s.sessionSelection.empty()) {
            pendingSelPath_ = s.sessionPaths[s.sessionActive];
            pendingSelItems_ = s.sessionSelection;
        }
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    Layout();

    // AI 通道：标签就绪后才启（状态快照要能取到活动标签）。
    // 默认开启；settings.json 的 aiEnabled=false 才关闭。
    // 运行中改 aiEnabled 立即生效（WM_APP_SYNC_AI -> SyncAiChannel），无需重启。
    SyncAiChannel();

    // MCP over stdio（--mcp-stdio）：把 stdin/stdout 接到上面那条命名管道上，
    // 让标准 MCP 客户端能直接驱动本程序。
    //
    // 【为什么放在 SyncAiChannel 之后】桥接要连的正是刚启动的管道；
    // 顺序反了就可能连到一个还没建好的名字。
    // 【为什么独立于 aiEnabled 开关】stdio 是**进程启动方式**（客户端拉起我们），
    // 不是"要不要 AI"：客户端既然用 stdio 拉起本进程，就说明它要 AI 通道。
    // 所以它在 App::Run 里无条件起（Requested() 内部自己判断命令行），
    // 不随运行中热插拔的 aiEnabled 反复起停。
    mcpstdio::Start(hwnd_);

    MSG msg;
    int gm;
    while ((gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            HWND focus = GetFocus();
            HWND root = focus ? GetAncestor(focus, GA_ROOT) : nullptr;
            if (root == hwnd_ && input_.HandleKey(msg.wParam, focus, msg.lParam)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // GetMessage 返回 -1 是错误（不能继续循环），0 是 WM_QUIT。
    if (gm == 0) {
        // 兜底停桥接：正常路径 OnDestroy 已经停过，这里再调一次是幂等的。
        // 万一有路径绕过 WM_CLOSE 直接 PostQuitMessage，这里也能收干净，
        // 不会留下一个 joinable 的静态线程在进程退出时 std::terminate()。
        mcpstdio::Stop();
        return 0;
    }
    LogDbg(L"[loop] GetMessage 失败 error=%lu", GetLastError());
    mcpstdio::Stop();
    return 1;
}

// ---------------------------------------------------------------- 输入
bool App::OnChromeMouse(int target, UINT msg, int button, int mods, POINT pt) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        return input_.OnMouseDown(target, button, mods, pt);
    case WM_APP_MOUSE_DOWN_NOUP:  // Shell WM_PARENTNOTIFY：没有配对 UP
        return input_.OnMouseDown(target, button, mods, pt, false);
    case WM_APP_MOUSE_UP_NOUP:
        return input_.OnMouseUp(target, button, mods);
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_XBUTTONUP:
        return input_.OnMouseUp(target, button, mods);
    case WM_MOUSEWHEEL:
        return input_.OnWheel(target, mods, button, false);
    case WM_MOUSEHWHEEL:
        return input_.OnWheel(target, mods, button, true);
    case WM_MOUSEMOVE:
        // 移动本身永远不吞（返回 false）；只用来取消"按下后拖动 => 长按"。
        input_.OnMouseMove(pt);
        return false;
    default:
        return false;
    }
}

bool App::ConfirmExit() {
    // MCP 模式（--mcp-stdio）：这个进程是 MCP 客户端拉起来的服务器，
    // 客户端断开即应静默退出 —— 弹框问"确定退出吗"会把客户端晾在原地。
    if (mcpstdio::Requested()) return true;
    // 只有多标签才问：最后一个标签关闭 = 正常结束，不该再拦一次。
    // 默认按钮是"否"：误按 Enter 的常见场景（Alt+F4 后手还没离开键盘）
    // 不该把整组标签关掉；会话虽能恢复，但用户此刻未必想退出。
    if (!Config::Get().s().confirmCloseTabs) return true;
    const int n = tabSet_.Count();
    if (n <= 1) return true;
    const std::wstring text =
        L"当前打开了 " + std::to_wstring(n) + L" 个标签页。\n\n确定要退出吗？";
    return MessageBoxW(hwnd_, text.c_str(), L"退出 ExBarBrowser",
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

void App::MarkSessionDirty() {
    if (!hwnd_) return;
    // 去抖：每次变更都把 2s 计时重新开始，连续操作（新建一批标签）只落盘一次。
    SetTimer(hwnd_, kSessionTimer, 2000, nullptr);
}

void App::SaveSessionSnapshot() {
    if (!hwnd_) return;
    Config& cfg = Config::Get();
    std::vector<std::wstring> paths;
    for (const auto& t : tabSet_.items())
        if (!t.path.empty()) paths.push_back(t.path);
    // 选中状态一并入会话：重启后活动标签的选中项可恢复（尽力语义）。
    std::vector<std::wstring> selPaths;
    if (Tab* t = Active(); t && t->view) {
        std::vector<ShellView::SelectionItem> sel;
        t->view->SelectionPaths(256, sel);
        for (const auto& it : sel) selPaths.push_back(it.path);
    }
    cfg.RememberSession(paths, ActiveIndex(), selPaths, [&] {
        // 与 paths 同序、同数量：每个非空路径对应一份 nav 历史。
        std::vector<TabHistory> hist;
        hist.reserve(paths.size());
        for (const auto& t : tabSet_.items()) {
            if (t.path.empty()) continue;
            TabHistory h;
            if (Tab* tab = ById(t.id)) {
                h.paths = tab->nav.entries();
                h.idx = tab->nav.idx();
            }
            hist.push_back(std::move(h));
        }
        return hist;
    }());
    cfg.mutableSettings().closedTabs = tabSet_.closed();
    cfg.Save();
}

void App::OnDestroy() {
    if (inDestroy_) return;
    inDestroy_ = true;
    // Store 是进程级单例，回调捕获了 this；窗口销毁后不能再让它弹提示。
    BookmarkStore::Get().SetSaveNotifyFn(nullptr);

    Config& cfg = Config::Get();
    Settings& s = cfg.mutableSettings();

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        s.hasGeometry = true;
        s.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
        s.winX = wp.rcNormalPosition.left;
        s.winY = wp.rcNormalPosition.top;
        s.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        s.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }

    // 会话快照与自动快照共用一份实现（含 closedTabs 与 Save）。
    SaveSessionSnapshot();

    // 只做"保存 + 停表"：Shell 视图与字体必须等 DestroyWindow 之后
    // （WM_DESTROY 里）再释放，否则子窗口在销毁过程中重绘会用到已删字体。
    KillTimer(hwnd_, kStatsTimer);
    KillTimer(hwnd_, kAiTimer);
    KillTimer(hwnd_, kSessionTimer);
    // 先停 stdio 桥接：它可能正阻塞在 stdin 上，要靠 CancelSynchronousIo 唤醒。
    // 放在 ai::StopServer() 前面，让桥接线程在管道还在的时候干净地退出。
    mcpstdio::Stop();
    ai::StopServer();   // 先停监听线程，避免它持有请求排队时窗口已销毁
    ai::SetStateProvider(nullptr);
    // 长任务作业：取消全部并 join 工作线程（实现见 app_commands_jobs.cpp）。
    ShutdownJobs();
    input_.Cancel();
    tabs_.clear();  // 字体仍有效，Shell 视图可安全销毁
}

void App::SyncAiChannel() {
    const bool want = Config::Get().AiChannelEnabled();
    const bool running = ai::IsRunning();
    if (want == running) return;

    if (want) {
        ai::StartServer(nullptr);
        // 泵定时器：命令 handler 必须在 UI 线程跑，监听线程只排队。
        // 100ms 足够（人感知不到），且无订阅者时不做状态轮询（见 PumpRequests）。
        SetTimer(hwnd_, kAiTimer, 100, nullptr);
        LogDbg(L"[ai] 通道已启用（热插拔）");
    } else {
        // 先停定时器再停服务：反过来的话，下一次 WM_TIMER 会对已停的管道泵请求。
        KillTimer(hwnd_, kAiTimer);
        ai::StopServer();
        LogDbg(L"[ai] 通道已停用（热插拔）");
    }
}

void App::FocusActiveView() {
    // 活动标签的容器窗口本身就是 tab view（ShellView::hwnd），焦点给它，
    // 内部的 shell 视图会接过键盘（type-ahead、方向键、Delete 都由 shell 处理）。
    Tab* t = Active();
    if (!t || !t->view) return;
    HWND target = t->view->hwnd();
    if (!target) {
        if (viewArea_) SetFocus(viewArea_);
        return;
    }
    SetFocus(target);
}

void App::OpenExternal(const std::wstring& path) {
    if (path.empty()) return;
    NewTab(path);
}
