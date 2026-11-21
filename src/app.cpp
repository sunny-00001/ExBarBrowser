#include "app.h"

#include "platform_win.h"

#include "ai_api.h"
#include "ai_server.h"
#include "app_commands.h"   // RunTerminal（面包屑段菜单的"在此打开终端"）
#include "commands.h"
#include "text_file.h"
#include "mouse_rules.h"
#include "shell_ops.h"
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
    navBar_.SetBackFn([] { CommandBus::Get().Invoke(CmdId::NavBack, Args::Empty(), L"ui"); });
    navBar_.SetForwardFn([] { CommandBus::Get().Invoke(CmdId::NavForward, Args::Empty(), L"ui"); });
    navBar_.SetUpFn([] { CommandBus::Get().Invoke(CmdId::NavUp, Args::Empty(), L"ui"); });
    navBar_.SetRefreshFn(
        [] { CommandBus::Get().Invoke(CmdId::NavRefresh, Args::Empty(), L"ui"); });
    // 视图菜单转投命令，不直接调 SetViewMode：
    // 命令 handler 会同时更新 viewMode_ 并写 settings.json，
    // 直接操作视图会让"用户点了详细信息、重启后还是小图标"。
    // 菜单给的是 fvm+icon 组合，这里反查回预留下标再走命令。
    navBar_.SetViewFn([](int fvm, int iconPx) {
        const int preset = ViewModeIndexFor(fvm, iconPx);
        if (preset >= 0)
            CommandBus::Get().InvokeLegacy(CmdId::ViewMode, std::to_wstring(preset), L"ui");
    });
    navBar_.SetHistoryFn([this](bool back) { return HistoryList(back); },
                         [this](int depth, bool back) { JumpHistory(depth, back); });
    navBar_.SetSettingsFn([this](POINT at) { ShowSettingsPanel(at); });
    navBar_.SetRecentFn([] { return Config::Get().s().recentPaths; });
    navBar_.SetClearHistoryFn([this] { ClearHistory(); });
    // 视图菜单打勾用：当前档位来自 viewMode_（用户偏好），排序/分组从**视图**现读。
    // 不从 Config 直接抄排序：用户可能在 shell 里点了列头反向排序，
    // 那时配置里的 sortAscending 是旧的 —— 菜单会打勾到错误的方向上。
    navBar_.SetViewStateFn([this] {
        NavBar::ViewState vs;
        vs.mode = viewMode_;
        vs.sort = Config::Get().s().sortMode;
        vs.group = Config::Get().s().groupBy;
        vs.ascending = Config::Get().s().sortAscending;
        if (IShellView* v = ActiveView()) {
            int gotMode = -1;
            bool gotAsc = true;
            if (shellops::GetSort(v, &gotMode, &gotAsc)) {
                if (gotMode >= 0 && gotMode < 4) vs.sort = gotMode;
                vs.ascending = gotAsc;
            }
            v->Release();
        }
        return vs;
    });
    // 同视图菜单：转投命令，否则排序/分组的选择不会落盘。
    navBar_.SetSortGroupFn([](int kind, int arg) {
        if (kind == 0)
            CommandBus::Get().InvokeLegacy(CmdId::ViewSort, std::to_wstring(arg), L"ui");
        else
            CommandBus::Get().InvokeLegacy(CmdId::ViewGroup, std::to_wstring(arg), L"ui");
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
    navBar_.SetSearchFn([this](const std::wstring& query) {
        Tab* t = Active();
        if (!t || !t->view) return;
        // 搜索来源目录：正在搜索时用记下来的那个，否则用当前目录。
        const std::wstring here = t->path();
        if (query.empty()) {
            if (!searchFrom_.empty()) {
                const std::wstring back = searchFrom_;
                searchFrom_.clear();
                NavigateActive(back, true);
            }
            return;
        }
        // 已经是搜索视图时，来源目录要用最早的（否则"在搜索结果里再搜"会搜到 0 项）。
        if (searchFrom_.empty()) searchFrom_ = here;
        // search-ms 语法：crumb 限定范围，query 是搜索词。
        // 双引号包住路径（可能含逗号/空格），词本身也转义引号。
        std::wstring q = query;
        for (size_t i = 0; i < q.size(); ++i)
            if (q[i] == L'"') q.insert(i++, 1, L'"');   // 引号翻倍转义
        const std::wstring url =
            L"search-ms:query=" + q + L"&crumb=location:" + searchFrom_ + L"&";
        if (!t->view->Navigate(url)) {
            LogDbg(L"[search] 无法搜索：%s", url.c_str());
            MessageBeep(MB_ICONWARNING);
        }
    });

    // 面包屑段右键：动作作用于"右键的那一段"，不是当前目录。
    // 这里把段路径转成既有的命令/入口，不新写一套文件语义：
    // 复制走剪贴板原语、终端走 RunTerminal、属性走 shell 动词。
    navBar_.SetSegmentCmdFn([this](const std::wstring& path, NavBar::SegCmd action) {
        switch (action) {
        case NavBar::kSegOpenNewTab:
            NewTab(path, NewTabIndex());
            break;
        case NavBar::kSegOpenNewWindow:
            OpenPathInNewWindow(path);
            break;
        case NavBar::kSegCopyPath:
            CopyText(path);
            break;
        case NavBar::kSegTerminal:
            RunTerminal(hwnd_, path, false);
            break;
        case NavBar::kSegProperties:
            ShowPathProperties(path);
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
        case Seg_Chk: CommandBus::Get().InvokeLegacy(CmdId::CmdSelectAll, L"", L"ui"); break;
        case Seg_Date: CommandBus::Get().InvokeLegacy(CmdId::ViewSort, L"1", L"ui"); break;
        case Seg_Path: CommandBus::Get().InvokeLegacy(CmdId::PathCopyDir, L"", L"ui"); break;
        case Seg_Size: CommandBus::Get().InvokeLegacy(CmdId::CmdProps, L"", L"ui"); break;
        case Seg_Disk: CommandBus::Get().InvokeLegacy(CmdId::GoThisPC, L"", L"ui"); break;
        case Seg_Eye: CommandBus::Get().InvokeLegacy(CmdId::CycleHiddenSeg, L"", L"ui"); break;
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
            for (const auto& p : s.sessionPaths)
                if (tf::Exists(p) || p.rfind(L"::", 0) == 0) NewTab(p);
            int idx = s.sessionActive;
            if (idx >= 0 && idx < tabSet_.Count()) Activate(tabSet_.IdAt(idx));
            // 会话里的目录可能全被删了：别开一个没有标签的空窗口。
            if (tabSet_.Count() == 0) NewTab(HomePath());
        } else {
            NewTab(HomePath());
        }
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    Layout();

    // AI 通道：标签就绪后才启（状态快照要能取到活动标签）。
    // 默认关闭；只有 settings.json 里 aiEnabled=true 才监听管道。
    if (Config::Get().s().aiEnabled) {
        ai::StartServer(nullptr);
        // 泵定时器：命令 handler 必须在 UI 线程跑，监听线程只排队。
        // 100ms 足够（人感知不到），又不会占 CPU。
        SetTimer(hwnd_, kAiTimer, 100, nullptr);
    } else {
        LogDbg(L"[ai] 通道未启用（aiEnabled=false）");
    }

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
    if (gm == 0) return 0;
    LogDbg(L"[loop] GetMessage 失败 error=%lu", GetLastError());
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

    std::vector<std::wstring> paths;
    for (const auto& t : tabSet_.items())
        if (!t.path.empty()) paths.push_back(t.path);
    cfg.RememberSession(paths, ActiveIndex());
    s.closedTabs = tabSet_.closed();
    cfg.Save();

    // 只做"保存 + 停表"：Shell 视图与字体必须等 DestroyWindow 之后
    // （WM_DESTROY 里）再释放，否则子窗口在销毁过程中重绘会用到已删字体。
    KillTimer(hwnd_, kStatsTimer);
    KillTimer(hwnd_, kAiTimer);
    ai::StopServer();   // 先停监听线程，避免它持有请求排队时窗口已销毁
    ai::SetStateProvider(nullptr);
    input_.Cancel();
    tabs_.clear();  // 字体仍有效，Shell 视图可安全销毁
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
