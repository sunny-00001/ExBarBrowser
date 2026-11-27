#include "app.h"

#include "platform_win.h"

#include "ai_api.h"
#include "ai_server.h"
#include "aichat_service.h"   // node 服务子进程（面板的前置条件）
#include "aichat_webview_msg.h"  // 页面 → 宿主的 status WebMessage
#include "app_commands.h"   // RunTerminal（面包屑段菜单的"在此打开终端"）
#include "app_input_config.h"  // InputConfig 适配（输入端不直接依赖 Config，R15）
#include "commands.h"
#include "kb_hook.h"        // 低层键盘钩子（救中文输入法吃掉的字母键）
#include "mcp_stdio.h"      // --mcp-stdio：stdio ⇄ 命名管道 桥接
#include "pane_ui.h"        // 双栏族：视图菜单接线 + 打勾状态（app_pane_chips.cpp）
#include "search_scope.h"   // searchscope::RangeId（搜索范围档位 → id）
#include "text_file.h"
#include "mouse_rules.h"
#include "shell_ops.h"
#include "shell_columns.h"   // 属性键文本格式的唯一来源（{GUID}:pid）
#include "view_modes.h"      // ModeIndexFor：从 shell 现读档位（D2）
#include "view_stats.h"

#include <map>
#include <memory>

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
    // 栏高必须在任何一条栏/导航树建窗之前落地：ChromeHeight 与 Create() 都读 Theme。
    ApplyBarHeights();
    win::RegisterClassOnce(kViewAreaClass, DefWindowProcW);
    LogDbg(L"[boot] OnCreate 开始（主题已初始化）");

    // WS_CLIPSIBLINGS：导航树、两个文件视图、AI 面板和三条边界条现在全是
    // viewArea_ 的兄弟窗口。没有这一位，某个内容窗口重绘时会把它上方的边界条
    // 一起盖掉 —— 实测表现就是分隔线"悬浮一下就没了 / 时断时续"。
    viewArea_ = CreateWindowExW(
        0, kViewAreaClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
        10, 10, hwnd_, nullptr, hInst_, nullptr);
    theme_.ApplyToViewArea(viewArea_);

    // 两条内容区边缘覆盖带。复用 viewArea 的窗口类和背景刷：它们只做一件事 ——
    // 用内容底色盖住 Shell 视图在顶行/底行自己画的 0x646464 边框。
    // 上下栏仍各自画 1px theme.line，于是每侧最终只留下那条线。
    const DWORD coverStyle = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    viewTopCover_ = CreateWindowExW(0, kViewAreaClass, L"", coverStyle, 0, 0, 0, 0, viewArea_,
                                    nullptr, hInst_, nullptr);
    viewBottomCover_ = CreateWindowExW(0, kViewAreaClass, L"", coverStyle, 0, 0, 0, 0, viewArea_,
                                       nullptr, hInst_, nullptr);

    // 全局导航窗格：建在 viewArea 上、固定在左侧。它不属于任何窗格，所以双栏
    // （左右或将来上下）都共用这一棵 —— 见 src/nav_pane.h 的说明。
    // 点中节点 = 让**活动窗格**导航到那里（不是固定左栏），与命令/地址栏同源。
    {
        const int navW = Config::Get().s().navPaneWidth;

        // 【建树时就用真实高度，不要再用占位值】
        // 以前这里写死 100。那 100px 高的树会被**画出来**（NSTC 在 Create 内就
        // 完成首次布局），探针 77 逐帧实测：显示前 treeH=80、节点 3、可见行 3、
        // 这 3 行**全是空白**；300ms 后才变成 treeH=463、19 行、全部有图标。
        // 用户看到的就是"图标还没渲染完"。
        //
        // 现在按与 Layout() 完全相同的公式先算 viewH（主窗口客户区减去
        // 标签/导航/书签/状态四条栏），直接建成最终高度 —— 不存在"先矮后高"
        // 的那一帧。公式必须与 Layout() 保持一致，否则会有 1~2px 的二次 Resize。
        int viewH = 100;
        {
            RECT rc{};
            if (GetClientRect(hwnd_, &rc) && rc.bottom > 0) {
                const Settings& s = Config::Get().s();
                // 与 Layout() 共用同一个公式（Theme::ChromeHeight）：两处各算一遍
                // 是整个"启动第一帧树是空白"问题的成因，见 theme.h 的栏高令牌说明。
                // 与 Layout() 同一口径：TabBarVisible 是唯一语义来源。
                const bool showTabBar = TabBarVisible(s, tabSet_.Count());
                viewH = rc.bottom - theme_.ChromeHeight(showTabBar, s.showNav, s.showBookmark,
                                                        s.showStatus);
                if (viewH < 100) viewH = 100;
            }
        }


        // 【预取必须在建树**之前**起 —— 探针 81 实测】
        // 预取线程跑 DiscoverExplorerRoots()，它内部会解析那 18 个 CLSID
        // （AppendClsidRoot / OwnFolder 都解析过同一批 GUID）。也就是说它
        // **顺便把第三方 shell 扩展加载热了**。
        //
        // 探针 81 实测这份预热的价值：
        //   · 冷启动直接解析 17 个 CLSID = 151.7 ms
        //   · 先让工作线程预热，主线程再解析 = 64.4 ms   （省 87 ms / 58%）
        //   · 热极限（第二次解析）          = 43.7 ms
        // 代价是工作线程那 155 ms —— 但它本来就在等、而且不占 UI 线程。
        //
        // 以前这行写在 Create **之后**：那时树已经按冷状态解析完 27 个根了，
        // 预热的成果一点用不上（日志可见 [navPane 建树完成] 早于 [根清单 27 项]）。
        // 提到前面后，NSTC Initialize 与建树两段 shell 调用就能吃到预热。
        //
        // 【配置里写了根就不要预取】发现结果只会被 ApplyDiscoveredRootsIfReady
        // 丢掉（它尊重用户配置），预取就成了纯浪费的 461 ms + 18 次 DLL 绑定。
        // 但**预热**对这个场景仍然有益（建树还是要解析配置里那些根），所以这里
        // 只跳过"回填根集"，不跳过预热 —— 判据在回调里已经由
        // ApplyDiscoveredRootsIfReady 自己处理，这里传回调即可。
        if (Config::Get().s().navPaneRoots.empty()) {
            navpane::PrefetchDefaultsAsync([this] {
                if (hwnd_) PostMessageW(hwnd_, WM_APP_NAV_ROOTS_READY, 0, 0);
            });
            // explorer 里取消/新增侧边栏项改的是注册表，不会有 shell 事件告诉我。
            // 监听配置子树；一有变化就重跑发现，再走同一个"根集就绪"消息换树。
            navpane::StartRootChangeWatch([this] {
                navpane::RefreshDefaultsAsync([this] {
                    if (hwnd_) PostMessageW(hwnd_, WM_APP_NAV_ROOTS_READY, 0, 0);
                });
            });
        }

        if (navPane_.Create(viewArea_, theme_, navW, viewH)) {
            LogDbg(L"[boot] navPane 建树完成");
            navPane_.SetPickFn([this](const std::wstring& p) { NavigateActive(p, true); });
            ApplyNavPane();   // 把配置里的根集套上去
        } else {
            LogDbg(L"[navpane] 原生导航树创建失败，退回无树模式（功能不受影响）");
        }
    }

    tabStrip_.Create(hwnd_, theme_);
    navBar_.Create(hwnd_, theme_);
    bookmarkBar_.Create(hwnd_, theme_);
    statusBar_.Create(hwnd_, theme_);
    // 图标大小滑块：状态栏行右端，独立子窗口（见 status_zoom_slider.h）。
    zoomSlider_.Create(hwnd_, theme_, id::kZoomSliderClass);
    // 拖动 → 走命令总线（view.zoom.set），与 Ctrl+滚轮、视图菜单同一路径：
    // 审计、前置检查、界面落地都只有一处。
    zoomSlider_.SetOnChange([this](int iconPx) {
        json::Value a = json::Value::MakeObject();
        a.Set(L"iconPx", json::Value::MakeInt(iconPx));
        CommandBus::Get().Invoke(CmdId::ViewZoomSet, Args::FromJson(a), Actor::Ui());
        SyncZoomSlider();
    });
    LogDbg(L"[boot] 四条栏已创建");

    // 三条可拖的布局边界（窗格间 / 导航|文件 / 文件|AI 面板）：
    // 与窗格同父或与主窗口同父由 app_panes.cpp 决定 —— 它属于"布局"那一域，
    // 不是四条栏的第五条（见 CreateSplitBars 的说明）。
    CreateSplitBars();

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
                         [this](int depth, bool back) { JumpHistoryViaCommand(depth, back); });
    // 设置走独立窗口（不是弹出菜单）：锚点对独立窗口没有意义 ——
    // 它有自己的位置，而且是非模态的，改设置时主窗口还能用。
    // 三条 UI 命令都走总线（与快捷键/命令面板同源，菜单有审计、有事件）：
    // .Cmd(CmdId::X) 只是快捷键显示锚点，UI 回调必须自己 Invoke 才真正成命令。
    navBar_.SetSettingsFn([this](POINT) {
        (void)CommandBus::Get().Invoke(CmdId::Preferences, Args::Empty(), Actor::Ui());
    });
    navBar_.SetColumnsFn([this](POINT) {
        (void)CommandBus::Get().Invoke(CmdId::ViewColumnsChoose, Args::Empty(), Actor::Ui());
    });
    navBar_.SetZoomFn([this](POINT) {
        (void)CommandBus::Get().Invoke(CmdId::ViewZoomChoose, Args::Empty(), Actor::Ui());
    });
    navBar_.SetRecentFn([] { return Config::Get().s().recentPaths; });
    navBar_.SetClearHistoryFn([this] { (void)CommandBus::Get().Invoke(CmdId::NavHistoryClear, Args::Empty(), Actor::Ui()); });
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
        // 双栏三态：布局没有 shell 侧的第二份真源，配置就是唯一真相（R19）。
        paneui::FillState(&vs);
        if (ShellView* host = ActiveShellViewHost()) {
            if (IShellView* v = host->CurrentView()) {
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
            if (host->GetViewMode(&fvm, &iconPx)) {
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
    // 视图菜单里的双栏族（开关 / 方向 / 比例预设）：与键位、命令面板、
    // 分割条右键菜单走同一条命令总线 —— 前置检查、落盘、审计只有那一处。
    paneui::WireMenu(navBar_);

    bookmarkBar_.SetNavigateFn([this](const std::wstring& p) { NavigateActive(p, true); });
    // 书签落盘失败（被外部改动 / 加载失败 / 文件被占用）时统一提示，
    // 见 App::NotifyBookmarkSave。回执挂在 Store 层，覆盖所有 Save 调用点
    // （书签栏 / 拖放 / 导入 / 命令面板），不再依赖某个控件转交。
    BookmarkStore::Get().SetSaveNotifyFn([this](bool ok) { NotifyBookmarkSave(ok); });
    // 书签栏的键盘态按 Esc 退出后要把焦点还给视图，否则 Delete / type-ahead 全哑。
    bookmarkBar_.SetRefocusFn([this] { FocusActiveView(); });
    // 空状态提示的键名现查：键位用户可改，写死的 Ctrl+D / Ctrl+B 改键后就是假提示。
    bookmarkBar_.SetShortcutFn([this](CmdId id) { return input_.ShortcutText(id); });
    // 导航栏搜索框提交：用 shell 自己的 search-ms 协议在当前目录里搜。
    //
    // 为什么不自建扫描：那要么阻塞 UI（大目录几十万项），要么起线程/helper
    // （另一套索引与用户看到的视图不一致）。search-ms 是资源管理器自己走的路，
    // 结果就在同一个 IShellView 里呈现 —— 排序、右键、拖放全部照旧可用。
    //
    // 空串 = 取消搜索：回到原目录（搜索视图没有"上一级"，只能记住来源）。
    navBar_.SetSearchFn([this](const std::wstring& query) { RunSearchViaCommand(query); });
    // 搜索框的两条带：左带弹「搜索范围」、右带弹「搜索历史」。
    // 菜单内容在 App 侧组装（范围的真相在 Config::Settings 里，导航栏不持有副本），
    // 导航栏只算锚点并转过来 —— 与"设置…""选择详细信息…"同一套路。
    navBar_.SetSearchScopeFn([this](POINT at, int w) { ShowSearchScopeMenu(at, w); });
    navBar_.SetSearchHistoryFn([this](POINT at, int w) { ShowSearchHistoryMenu(at, w); });

    // 面包屑段右键：动作作用于"右键的那一段"，不是当前目录。
    // 能点名目标的动作一律转投命令（前置检查/审计/面板/AI 同源）：
    //   · 新标签 → tab.new {path}     · 新窗口 → fs.open.window {path}
    //   · 属性   → fs.properties {target}
    //   · 复制   → clipboard.copy {text}
    //   · 终端   → ext.terminal.at {path}
    // 这五项在批次 2 全部转成 CommandBus，右键菜单里不再直接碰剪贴板/进程。
    navBar_.SetSegmentCmdFn([](const std::wstring& path, NavBar::SegCmd action) {
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

    RegisterStateProviders();

    MouseSink sink = [this](int target, UINT msg, int button, int mods, POINT pt) {
        return OnChromeMouse(target, msg, button, mods, pt);
    };
    tabStrip_.SetMouseSink(sink);
    navBar_.SetMouseSink(sink);
    bookmarkBar_.SetMouseSink(sink);
    statusBar_.SetMouseSink(sink);

    statusBar_.SetClickFn([this](int seg) {
        switch (seg) {
        // AI chip 与撤销 chip：转投总线（win.ai.peek / win.undo.history）——
        // 与命令面板/快捷键/AI 同源（审计、前置检查、失败上报一处）。
        // 以前直调 ToggleAiPeek / ShowUndoHistory，这条路没有审计也没有事件。
        case Seg_Ai: CommandBus::Get().Invoke(CmdId::WinAiPeek, Args::Empty(), Actor::Ui()); break;
        case Seg_Undo: CommandBus::Get().Invoke(CmdId::WinUndoHistory, Args::Empty(), Actor::Ui()); break;
        // 双栏协同的两个 chip：与 F7/F8、命令面板同一条总线。move 的确认门
        // 在 ConfirmIfDestructive 里（人侧弹框），这里不必再问一次。
        case Seg_CopyPane: (void)CommandBus::Get().Invoke(CmdId::FsCopyToPane, Args::Empty(), Actor::Ui()); break;
        case Seg_MovePane: (void)CommandBus::Get().Invoke(CmdId::FsMoveToPane, Args::Empty(), Actor::Ui()); break;
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

    input_.Init(hwnd_, theme_, AppInputConfigInstance());
    SetupSequenceOverlay();   // 序列总览弹窗：取数/派发回调注入输入端
    RegisterCommands();

    // 破坏性命令的人工确认收口（总线层）。
    // 实现见 src/app_confirm.cpp —— 全程只有那一处弹框，所有人工入口共用。
    InstallConfirmCallback();

    Config& cfg = Config::Get();
    statusBar_.SetEnabledSegments(cfg.s().statusSegs);
    BookmarkStore::Get().Load(cfg.BookmarksPath());
    bookmarkBar_.Reload();
    LogDbg(L"[boot] 书签已加载");

    SetTimer(hwnd_, kStatsTimer, 800, nullptr);
    ApplyDarkTitleBar(hwnd_, theme_.dark);
    BuildSystemMenu();
    // 焦点观察：双栏下活动窗格跟随真实焦点（与"焦点从哪来"无关）。
    StartFocusWatch();
    LogDbg(L"[boot] OnCreate 结束");
}

int App::Run(HINSTANCE hInst, const std::wstring& initialPath,
             const std::wstring& selectTarget) {
    hInst_ = hInst;
    Config::Get().Load();
    LogDbg(L"[boot] Config 已加载");
    if (!CreateMainWindow(hInst)) return 1;
    LogDbg(L"[boot] 主窗口已创建，开始建标签");

    // 关闭历史是"可重开"的跨会话状态：以前只保存不读取，Ctrl+Shift+T 重启后永远空。
    // 【顺序】先把上限推进 TabSet 再灌数据：反过来的话，配置里 closedTabLimit=5
    // 而盘上有 20 条时，SetClosed 会先按默认 20 留下 20 条，之后收紧上限才裁 ——
    // 结果依赖两条语句的先后，而"顺序依赖"最容易在下次重构时被悄悄改掉。
    ApplyHistoryLimits();
    tabSet_.SetClosed(Config::Get().s().closedTabs);
    viewMode_ = Config::Get().s().viewMode;

    // 整个启动期都不建右窗格视图（见 app.h 的 startupPending_）：
    // 它会花 375 ms 且发生在窗口可见之前，而右栏对第一眼没有贡献。
    // 【必须在第一次 NewTab 之前置位】启动路径上有两处 Activate
    // （NewTab 内部一次 + 会话恢复末尾一次），晚一处就漏一处。
    // 置位是安全的：这段代码后面无条件 PostMessage(WM_APP_STARTUP_SETTLE)，
    // 所以标志一定会被消费掉，不会留下"永远不建右栏"的状态。
    startupPending_ = true;

    // 启动标签：显式路径 > 上次会话 > 主目录
    if (!initialPath.empty()) {
        // /select,<文件>：在导航之前挂起选中项，首次导航完成（路径匹配）时消费。
        // 与会话选中恢复共用同一套 pendingSel 机制（见 app_nav.cpp 消费点）。
        // 必须在 NewTab 之前写：NewTab 内可能同步完成导航并触发消费。
        if (!selectTarget.empty()) {
            pendingSelPath_ = initialPath;
            pendingSelItems_.assign(1, selectTarget);
        }
        NewTab(initialPath);
    } else {
        const Settings& s = Config::Get().s();
        if (!s.sessionPaths.empty()) {
            restoringSession_ = true;   // 恢复期间 NewTab 的 Activate 不建右视图
            size_t pane2HistIndex = 0;
            for (size_t i = 0; i < s.sessionPaths.size(); ++i) {
                const std::wstring& p = s.sessionPaths[i];
                if (!(tf::Exists(p) || p.rfind(L"::", 0) == 0)) continue;
                const int id = NewTab(p);
                // 每窗格历史：对齐 sessionPaths 的下标（跳过已失效路径时也不移位）。
                if (id && i < s.sessionHistory.size()) {
                    const TabHistory& h = s.sessionHistory[i];
                    if (Tab* t = ById(id); t && !h.paths.empty()) {
                        Pane* left = PaneAt(t, 0);
                        if (left) left->nav.Restore(h.paths, h.idx);
                        SyncChrome();
                    }
                }
                // 右窗格只恢复路径/历史到模型：视图留给最后 Activate 按需建，
                // 避免会话里 N 个标签在启动时各建两个 ShellView。
                if (id && i < s.sessionPane2Paths.size()) {
                    const std::wstring& p2 = s.sessionPane2Paths[i];
                    if (!p2.empty() && (tf::Exists(p2) || p2.rfind(L"::", 0) == 0)) {
                        if (Tab* t = ById(id); t) {
                            Pane* right = PaneAt(t, 1);
                            right->nav.Reset(p2);
                            if (pane2HistIndex < s.sessionPane2History.size()) {
                                const TabHistory& h2 = s.sessionPane2History[pane2HistIndex];
                                if (!h2.paths.empty()) right->nav.Restore(h2.paths, h2.idx);
                            }
                        }
                        ++pane2HistIndex;
                    }
                }
            }
            restoringSession_ = false;  // 最后一个标签的右视图在这里补建一次
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
            pendingSelPath_ = s.sessionPaths[static_cast<size_t>(s.sessionActive)];
            pendingSelItems_ = s.sessionSelection;
        }
    }

    // 【必须先 Layout 再 ShowWindow】四条栏的几何在 Layout 里才算出来，
    // 而 ShowWindow 之后的 UpdateWindow 会立刻画第一帧。旧顺序先 Show 再
    // Layout，第一帧就是错的（曾观测到导航树以 80px 高画出、3 行全空白）。
    // 窗口不可见时重绘不会闪，所以先排好版再显示。
    // 【首帧之前先把已完成的根集发现套上 —— 这是"导航图标渲染延迟"的直接修复】
    //
    // 时序（实测埋点）：OnCreate 建树时只有 3 个内置兜底根（0 ms 出图），
    // 后台发现 ~450 ms 完成；而"发现完成"是靠 WM_APP_NAV_ROOTS_READY 回投的，
    // 那条消息**只能在消息循环里被处理**，而消息循环在 ShowWindow 之后才开始。
    // 于是第一帧永远是 3 项，用户看到树"先短一截、再补满 27 项" —— 表现就是
    // "图标还没渲染完"（其实是根集整棵树被 RemoveAllRoots 重建了一遍）。
    //
    // 而建标签 + 首帧布局本身要 ~500 ms，这时发现通常**已经**完成了
    // （埋点：[+671] 根清单 27 项 / [+703] 窗口已显示）。所以在 Layout 之前
    // 主动取一次：拿到了就一次画全，拿不到就仍按 3 项走（消息稍后再补）。
    // ApplyDiscoveredRootsIfReady 本就是幂等的，这里重复调用没有副作用。
    ApplyDiscoveredRootsIfReady();

    LogDbg(L"[boot] 标签已建（%d 个），开始首帧布局", tabSet_.Count());
    Layout();
    LogDbg(L"[boot] 首帧布局完成，准备 ShowWindow");
    // 【这一行是"首帧布局是否正确"的判据，不要删】
    // 探针 77 在窗口可见后才采样，抓不到 ShowWindow 与 Layout 之间那一瞬 ——
    // 实测把顺序改回旧的（先 Show 再 Layout），探针仍报"0 空白行"，判据是空的。
    // 所以判据必须落在**代码里**：显示之前就把树的实际高度记下来。
    // 期望：这里的高度 == Layout 算出的 viewH（不再是 100/80 这类占位值）。
    if (HWND tv = navPane_.treeWindow()) {
        RECT tr{};
        GetClientRect(tv, &tr);
        LogDbg(L"[boot] ShowWindow 前 树=%ldx%ld", static_cast<long>(tr.right),
               static_cast<long>(tr.bottom));
    }
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    LogDbg(L"[boot] 窗口已显示");

    // 首帧已落地：把启动期押后的活补上（双栏的右窗格视图）。
    //
    // 【为什么用 PostMessage 而不是在这里直接调】直接把右视图建出来仍然会
    // 阻塞"用户看到窗口"这件事 —— Run 还在其中，消息循环还没开始转。
    // 投一条消息，它的处理时刻必然在首帧绘制之后；用户先看到窗口，右栏随后填上。
    // 判据用 startupPending_：它只在启动期（Run 设、这里清）为真，运行期不设，
    // 所以"用户点标签"这类路径照旧同步建右视图（那里不能被拖）。
    if (startupPending_) PostMessageW(hwnd_, WM_APP_STARTUP_SETTLE, 0, 0);
    LogDbg(L"[boot] 首帧完成，右视图延后补建=%d", startupPending_ ? 1 : 0);

    // 启动时的首次焦点在这里补。Activate 路径上原本会 SetFocus，但那时窗口还没
    // 显示 —— 给隐藏窗口设焦点会同步触发 ShellView 的 UIActivate（实测 344 ms），
    // 而用户什么都看不到。现在窗口已经画出来了，这一次焦点是"看得见"的，
    // 顺带让键盘（热键/type-ahead）从第一帧起就可用。
    FocusActiveView();
    LogDbg(L"[boot] 首次焦点已给出");

    // AI 通道：标签就绪后才启（状态快照要能取到活动标签）。
    // 默认开启；settings.json 的 aiEnabled=false 才关闭。
    // 运行中改 aiEnabled 立即生效（WM_APP_SYNC_AI -> SyncAiChannel），无需重启。
    SyncAiChannel();

    // AI 面板：把 node 服务拉起来并创建面板宿主（此时管道名与令牌已在进程内，
    // 直接交给服务，它不需要任何"发现"逻辑 —— 见 aichat_service.h）。
    // 放在 SyncAiChannel 之后：服务要连的正是刚启动的那条管道。
    InitAiPanel();

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

    // 低层键盘钩子：救「中文输入法把字母键吃成 VK_PROCESSKEY」这件事。
    //
    // 【为什么必须开】实测：中文输入法下按 g，宿主收到的是 WM_KEYDOWN(vk=0xE5)，
    // 应用根本拿不到真实 vk —— 序列键、字母热键全部按不出来，
    // 弹窗也不会出现（宿主日志里连一行都没有）。
    // 原版 exbar.cpp 用 WH_KEYBOARD_LL 解决：钩子跑在输入法之前，
    // 拿到真实 vk 并把键吞掉（输入法看不见 → 不会弹拼音候选框），再投给 UI 线程。
    //
    // 【装不上怎么办】Start 失败就退回原来的 WM_KEYDOWN 路径 ——
    // 行为与装钩子前一致（字母仍然按不出来，但别的功能不受影响）。
    kbhook::Start(GetCurrentThreadId(), hwnd_);

    // 钩子吞掉的键，若输入路由没消费，要还回焦点窗口（例：文件视图的 type-ahead）。
    // 计数表标出「这些 vk 的 WM_KEYDOWN 是我们自己塞回去的，别再走一次输入路由」。
    std::map<UINT, int> reInjected;

    MSG msg;
    int gm;
    while ((gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == kbhook::kKeyMsg) {
            // 低层钩子投递的按键事件（真实 vk + 事件流修饰键 + 自动重复标记）。
            std::unique_ptr<kbhook::KeyEvent> ev(
                reinterpret_cast<kbhook::KeyEvent*>(msg.lParam));
            if (ev) {
                HWND focus = GetFocus();
                HWND root = focus ? GetAncestor(focus, GA_ROOT) : nullptr;
                bool consumed = false;
                if (root == hwnd_) {
                    // 合成 WM_KEYDOWN 的 lParam：bit30 = 自动重复、bit0 = 按下。
                    // 输入路由靠 bit30 判「按住键不该连发序列」，这里必须带上。
                    LPARAM keyInfo = (static_cast<LPARAM>(1) << 30) * (ev->repeat ? 1 : 0) | 1;
                    consumed = input_.HandleKey(ev->vk, focus, keyInfo, ev->mods);
                }
                if (!consumed && focus) {
                    // 没被消费 → 把键还给焦点窗口。
                    // 【为什么必须还】裸字母在文件视图里本来能做 type-ahead
                    // （敲首字母跳到对应项），钩子吞掉后不还回去 =
                    // 悄悄砍掉一个 shell 特性。
                    reInjected[static_cast<UINT>(ev->vk)]++;
                    LPARAM keyInfo = (static_cast<LPARAM>(1) << 30) * (ev->repeat ? 1 : 0) | 1;
                    PostMessage(focus, WM_KEYDOWN, ev->vk, keyInfo);
                }
            }
            continue;
        }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            // 我们自己塞回去的键：已经走过一次输入路由，别再走第二次；
            // 但仍要正常派发（文件视图要靠它做 type-ahead）。
            auto it = reInjected.find(static_cast<UINT>(msg.wParam));
            if (it != reInjected.end() && it->second > 0) {
                if (--it->second == 0) reInjected.erase(it);
            } else {
                HWND focus = GetFocus();
                HWND root = focus ? GetAncestor(focus, GA_ROOT) : nullptr;
                if (root == hwnd_ && input_.HandleKey(msg.wParam, focus, msg.lParam)) continue;
            }
        } else if (msg.message == WM_CHAR) {
            // WM_CHAR：输入法提交组合后的字符。弹窗开着时归弹窗（输入框过滤）；
            // 弹窗没开 → 不消费，字符照常发给视图/编辑框。
            if (input_.HandleChar(static_cast<wchar_t>(msg.wParam))) continue;
        } else if (msg.message == WM_MOUSEWHEEL || msg.message == WM_MOUSEHWHEEL) {
            // 滚轮：光标在序列弹窗上时归弹窗（派发之前截住，否则滚的是
            // 弹窗背后的文件视图）。其余情况不消费，走各自的控件。
            if (input_.HandleWheel(msg.message, msg.wParam, msg.lParam)) continue;
            // 书签栏同理：滚轮只发给焦点窗口，光标悬在书签栏上时它收不到 ——
            // 宿主在派发之前按光标位置转交（与上面序列弹层同一手法）。
            if (bookmarkBar_.HandleWheelAt(msg.message, msg.wParam, msg.lParam)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // GetMessage 返回 -1 是错误（不能继续循环），0 是 WM_QUIT。
    if (gm == 0) {
        // 兜底停桥接：正常路径 OnDestroy 已经停过，这里再调一次是幂等的。
        // 万一有路径绕过 WM_CLOSE 直接 PostQuitMessage，这里也能收干净，
        // 不会留下一个 joinable 的静态线程在进程退出时 std::terminate()。
        kbhook::Stop();
        mcpstdio::Stop();
        return 0;
    }
    LogDbg(L"[loop] GetMessage 失败 error=%lu", GetLastError());
    kbhook::Stop();
    mcpstdio::Stop();
    return 1;
}

// ---------------------------------------------------------------- 输入

void App::MarkSessionDirty() {
    if (!hwnd_) return;
    // 去抖：每次变更都把计时重新开始，连续操作（新建一批标签）只落盘一次。
    // 间隔来自设置 sessionSaveDelayMs（默认 2000ms）—— 它决定崩溃前最多丢多久状态。
    SetTimer(hwnd_, kSessionTimer, static_cast<UINT>(Config::Get().s().sessionSaveDelayMs), nullptr);
}

void App::SaveSessionSnapshot() {
    if (!hwnd_) return;
    Config& cfg = Config::Get();
    std::vector<std::wstring> paths;
    std::vector<std::wstring> pane2Paths;
    std::vector<TabHistory> hist;
    std::vector<TabHistory> pane2Hist;
    for (const auto& ref : tabSet_.items()) {
        // 会话固定记"左/右窗格"，不记当前活动窗格：切窗格只改变显示，
        // 不该把两个窗格的目录在落盘时对调。
        Tab* tab = ById(ref.id);
        Pane* left = tab ? PaneAt(tab, 0) : nullptr;
        if (!left || left->path().empty()) continue;
        paths.push_back(left->path());
        TabHistory h;
        h.paths = left->nav.entries();
        h.idx = left->nav.idx();
        hist.push_back(std::move(h));

        Pane* right = tab ? PaneAt(tab, 1) : nullptr;
        const std::wstring p2 = right ? right->path() : std::wstring();
        pane2Paths.push_back(p2);
        if (!p2.empty()) {
            TabHistory h2;
            h2.paths = right->nav.entries();
            h2.idx = right->nav.idx();
            pane2Hist.push_back(std::move(h2));
        }
    }
    // 选中状态一并入会话：重启后活动窗格的选中项可恢复（尽力语义）。
    std::vector<std::wstring> selPaths;
    if (ShellView* host = ActiveShellViewHost()) {
        std::vector<ShellView::SelectionItem> sel;
        host->SelectionPaths(256, sel);
        for (const auto& it : sel) selPaths.push_back(it.path);
    }
    cfg.RememberSession(paths, ActiveIndex(), selPaths, hist, pane2Paths, pane2Hist);
    cfg.mutableSettings().closedTabs = tabSet_.closed();
    cfg.Save();
}

void App::OnDestroy() {
    if (inDestroy_) return;
    inDestroy_ = true;
    // AI 面板：先关 WebView2 再关服务 —— 顺序反了的话页面会在加载中途
    // 拿到连接被拒，控制台刷一片错误（不影响正确性，但日志难读）。
    // 服务的 Job Object 是最后一道保险，这里显式收一次是给"正常关闭"一个体面的顺序。
    // peek 是主窗口的 owner 弹层，先收掉再关主窗口。
    aiPeek_.Destroy();
    aiPanel_.Destroy();
    kbhook::SetTextEntryRoot(nullptr);
    input_.SetTextEntryRoot(nullptr);
    // 卸低层键盘钩子：钩子线程还活着的话，窗口销毁后它仍会往主线程投递按键事件
    // —— 那时主循环已经退出，事件会烂在队列里（并让钩子线程一直等 WM_QUIT）。
    kbhook::Stop();
    // 先卸焦点观察：窗口将销毁，回调再进来只会摸到将死的对象。
    StopFocusWatch();
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
    KillTimer(hwnd_, kViewRefreshTimer);
    // 先停 stdio 桥接：它可能正阻塞在 stdin 上，要靠 CancelSynchronousIo 唤醒。
    // 放在 ai::StopServer() 前面，让桥接线程在管道还在的时候干净地退出。
    mcpstdio::Stop();
    ai::StopServer();   // 先停监听线程，避免它持有请求排队时窗口已销毁
    ai::SetStateProvider(nullptr);
    // 长任务作业：取消全部并 join 工作线程（实现见 app_commands_jobs.cpp）。
    ShutdownJobs();
    // 根集发现线程：同样必须 join。它正在 shell 扩展里跑（几百毫秒），
    // 不 join 就在退出途中让工作线程去摸已析构的 shell/COM —— 表现为
    // "关窗口时偶发闪退"，且崩点看起来毫无关联。
    // PrefetchShutdown 自己会先停注册表监听（那个回调会再启动发现线程）。
    // 6000 ms 只是"等多久开始记一条告警"，**不是**放弃 join 的界限：
    // std::thread 无法中断，超时后仍会等它收工（见 PrefetchShutdown 的说明）。
    navpane::PrefetchShutdown(6000);
    input_.Cancel();
    tabs_.clear();  // 字体仍有效，Shell 视图可安全销毁
    // 待回收队列也必须清掉：它们同样是 ShellView，析构里有 IExplorerBrowser::Destroy。
    // 不清就是**泄漏** —— 退出前不再有消息循环，WM_APP_RECLAIM_VIEWS 永远不会到。
    // 这里同步清是可接受的：进程正在退出，没有"用户等界面反应"这回事了。
    pendingDestroy_.clear();
}

void App::SyncAiChannel() {
    const bool want = Config::Get().AiChannelEnabled();
    const bool running = ai::IsRunning();
    if (want == running) return;

    if (want) {
        ai::StartServer(nullptr);
        // 泵定时器：命令 handler 必须在 UI 线程跑，监听线程只排队。
        // 间隔来自设置 aiPumpIntervalMs（默认 100ms，人感知不到；无订阅者时不轮询，
        // 见 PumpRequests）。低延迟自动化可调小。
        SetTimer(hwnd_, kAiTimer, static_cast<UINT>(Config::Get().s().aiPumpIntervalMs), nullptr);
        // 通道令牌每次启用都会变；node 子进程必须带着新 pipe/token 重启，
        // 否则它会一直拿旧凭据连宿主，工具调用全部失败。
        const std::wstring exeDir = tf::ExeDir();
        const std::wstring script = tf::Join(exeDir, L"aichat\\service\\server.js");
        unsigned port = 0;
        std::wstring reason;
        if (aisvc::Start(script, exeDir, ai::CurrentPipeName(), ai::CurrentToken(),
                         &port, &reason)) {
            aiPanel_.SetServicePort(port);
        } else {
            LogDbg(L"[aiui] 通道启用后 node 服务启动失败：%s", reason.c_str());
        }
        LogDbg(L"[ai] 通道已启用（热插拔）");
    } else {
        // 先停定时器再停服务：反过来的话，下一次 WM_TIMER 会对已停的管道泵请求。
        KillTimer(hwnd_, kAiTimer);
        ai::StopServer();
        // 同时停 node：它持有旧 pipe/token，留着既占端口又会在重开时失配。
        aisvc::Stop();
        aiPanel_.SetServicePort(0);
        LogDbg(L"[ai] 通道已停用（热插拔）");
    }
}

void App::InitAiPanel() {
    // 面板需要 Web UI，Web UI 需要 node 服务，服务需要"连哪个窗口" ——
    // 那个信息只有本进程知道（管道名内嵌 hwnd+pid，令牌每次启动随机）。
    // 所以顺序是：确认通道在跑 → 把管道名/令牌交给服务 → 创建面板。
    //
    // 失败不是致命错误：面板会显示占位页说明原因，文件管理功能不受影响。
    const std::wstring exeDir = tf::ExeDir();
    // 用 tf::Join 而不是手工拼 "\\" —— ExeDir() 的尾部分隔符不保证存在
    // （实测它没有），手工拼会得到 "...\Tempaichat\service\server.js" 这种
    // 把目录名粘到上一级上的路径。Join 会自己处理分隔符。
    const std::wstring script = tf::Join(exeDir, L"aichat\\service\\server.js");

    // 外层声明：下面要把端口交给面板（不只是在这里打日志用）。
    unsigned port = 0;
    if (Config::Get().AiChannelEnabled() && ai::IsRunning()) {
        std::wstring reason;
        // ai::CurrentPipeName()/CurrentToken() 是**进程内**取当前实例通道的入口
        // （mcp_stdio 也用它）。外部客户端才走发现文件。
        if (aisvc::Start(script, exeDir, ai::CurrentPipeName(), ai::CurrentToken(),
                         &port, &reason)) {
            LogDbg(L"[aiui] 服务已启动 port=%u", port);
            // 端口立刻交给面板，而不是等 WebView2 创建流程里再赋值。
            // 理由：ai.panel.web（浏览器打开）正是"WebView2 起不来"时的退路，
            // 而那条路的端口必须在本函数里就已知 —— 否则越是 WebView2 失败、
            // 越拿不到端口，退路也用不了（实测就是这个死结）。
            aiPanel_.SetServicePort(port);
        } else {
            LogDbg(L"[aiui] 服务启动失败：%s", reason.c_str());
        }
    } else {
        LogDbg(L"[aiui] AI 通道未开，跳过服务启动");
    }

    // 面板父窗口是 viewArea_，不是主窗口 hwnd_：它与导航树、文件视图、三条
    // 边界条处于同一条兄弟裁剪链，Line 才能压在原生控件边框之上（见 app_panes.cpp）。
    if (!aiPanel_.Create(viewArea_, exeDir, &theme_)) {
        LogDbg(L"[aiui] 面板创建失败：%s", aiPanel_.lastError().c_str());
        return;
    }
    // 状态栏 peek：owner 是主窗口，theme 借用 App 的成员（销毁早于 Theme）。
    aiPeek_.Create(hwnd_, &theme_);
    aiPeek_.SetOpenFn([this]() {
        ConsumeAiUnread();
        HideAiPeek();
        if (!aiPanel_.visible())
            (void)CommandBus::Get().Invoke(CmdId::AiPanelToggle, Args::Empty(), Actor::Ui());
    });
    aiPeek_.SetDismissFn([this]() { ConsumeAiUnread(); });
    aiPeek_.SetUndoFn([this](const std::wstring& domain) {
        const CmdId undoId = domain == L"bookmark" ? CmdId::BmkUndo : CmdId::CmdUndo;
        // 由人从 peek 点下，但撤的是机器那一步：保持 Actor::Ai，才能命中
        // "撤销不穿透人工操作"的边界判定。
        CommandBus::Get().Invoke(undoId, Args::Empty(), Actor::Ai());
        ApplyAiWebStatus(aiStatusState_, aiStatusSummary_, aiStatusDetail_,
                         false, L"", aiStatusTurnId_);
        // 重放同一条状态会再次点亮未读；撤销动作本身就是消费。
        ConsumeAiUnread();
        HideAiPeek();
    });
    // AI 页里的自定义标题栏按钮走 WebMessage 到这里。窗口动作一律转投命令
    // 总线（ai.panel.toggle / win.minimize / win.maximize / win.restore /
    // win.topmost），与系统菜单、快捷键同源：以前这里直接调 ShowWindow /
    // SetWindowPos，那一路既没有审计也没有事件。
    // 所有调用都带 NOACTIVATE / 不动光标，符合 R21 输入纪律。
    aiPanel_.SetWindowActionCallback(
        [this](const std::wstring& action) { DispatchAiPanelWindowAction(action); });
    // 页面状态不依赖面板可见：隐藏 WebView2 后 SSE 仍会跑完，done/error
    // 会经这里落到状态栏 chip + peek（ROADMAP 的"收起不是消失"）。
    aiPanel_.SetStatusCallback([this](const aiui::AiWebStatus& st) {
        ApplyAiWebStatus(st.state, st.summary, st.detail, st.canUndo, st.undoDomain, st.turnId);
    });
    // 页面尚未发第一条 status 前，chip 显示宿主自己能确认的启动进度。
    // 展开、服务重启、WebView2 创建失败/重试都会回调这里；页面状态到达后
    // ApplyAiWebStatus 会把 aiPageStatusSeen_ 置真，原生态不再回写。
    aiPanel_.SetNativeActivityCallback([this]() { RefreshAiNativeStatus(); });
    RefreshAiNativeStatus();
    // WebView2 的输入框不是原生 EDIT：钩子/序列路由必须把它的整棵子窗口树
    // 当成文本输入区，否则在聊天框里打 g / x / y 会被序列键弹窗截走。
    kbhook::SetTextEntryRoot(aiPanel_.hwnd());
    input_.SetTextEntryRoot(aiPanel_.hwnd());
    // 【把端口立刻告诉面板，而不是等 EnsureWebView】
    // EnsureWebView 只在面板展开、要建 WebView2 时才跑。若用户还没展开，
    // 或 WebView2 建失败后不再重试，servicePort_ 会一直是 0 —— 而这时服务
    // 其实已经在跑、端口有效。少这一句的后果是 exbar/state 里 aiPanelPort=0，
    // 连"用浏览器直接访问"这条退路也一起藏掉了。
    aiPanel_.SetServicePort(port);
}

void App::ToggleAiPanel() {
    // 面板一旦展开，peek 的任务就结束了：结果已经在用户面前，别再叠一层气泡。
    const bool opening = !aiPanel_.visible();
    HideAiPeek();
    if (opening) ConsumeAiUnread();
    aiPanel_.Toggle();
    Layout();   // 宽度变了，文件区可用宽度随之改变
}

void App::FocusActiveView() {
    // 活动窗格的容器窗口本身就是 tab view（ShellView::hwnd）：焦点给它，
    // shell 内部视图会接过键盘（type-ahead、方向键、Delete 都由 shell 处理）。
    HWND target = ActiveShellViewHost() ? ActiveShellViewHost()->hwnd() : nullptr;
    if (!target) {
        if (viewArea_) SetFocus(viewArea_);
        return;
    }
    SetFocus(target);
}

void App::OpenExternal(const std::wstring& path, const std::wstring& selectTarget) {
    if (path.empty()) return;
    if (!selectTarget.empty()) {
        pendingSelPath_ = path;
        pendingSelItems_.assign(1, selectTarget);
    }
    NewTab(path);
}
