// 主窗口过程（App 的编排分片）：消息分发、定时器、DPI 变化。
//
// 这里只路由到成员函数，不放业务：每个 case 最多一行调用。
#include "app.h"

#include "ai_server.h"
#include "modal.h"

// ---------------------------------------------------------------- 输入
// chrome（导航栏/标签栏/书签栏等自绘栏）的鼠标事件 → 路由给 InputRouter。
//
// 【为什么在这里】这是窗口过程收到外壳栏鼠标消息后的**唯一**去处：
// 消息在 app_wnd.cpp 的 WndProc 里被截获并转发到这里，再由 InputRouter
// 按「命中哪个栏/哪个控件」决定消费还是放行。放在 app.cpp 会让
// 「窗口过程」与「输入路由」两套语义混在一个文件里。
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

// ---------------------------------------------------------------- 消息
LRESULT CALLBACK App::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App& self = Get();
    switch (m) {
    case WM_CREATE:
        self.hwnd_ = h;
        self.hInst_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(h, GWLP_HINSTANCE));
        self.OnCreate();
        return 0;

    case WM_SIZE:
        self.Layout();
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        mmi->ptMinTrackSize.x = self.theme_.S(560);
        mmi->ptMinTrackSize.y = self.theme_.S(360);
        // 不设上限时，最大化后拖边可以把窗口拉到远超工作区，布局溢出（栏高固定、
        // 视图被挤成 0 高）。按虚拟屏幕给一个上限。
        mmi->ptMaxTrackSize.x = GetSystemMetrics(SM_CXVIRTUALSCREEN) * 2;
        mmi->ptMaxTrackSize.y = GetSystemMetrics(SM_CYVIRTUALSCREEN) * 2;
        return 0;
    }

    case WM_TIMER:
        if (w == kStatsTimer)
            self.RefreshStats();
        else if (w == kAiTimer) {
            // 模态框开着的时候**不派发** AI 命令。
            //
            // 【为什么】模态框自己的消息循环里照样收得到 WM_TIMER，
            // 于是命令 handler 会**重入**地跑在模态框的循环里：
            // 可能再开一个框（框套框），也可能改掉那个框正在等的选中项。
            // 请求留在队列里，等模态框关掉再处理；客户端那边照旧按
            // 10 秒时限拿超时（语义不变，只是不再有重入）。
            if (modal::Depth() == 0) ai::PumpRequests();
        }
        else if (w == kSessionTimer) {
            KillTimer(h, kSessionTimer);
            self.SaveSessionSnapshot();   // 崩溃前的最后一份可恢复会话
        } else if (w == kViewRefreshTimer) {
            KillTimer(h, kViewRefreshTimer);
            self.RefreshActive();         // 合并后的唯一一次视图刷新（M7）
        } else
            self.input_.OnTimer(w);
        return 0;

    // AI 通道热插拔：命令 handler 已把设置写回，这里再做真正的启停。
    // 单独的（非嵌套的）消息保证 StopServer 不会在响应写回之前 join 监听线程。
    case WM_APP_SYNC_AI:
        self.SyncAiChannel();
        return 0;

    // 导航根集发现完成（工作线程算完 -> 回投）。只在这里换根集，
    // 工作线程绝不碰 UI。见 nav_pane_root.h 的 Prefetch* 与 app_panes.cpp 的实现。
    case WM_APP_NAV_ROOTS_READY:
        self.ApplyDiscoveredRootsIfReady();
        return 0;

    // 文件视图自动刷新（M7）：shell 变更由 NavPane 回投到这里，
    // 用合并定时器去抖后刷一次活动视图。见 nav_pane_shell.cpp 的 kViewShellEvents。
    case WM_APP_VIEW_AUTO_REFRESH:
        self.ScheduleViewAutoRefresh();
        return 0;

    // 关标签时摘下来的视图，现在真正销毁（探针 84：每个 20~69 ms）。
    // 放在消息循环里做，就不会挡在"用户点 X"的那一帧上。
    // 只清一次队列；清理过程中若又有人关了标签，会再投一条消息，下轮再清。
    case WM_APP_RECLAIM_VIEWS:
        self.ReclaimPendingViews();
        return 0;

    // 首帧已画出来：把启动期押后的活补上（双栏的右窗格视图）。
    // 见 app.h 的 startupPending_ 与 App::Run 里 PostMessage 处的说明。
    case WM_APP_STARTUP_SETTLE: {
        if (!self.startupPending_) return 0;
        self.startupPending_ = false;
        // 【必须判 dualPane —— 否则单栏启动白建一个右视图】
        // 实测：dualPane=false 时这里仍然调 EnsurePaneView(t, 1)，
        // 日志 [+1328] 第二个 Initialize → [+1781] "右视图已补建"，
        // 整整 453 ms 建了一个随后被隐藏、用户永远看不到的 ShellView。
        // 判据与 app_tabs.cpp 的 Activate 保持同一条：双栏才需要右视图。
        if (!Config::Get().s().dualPane) return 0;
        if (Tab* t = self.Active()) {
            if (!self.EnsurePaneView(t, 1)) return 0;
            // 右视图建出来了：显示它，并按最终几何排一次，让它真正出现在右半区。
            //
            // 【两件事缺一不可 —— 实测踩过：启动即双栏时右半区整块空白】
            //   1) 显隐：窗格显隐由 SetDualPane / NextPane 负责（见 ApplyPaneChrome
            //      的说明），启动这条路没走过 SetDualPane —— 不显式显示，右窗格
            //      就是一个隐藏的空壳（实测 vis=0）。
            //   2) 布局：Layout() 带指纹（输入没变就整段跳过），而右视图是**启动
            //      流程里最后**才建的 —— 那一刻指纹早已记下，这次 Layout 会被跳过，
            //      右窗格停在 8x8 的初始尺寸。所以这里必须强制重排。
            if (Pane* p1 = self.PaneAt(t, 1)) {
                if (p1->view) p1->view->SetVisible(true);
            }
            self.ApplyPaneChrome();
            self.Layout(/*force=*/true);
            self.SyncChrome();
            LogDbg(L"[boot] 右视图已补建（首帧之后）");
        }
        return 0;
    }

    case WM_APP_TAB_SELECT: {
        // 点击标签 = tab.activate：与快捷键/AI 同源（前置检查、审计、断言一处）。
        // 以前直调 Activate —— 点标签那条路没有审计也没有事件。
        // 注意命令语义是 0 起下标、只切换不关闭，与点击完全等价（TabStrip 只在
        // 真正的单击选中时发这条消息，快速关闭走 WM_APP_TAB_CLOSE）。
        json::Value a = json::Value::MakeObject();
        a.Set(L"index", json::Value::MakeInt(static_cast<int>(w)));
        (void)CommandBus::Get().Invoke(CmdId::TabActivate, Args::FromJson(a), Actor::Ui());
        return 0;
    }
    case WM_APP_TAB_CLOSE:
        self.CloseTab(self.tabSet_.IdAt(static_cast<int>(w)));
        return 0;
    case WM_APP_TAB_NEW:
        // "+" 按钮固定打开此电脑（namespace 根），与 tab.new 无参语义一致：
        // 用户点 + 的预期是"开一个全新的起点"，而不是复制当前目录（那是"复制标签"）。
        // 需要复制当前目录时走 kTabMenuClone / tab.clone，两条路径各有明确用途。
        self.NewTab(nav::kThisPc, self.NewTabIndex());
        return 0;
    case WM_APP_TAB_MOVED:
        self.MoveTab(self.tabSet_.IdAt(static_cast<int>(w)), static_cast<int>(l));
        return 0;
    case WM_APP_TAB_MENU:
        self.ShowTabMenu(static_cast<int>(w), static_cast<int>(l));
        return 0;
    case WM_APP_STATUS_MENU:
        self.ShowStatusMenu(*reinterpret_cast<POINT*>(l));
        return 0;
    case WM_APP_REFOCUS:
        self.FocusActiveView();
        return 0;
    case WM_APP_FOCUS_CHANGED:
        // 焦点换了视图（键盘/程序/鼠标/shell 内部迁移都算）：按真实焦点同步外壳。
        self.OnFocusMaybeChanged();
        return 0;

    case WM_COPYDATA: {
        auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (!cd || cd->dwData != kCopyDataMagic || !cd->lpData) return FALSE;
        // 按"结构头 + 实际长度"读取，而不是要求 cbData == sizeof(CopyDataPayload)：
        // 以前任何字段/长度变化都会让新旧实例互相静默丢弃转发（点了打开没反应）。
        constexpr size_t kPathOff = offsetof(CopyDataPayload, path);
        if (cd->cbData < kPathOff + sizeof(wchar_t)) return FALSE;
        auto* p = reinterpret_cast<const CopyDataPayload*>(cd->lpData);
        if (p->magic != kCopyDataMagic) return FALSE;
        const size_t maxChars = (cd->cbData - kPathOff) / sizeof(wchar_t);
        size_t n = 0;
        while (n < maxChars && p->path[n]) ++n;  // 不信任对方一定 NUL 结尾
        if (!n) return FALSE;
        std::wstring path(p->path, n);

        // select 是变长尾随数据，紧跟 path 的 NUL 之后（见 common.h 契约注释）。
        // 老实例发来的消息只到 path 的 NUL，这里剩余长度为 0 → 不读，兼容旧行为。
        std::wstring selectTarget;
        if (n + 1 < maxChars) {
            const wchar_t* sel = p->path + n + 1;
            const size_t selMax = maxChars - (n + 1);
            size_t m = 0;
            while (m < selMax && sel[m]) ++m;
            if (m) selectTarget.assign(sel, m);
        }

        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        self.OpenExternal(path, selectTarget);
        return TRUE;
    }

    case WM_SETTINGCHANGE: {
        // 系统切深浅色/高对比度会先后发 SETTINGCHANGE + THEMECHANGED。以前两个都
        // 无条件 ApplyTheme，一次切换要重建两遍 brush/font（可见闪烁）。
        // 现在只在"深浅色真的变了"时重建，另一个消息直接忽略。
        const bool colorMsg =
            !l || wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet") == 0 ||
            wcscmp(reinterpret_cast<const wchar_t*>(l), L"HighContrast") == 0 ||
            wcscmp(reinterpret_cast<const wchar_t*>(l), L"WindowsThemeElement") == 0;
        // 判据是**当前意图**（配置档位 + 系统），不是裸 SystemIsDark()：
        // 强制浅色时用户的系统切到深色，不该把本窗口一起换成深色。
        if (colorMsg && ThemePrefersDark() != self.theme_.dark) self.ApplyTheme();
        return DefWindowProcW(h, m, w, l);
    }

    case WM_THEMECHANGED:
        if (ThemePrefersDark() != self.theme_.dark) self.ApplyTheme();
        return DefWindowProcW(h, m, w, l);

    case WM_MOVE:
        // WebView2 缓存父窗口绝对位置；不通知会让 tooltip/无障碍坐标跟主窗口错位。
        self.aiPanel().NotifyParentMoved();
        return DefWindowProcW(h, m, w, l);

    case WM_DPICHANGED: {
        // 必须用 wParam 里的新 DPI：处理 WM_DPICHANGED 期间 GetDpiForWindow 尚未更新，
        // 靠它取会拿到旧值 → 字体/栏高/最小尺寸全部按旧 DPI 缩放（跨屏拖动后错位）。
        const int newDpi = static_cast<int>(LOWORD(w));
        self.ApplyTheme(newDpi);
        self.aiPanel().NotifyParentMoved();
        RECT* r = reinterpret_cast<RECT*>(l);
        if (r)
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_DPICHANGED_AFTERPARENT:
        // 子窗口在父窗口跨 DPI 拖动后收到的是这个；不处理会一直用旧字体尺寸。
        if (int d = DpiOf(h); d > 0 && d != self.theme_.dpi) self.ApplyTheme(d);
        return 0;

    case WM_SYSCOMMAND: {
        // 标题栏系统菜单里的应用入口：转投命令总线，与导航栏按钮/快捷键/命令面板
        // 走同一条路径（前置检查、审计、写状态只在一个入口）。
        // 注意 SC_* 用整个 wParam（含高位），自定义项只需 LOWORD —— 先判自定义段，
        // 其余（还原/移动/大小/最小化/最大化/关闭）一律交系统默认。
        const UINT id = LOWORD(w);
        switch (id) {
        case kSysViewMenu:
            CommandBus::Get().Invoke(CmdId::NavViewMenu, Args::Empty(), Actor::SysMenu());
            return 0;
        case kSysPalette:
            CommandBus::Get().Invoke(CmdId::CommandPalette, Args::Empty(), Actor::SysMenu());
            return 0;
        case kSysPrefs:
            CommandBus::Get().Invoke(CmdId::Preferences, Args::Empty(), Actor::SysMenu());
            return 0;
        case kSysHotkeys:
            CommandBus::Get().Invoke(CmdId::HotkeyManager, Args::Empty(), Actor::SysMenu());
            return 0;
        case kSysQuit:
            CommandBus::Get().Invoke(CmdId::QuitExplorer, Args::Empty(), Actor::SysMenu());
            return 0;
        default:
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }

    case WM_CLOSE:
        if (!self.ConfirmExit()) return 0;
        self.OnDestroy();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        self.tabStrip_.ReleaseIcons();
        self.theme_.Shutdown();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
