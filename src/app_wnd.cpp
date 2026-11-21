// 主窗口过程（App 的编排分片）：消息分发、定时器、DPI 变化。
//
// 这里只路由到成员函数，不放业务：每个 case 最多一行调用。
#include "app.h"

#include "ai_server.h"

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
        else if (w == kAiTimer)
            ai::PumpRequests();   // 命令 handler 只能在 UI 线程跑
        else
            self.input_.OnTimer(w);
        return 0;

    // AI 通道热插拔：命令 handler 已把设置写回，这里再做真正的启停。
    // 单独的（非嵌套的）消息保证 StopServer 不会在响应写回之前 join 监听线程。
    case WM_APP_SYNC_AI:
        self.SyncAiChannel();
        return 0;

    case WM_APP_TAB_SELECT:
        self.Activate(self.tabSet_.IdAt(static_cast<int>(w)));
        return 0;
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
    case WM_APP_TAB_COLOR:
        self.ShowTabColor(static_cast<int>(w), static_cast<int>(l));
        return 0;
    case WM_APP_STATUS_MENU:
        self.ShowStatusMenu(*reinterpret_cast<POINT*>(l));
        return 0;
    case WM_APP_REFOCUS:
        self.FocusActiveView();
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
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        self.OpenExternal(path);
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
        if (colorMsg && SystemIsDark() != self.theme_.dark) self.ApplyTheme();
        return DefWindowProcW(h, m, w, l);
    }

    case WM_THEMECHANGED:
        if (SystemIsDark() != self.theme_.dark) self.ApplyTheme();
        return DefWindowProcW(h, m, w, l);

    case WM_DPICHANGED: {
        // 必须用 wParam 里的新 DPI：处理 WM_DPICHANGED 期间 GetDpiForWindow 尚未更新，
        // 靠它取会拿到旧值 → 字体/栏高/最小尺寸全部按旧 DPI 缩放（跨屏拖动后错位）。
        const int newDpi = static_cast<int>(LOWORD(w));
        self.ApplyTheme(newDpi);
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
            CommandBus::Get().Invoke(CmdId::NavViewMenu, Args::Empty(), L"sysmenu");
            return 0;
        case kSysPalette:
            CommandBus::Get().Invoke(CmdId::CommandPalette, Args::Empty(), L"sysmenu");
            return 0;
        case kSysPrefs:
            CommandBus::Get().Invoke(CmdId::Preferences, Args::Empty(), L"sysmenu");
            return 0;
        case kSysHotkeys:
            CommandBus::Get().Invoke(CmdId::HotkeyManager, Args::Empty(), L"sysmenu");
            return 0;
        case kSysQuit:
            CommandBus::Get().Invoke(CmdId::QuitExplorer, Args::Empty(), L"sysmenu");
            return 0;
        default:
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }

    case WM_CLOSE:
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
