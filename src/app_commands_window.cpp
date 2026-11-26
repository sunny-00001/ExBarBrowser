// 窗口管理命令：主窗口几何 / 层叠状态 / 多窗口布局。
//
// 【为什么单独成文件】这些命令的作用域是"顶层窗口"，既不是标签/视图（app_commands_tabs），
// 也不是进程外程序（external），更不是磁盘操作。混进 misc 会让"改窗口"与"改配置"
// 挤在一处，读的人看不出边界。
//
// 【R21 输入纪律】这里用 ShowWindow / SetWindowPos / MoveWindow / SetForegroundWindow
// 都是**产品代码里由命令触发的窗口 API**，不是自动化脚本模拟输入。AGENTS.md:64-66
// 明确豁免 src/ 里的这类调用：判据是触发源（人/命令/AI）而非 API 名字。
// 唯一例外 win.activate 抢前台焦点 —— 它 aiExposed=false，只有人能触发（快捷键/面板）。
// 并排/级联一律带 SWP_NOACTIVATE，布局时不把别的窗口抢到前台。
#include "app.h"

#include <algorithm>
#include <vector>

#include "app_commands.h"
#include "identity.h"   // kMainClass：窗口类名只允许从这里取（check_components 唯一性规则）

namespace {

// 一台显示器的工作区矩形。多窗口布局都摆在"当前主窗口所在显示器"上。
RECT WorkAreaFor(HWND anchor) {
    HMONITOR mon = MonitorFromWindow(anchor ? anchor : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    // 兜底：整屏减去任务栏的粗略估计（异常显示器才走到这里）。
    RECT r{};
    GetWindowRect(GetDesktopWindow(), &r);
    return r;
}

struct WinInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
    RECT rect{};
    bool maximized = false;
    bool minimized = false;
    bool visible = false;
};

// 枚举所有本程序顶层窗口（跨进程，只看类名，不看 PID —— 多实例是常态）。
// 与 ai_server.cpp 的 OwnMainWindow 不同：这里要**全部**，不是一个。
BOOL CALLBACK CollectWindows(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[64] = L"";
    if (!GetClassNameW(h, cls, _countof(cls)) || wcscmp(cls, id::kMainClass) != 0) return TRUE;
    auto* out = reinterpret_cast<std::vector<WinInfo>*>(lp);
    WinInfo info;
    info.hwnd = h;
    GetWindowThreadProcessId(h, &info.pid);
    wchar_t title[512] = L"";
    const int n = GetWindowTextW(h, title, _countof(title));
    info.title.assign(title, n > 0 ? static_cast<size_t>(n) : 0);
    GetWindowRect(h, &info.rect);
    info.maximized = IsZoomed(h) != FALSE;
    info.minimized = IsIconic(h) != FALSE;
    info.visible = true;
    out->push_back(std::move(info));
    return TRUE;
}

std::vector<WinInfo> AllWindows() {
    std::vector<WinInfo> out;
    EnumWindows(CollectWindows, reinterpret_cast<LPARAM>(&out));
    // 用句柄作稳定次序，避免不同进程枚举顺序抖动让 win.list 结果乱跳。
    std::sort(out.begin(), out.end(), [](const WinInfo& a, const WinInfo& b) {
        return reinterpret_cast<uintptr_t>(a.hwnd) < reinterpret_cast<uintptr_t>(b.hwnd);
    });
    return out;
}

// 只对"可正常摆位"的窗口生效：最大化/最小化的窗口先还原，再摆位。
// 摆位统一 NOACTIVATE，布局不该改变谁在前台。
static constexpr UINT kPlaceFlags = SWP_NOZORDER | SWP_NOACTIVATE;

}  // namespace

void App::RegisterWindowCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;

    reg(CmdId::WinMinimize, [this](const Args&) {
        ShowWindow(hwnd_, SW_MINIMIZE);
        return true;
    });
    reg(CmdId::WinMaximize, [this](const Args&) {
        ShowWindow(hwnd_, SW_MAXIMIZE);
        return true;
    });
    reg(CmdId::WinRestore, [this](const Args&) {
        ShowWindow(hwnd_, SW_RESTORE);
        return true;
    });
    reg(CmdId::WinTopmost, [this](const Args& args) {
        // 省略 on = 切换：按窗口自身 ex-style 判断当前是否置顶再取反。
        const bool current = (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const bool on = args.Has(L"on") ? args.Bool(L"on", true) : !current;
        SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return true;
    });
    // 激活 = 抢前台焦点：AI 不该替人决定焦点归属，aiExposed=false（表已声明）。
    reg(CmdId::WinActivate, [this](const Args&) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        return true;
    });
    reg(CmdId::WinCenter, [this](const Args&) {
        const RECT work = WorkAreaFor(hwnd_);
        RECT r{};
        GetWindowRect(hwnd_, &r);
        const int w = r.right - r.left, h = r.bottom - r.top;
        const int x = work.left + (work.right - work.left - w) / 2;
        const int y = work.top + (work.bottom - work.top - h) / 2;
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | kPlaceFlags);
        return true;
    });
    reg(CmdId::WinMove, [this](const Args& args) {
        const int x = args.Int(L"x", 0), y = args.Int(L"y", 0);
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | kPlaceFlags);
        return true;
    });
    reg(CmdId::WinResize, [this](const Args& args) {
        // 与 WM_GETMINMAXINFO 的轨道粗对齐：太小会遮不住内容，太大超出工作区无意义。
        const RECT work = WorkAreaFor(hwnd_);
        const int maxW = work.right - work.left, maxH = work.bottom - work.top;
        int w = args.Int(L"width", 0), h = args.Int(L"height", 0);
        w = std::clamp(w, 320, maxW > 320 ? maxW : 320);
        h = std::clamp(h, 240, maxH > 240 ? maxH : 240);
        SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | kPlaceFlags);
        return true;
    });
    regR(CmdId::WinList, [](const Args&, json::Value* out) {
        if (!out) return true;
        json::Value arr = json::Value::MakeArray();
        for (const auto& w : AllWindows()) {
            json::Value v = json::Value::MakeObject();
            wchar_t hwndBuf[32] = L"";
            swprintf(hwndBuf, _countof(hwndBuf), L"0x%llX",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(w.hwnd)));
            v.Set(L"hwnd", json::Value::MakeStr(hwndBuf));
            v.Set(L"pid", json::Value::MakeInt(static_cast<int>(w.pid)));
            v.Set(L"title", json::Value::MakeStr(w.title.c_str()));
            v.Set(L"x", json::Value::MakeInt(w.rect.left));
            v.Set(L"y", json::Value::MakeInt(w.rect.top));
            v.Set(L"width", json::Value::MakeInt(w.rect.right - w.rect.left));
            v.Set(L"height", json::Value::MakeInt(w.rect.bottom - w.rect.top));
            v.Set(L"maximized", json::Value::MakeBool(w.maximized));
            v.Set(L"minimized", json::Value::MakeBool(w.minimized));
            v.Set(L"visible", json::Value::MakeBool(w.visible));
            arr.arr.push_back(std::move(v));
        }
        json::Value root = json::Value::MakeObject();
        root.Set(L"windows", std::move(arr));
        *out = std::move(root);
        return true;
    });
    regR(CmdId::WinTile, [this](const Args& args, json::Value* out) {
        const std::wstring orientation = args.Str(L"orientation");
        const bool vertical = orientation == L"vertical";
        auto wins = AllWindows();
        if (wins.empty()) return false;
        const RECT work = WorkAreaFor(hwnd_);
        const int n = static_cast<int>(wins.size());
        const int cols = vertical ? 1 : n;   // horizontal=横排（每行 n 个），vertical=纵排（每列 1 个）
        const int rows = vertical ? n : 1;
        const int cellW = (work.right - work.left) / cols;
        const int cellH = (work.bottom - work.top) / rows;
        for (int i = 0; i < n; ++i) {
            HWND h = wins[static_cast<size_t>(i)].hwnd;
            if (IsZoomed(h) || IsIconic(h)) ShowWindow(h, SW_RESTORE);
            const int x = work.left + (vertical ? 0 : i * cellW);
            const int y = work.top + (vertical ? i * cellH : 0);
            SetWindowPos(h, nullptr, x, y, cellW, cellH, kPlaceFlags);
        }
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"count", json::Value::MakeInt(n));
            *out = std::move(root);
        }
        return true;
    });
    regR(CmdId::WinCascade, [this](const Args&, json::Value* out) {
        auto wins = AllWindows();
        if (wins.empty()) return false;
        const RECT work = WorkAreaFor(hwnd_);
        constexpr int kStep = 30;  // 每层右上错开 30px
        const int w = (work.right - work.left) * 3 / 5;
        const int h = (work.bottom - work.top) * 3 / 5;
        int i = 0;
        for (const auto& info : wins) {
            HWND hWnd = info.hwnd;
            if (IsZoomed(hWnd) || IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
            const int x = work.left + kStep * i;
            const int y = work.top + kStep * i;
            SetWindowPos(hWnd, nullptr, x, y, w, h, kPlaceFlags);
            ++i;
        }
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"count", json::Value::MakeInt(static_cast<int>(wins.size())));
            *out = std::move(root);
        }
        return true;
    });
}


void App::DispatchAiPanelWindowAction(const std::wstring& action) {
    // AI 页自绘标题栏的窗口动作 → 命令总线。
    // 以前这一路直接调 ShowWindow / SetWindowPos：没有审计、没有事件，
    // 也不经过命令层的前置检查 —— 与系统菜单、快捷键那条路强度不同。
    if (action == L"close") {
        // 收起面板而不是退出程序 —— 与原来的 ToggleAiPanel 同一语义。
        if (aiPanel_.visible())
            (void)CommandBus::Get().Invoke(CmdId::AiPanelToggle, Args::Empty(), Actor::Ui());
        return;
    }
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    if (action == L"minimize") {
        (void)CommandBus::Get().Invoke(CmdId::WinMinimize, Args::Empty(), Actor::Ui());
    } else if (action == L"maximize") {
        (void)CommandBus::Get().Invoke(IsZoomed(hwnd_) ? CmdId::WinRestore : CmdId::WinMaximize,
                                       Args::Empty(), Actor::Ui());
    } else if (action == L"pin" || action == L"unpin") {
        json::Value a = json::Value::MakeObject();
        a.Set(L"on", json::Value::MakeBool(action == L"pin"));
        (void)CommandBus::Get().Invoke(CmdId::WinTopmost, Args::FromJson(a), Actor::Ui());
    } else {
        LogDbg(L"[aiui] 忽略未知页面窗口动作：%s", action.c_str());
    }
}
