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
    void RegisterDevCommands();
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
    // 关闭标签。返回是否**真的关掉了**：
    //   false = 标签不存在，或已锁定（只响一声提示）
    //   true  = 已关闭（注意：关掉最后一个标签会让程序退出）
    //
    // 返回值是为命令总线准备的 —— 以前它是 void，handler 只能无条件 return true，
    // 于是"锁定标签没关掉"在审计里被记成成功（ok=1），AI 也以为自己做到了。
    bool CloseTab(int id);
    // 返回**实际关闭数**。锁定标签会被跳过且不调用 CloseTab（否则每留一个就
    // 响一次铃）；一个都没关掉时命令必须如实报 false，不能记成 ok=1。
    int CloseOthers(int keepId);
    int CloseSide(int id, bool left);
    void Activate(int id);
    void MoveTab(int id, int toIndex);
    // 锁定状态的唯一写入点（按值设定，幂等）。ToggleLock 是它的"切换"用法。
    // 分两个名字而不是一个 bool 参数：命令侧要"确保锁着"，菜单要"翻转"，
    // 两种意图都常见，而 bool 参数在调用点读起来永远是 `ToggleLock(id, true)` ——
    // 读者得回去查"true 是锁还是切换"。
    void SetTabLocked(int id, bool locked);
    void ToggleLock(int id);
    // 用户重命名。空串 = 取消改名（退回跟随路径/Shell 名）。
    // 存 TabRef::alias 而不是覆盖 title：title 每次导航都会被 Shell 回调重写。
    void RenameTab(int id, const std::wstring& name);
    // 标签颜色下标（0 = 无颜色，见 Theme::TabColorFill）。
    void SetTabColor(int id, int color);
    // 关掉与 keepId 同路径的其他标签。返回真正关掉的个数（锁定标签不关）。
    int CloseDuplicates(int keepId);
    // 把某标签当前的 alias/color 写进配置（按路径）。改名与着色共用 ——
    // 两处各写一遍"先改模型、再回读、再写配置"必然分叉：一处漏写 color，
    // 表现就是"改了颜色，再改名时颜色被清掉"。
    void PersistTabMeta(int id);
    Tab* Active();
    Tab* ById(int id);
    int ActiveIndex() const;
    std::wstring ActivePath() const;
    void SyncChrome();
    // AI 通道热插拔：按当前配置启停管道服务与请求泵定时器（幂等）。
    // 启动时调一次；运行中 config.set aiEnabled 之后由 WM_APP_SYNC_AI 再调。
    void SyncAiChannel();
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
    void ShowTabColor(int index, int color);
    void ShowViewMenuAt();
    // 设置面板：表（settings_reg）驱动的可调项列表，改动走 config.set 命令。
    // at 是弹层的屏幕锚点（视图菜单按钮下方），与其它弹层一致。
    void ShowSettingsPanel(POINT at);

    // 在搜索视图里搜当前目录（query 为空 = 取消搜索回到来源目录）。
    //
    // 导航栏搜索框与 search.find 命令共用这一份实现：两处各写一遍，表现迟早
    // 不同（"搜索框能搜、命令搜不到"这种最典型的假等价）。成功时把实际用的
    // URL 写进 url，供命令返回结构化结果。
    bool RunSearch(const std::wstring& query, std::wstring* url);

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
    // 书签保存的统一善后：成功什么都不做；被"外部已改"拦下时告诉用户
    // 内存里的书签被另存到了哪个文件。
    //
    // 为什么必须集中：Save() 有 16 个调用点（拖放、菜单、导入、命令），
    // 那条"拒绝覆盖 + 另存 .conflict.html"的护栏如果只在某一处提示，
    // 其余 15 处都会安静失败 —— 用户以为加上了，其实没落盘。
    void NotifyBookmarkSave(bool ok);
    // 对**指定路径**弹 shell 属性框。与 CmdProps 的区别：那个作用于"当前选中项"，
    // 这个按路径点名（面包屑段右键要用，右键的那一刻那段并不在选中集里）。
    void ShowPathProperties(const std::wstring& path);
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
    // 搜索视图的来源目录（非空 = 当前标签正在显示搜索结果）。
    // 搜索视图没有"上一级"，取消搜索必须靠它回到进来时的目录 ——
    // 否则取消之后用户只能自己重新输一遍路径。
    std::wstring searchFrom_;
};
