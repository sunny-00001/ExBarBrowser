// 应用编排：窗口、布局、标签生命周期、命令注册、消息路由。
//
// 状态归属：
//   · 标签顺序/锁定/MRU/关闭历史 -> core::TabSet（唯一）
//   · 每个标签的 Shell 视图     -> App::tabs_（id 映射）
//   · 标签图标（HICON）         -> TabStrip（唯一所有者，App 不碰）
//   · 视觉令牌                 -> Theme（唯一）
//   · 键位/鼠标规则            -> InputRouter（唯一）
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bookmark_bar.h"
#include "commands.h"
#include "common.h"
#include "config.h"
#include "input_router.h"
#include "nav_bar.h"
#include "nav_history.h"
#include "shell_view.h"
#include "status_bar.h"
#include "tab_set.h"
#include "tab_strip.h"
#include "theme.h"
#include "view_stats.h"

class App {
public:
    static App& Get();
    int Run(HINSTANCE hInst, const std::wstring& initialPath);

    HWND hwnd() const { return hwnd_; }
    void OpenExternal(const std::wstring& path);
    // 把键盘焦点还给当前标签的视图片。热键总线上所有"离开编辑态"的路径都收口到这里，
    // 否则焦点停在 OWNER 主窗口上：type-ahead / Delete / Enter 全部失效（只剩热键还能用）。
    void FocusActiveView();

private:
    App() = default;

    struct Tab {
        int id = 0;
        std::unique_ptr<ShellView> view;
        // 历史栈（含 pending 事务状态）：唯一实现在 nav::History。
        // 以前这里是 hist/idx/pending/pendingPushed/pendingPrevIdx 五个字段、
        // 由四个函数各自手工维护 —— 那正是"后退要按两次"这类 bug 的温床。
        nav::History nav;
        unsigned items = 0;
        std::wstring display;
        SplitCache split;  // 文件夹/文件分类缓存（状态栏 800ms 取数不再重算）
        // 当前目录。历史栈是唯一真相，这里只是便捷读取。
        std::wstring path() const { return nav.here(); }
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool CreateMainWindow(HINSTANCE hInst);
    void OnCreate();
    void OnDestroy();
    void RegisterCommands();
    // 命令注册按域分片（app_commands*.cpp），RegisterCommands 只做调度。
    void RegisterTabCommands();
    void RegisterFileCommands();
    void RegisterMiscCommands();
    void Layout();
    // dpiOverride > 0 时用它（WM_DPICHANGED 的 wParam 才是权威值）
    void ApplyTheme(int dpiOverride = 0);
    // 用户主目录。做成成员是为了让分片文件里的 HomePath() 不用再共享一个匿名命名空间。
    static std::wstring HomePath();
    // 状态栏取数的节流定时器 id（OnCreate 起、WndProc 收、OnDestroy 停）。
    static constexpr UINT_PTR kStatsTimer = 1;
    // AI 通道的请求泵定时器：监听线程排队 → 本定时器在 UI 线程执行命令。
    static constexpr UINT_PTR kAiTimer = 2;

    // 标签
    int NewTab(const std::wstring& path, int index = -1);
    int NewTabIndex() const;  // tabNewPos: right = 当前标签右侧, end = 末尾
    void CloseTab(int id);
    void CloseOthers(int keepId);
    void CloseSide(int id, bool left);
    void Activate(int id);
    void MoveTab(int id, int toIndex);
    void ToggleLock(int id);
    Tab* Active();
    Tab* ById(int id);
    int ActiveIndex() const;
    std::wstring ActivePath() const;
    void SyncChrome();
    void RefreshStats();
    // 把配置里的视图偏好（模式/排序/分组/窗格）套到指定标签。
    void ApplyPersistedViewState(ShellView* view);

    // 导航
    // 唯一出口：路径 -> 历史栈 + 一次 Navigate。所有入口都必须经过这里。
    bool NavigateActive(const std::wstring& path, bool push);
    void GotoHistory(int target);  // 历史游标移动的唯一入口（后退/前进/跳转共用）
    void GoBack();
    void GoForward();
    void GoUp();
    void GoHome();
    void RefreshActive();
    void JumpHistory(int depth, bool backward);
    std::vector<std::wstring> HistoryList(bool backward) const;
    // 清空地址框下拉里的历史：当前标签的后退/前进栈 + 全局"最近访问"。
    // 两者都清是因为下拉把它们合并成一个列表，只清一半会让人以为没生效。
    // 保留当前目录作为唯一历史条，且不触发导航。
    void ClearHistory();
    void OnTabNav(ShellView* view, const std::wstring& path, const std::wstring& display,
                  unsigned items);
    void OnTabNavFailed(ShellView* view);
    int FindTab(ShellView* view) const;

    // 输入
    bool OnChromeMouse(int target, UINT msg, int button, int mods, POINT pt);

    // 菜单/对话框
    void ShowStatusMenu(POINT screen);
    void ShowCommandPalette();
    void ShowTabMenu(int index, int action);
    void ShowViewMenuAt();

    // 命令辅助
    IShellView* ActiveView();
    // 跑一个"对当前 IShellView 操作"的命令：统一判成败、失败记日志 + 响铃。
    // 文件操作与视图两组命令共用，所以做成成员而不是某个函数内的 lambda。
    bool WithView(const std::function<bool(IShellView*)>& fn, const wchar_t* what = L"命令");
    // 确定式"复制/移动到目标文件夹"的公共实现（FsCopyTo / FsMoveTo 共用）。
    // 参数里 targets 是一组完整路径，to 是目标文件夹。
    bool TransferTargets(const Args& args, bool move);
    static int ViewModeCount();  // 与视图分组表一致（避免各处写 _countof）
    // 把 (FOLDERVIEWMODE, 图标尺寸) 反查回 0..7 的预留下标。无匹配返回 -1。
    // 导航栏视图菜单给的是这两个值，而命令系统要的是下标。
    static int ViewModeIndexFor(int fvm, int iconPx);
    void CopyText(const std::wstring& text);
    std::vector<std::wstring> SelectedOrFolder();
    void OpenPathInNewWindow(const std::wstring& path);
    void ApplyViewMode(int index);

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND viewArea_ = nullptr;
    Theme theme_;
    TabStrip tabStrip_;
    NavBar navBar_;
    BookmarkBar bookmarkBar_;
    StatusBar statusBar_;
    InputRouter input_;

    core::TabSet tabSet_;
    std::map<int, std::unique_ptr<Tab>> tabs_;
    int activeId_ = 0;
    // 视图模式 0..7（与导航栏的 8 档一致）。排序/分组/方向不在这里留副本：
    // 它们只在 Config::Settings 里存一份，需要时现取，避免两份值漂移。
    int viewMode_ = 3;
    bool inDestroy_ = false;
};
