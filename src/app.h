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
#include "file_undo.h"
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
    // 退出前确认（多标签 + confirmCloseTabs）。true = 继续退出。
    // WM_CLOSE 与 win.quit / 系统菜单退出共用这一份判定。
    bool ConfirmExit();
    void RegisterCommands();
    // 命令注册按域分片（app_commands*.cpp），RegisterCommands 只做调度。
    void RegisterTabCommands();
    void RegisterFileCommands();
    // 长任务域（工作线程 + 进度 + 取消）：体积统计 / 摘要 / 查重 / 比较。
    // 与 RegisterFileCommands 分开是因为它们有**独立的并发约定**，
    // 而那个约定只该有一份（见 app_commands_scan.cpp 文件头）。
    void RegisterScanCommands();
    void RegisterCompareCommands();
    // 作业域：作业查询/控制命令 + 事件接线（进度与完成推给订阅者）。
    void RegisterJobCommands();
    // 退出前收尾：取消全部作业并 join 工作线程（实现见 app_commands_jobs.cpp）。
    void ShutdownJobs();
    // fs.*（按路径操作）单独成文件：作用域与 file.*（选中集）不同。
    void RegisterFsCommands();
    void RegisterMiscCommands();
    void RegisterDevCommands();
    void RegisterArchiveCommands();
    void Layout();
    // dpiOverride > 0 时用它（WM_DPICHANGED 的 wParam 才是权威值）
    void ApplyTheme(int dpiOverride = 0);
    // 用户主目录。做成成员是为了让分片文件里的 HomePath() 不用再共享一个匿名命名空间。
    static std::wstring HomePath();
    // 状态栏取数的节流定时器 id（OnCreate 起、WndProc 收、OnDestroy 停）。
    static constexpr UINT_PTR kStatsTimer = 1;
    // AI 通道的请求泵定时器：监听线程排队 → 本定时器在 UI 线程执行命令。
    static constexpr UINT_PTR kAiTimer = 2;
    // 会话自动快照：标签增删/移动/切换/导航后 2s 落盘一次（去抖）。
    // 正常退出仍会立即保存；这个定时器只为"崩溃/断电"兜底。
    static constexpr UINT_PTR kSessionTimer = 3;
    // 标记会话已变：重启 2s 去抖定时器（重复 SetTimer 只改间隔，不叠加）。
    void MarkSessionDirty();
    // 会话快照的唯一写入点（内存 + 落盘）。退出与自动快照共用。
    void SaveSessionSnapshot();
    // 系统菜单（标题栏右键 / Alt+Space）自定义项的命令 ID 段。
    // 避开系统保留的 SC_* (0xF000+)：LOWORD(WM_SYSCOMMAND::wParam) 落在这一段时
    // 由 WndProc 转投命令总线，其余交 DefWindowProc（还原/移动/大小/最小化…）。
    static constexpr UINT kSysMenuBase = 0xE100;
    enum SysMenuCmd : UINT {
        kSysViewMenu = kSysMenuBase + 1,
        kSysPalette,
        kSysPrefs,
        kSysHotkeys,
        kSysQuit,
    };
    // 把应用入口挂进系统菜单。只追加高层入口并转投现有命令（NavViewMenu /
    // CommandPalette / Preferences / HotkeyManager / QuitExplorer），视图菜单的
    // 8 档子菜单不在系统菜单里再抄一份 —— 那会造出第二套实现，迟早漂移。
    void BuildSystemMenu();

    // 标签
    int NewTab(const std::wstring& path, int index = -1);
    int NewTabIndex() const;  // tabNewPos: right = 当前标签右侧, end = 末尾
    // 路径是否值得开一个标签：命名空间/shell: 一律允许；文件系统路径要求存在。
    //
    // 为什么需要预检：ShellView::Create 对不存在的目录也可能成功（导航失败是
    // 异步回报），于是 tab.new 会先报 ok=true、再留下一个空标签。命令侧
    // 宁可不建，也不要制造"开成功了但其实没打开"的假成功。
    static bool PathOpenable(const std::wstring& path);
    // 重开关闭历史里的第 index 条（0 = 最近关闭）。
    //
    // 先建成功再弹栈：老实现先 PopClosed 再 NewTab，路径已不存在时
    // NewTab 返回 0，关闭记录却已被弹掉 —— 条目永久丢失。返回值是
    // "真的重开了"，失败由 handler 用 NoteFailure 给出具体路径。
    bool ReopenClosed(int index = 0);
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
    // 固定/取消固定（按值设定，幂等）。持久化口径与锁一致：按路径存，
    // 重启恢复会话时同一目录仍是固定的。
    void SetTabPinned(int id, bool pinned);
    void TogglePin(int id);
    // Ctrl+Tab / Ctrl+Shift+Tab：按 MRU 循环（forward=false 时反向）。
    bool CycleMru(bool forward);

    // ---- 文件操作撤销栈 ----
    //
    // 记一条可撤销的文件操作（唯一写入口）。
    // byMachine 取自本次调用的 actor —— AI 做的与人工做的必须分得开，
    // 否则机器一次批量撤销能把用户的手工整理全部滚掉。
    void NoteFileOp(fileundo::Op op, std::vector<fileundo::Item> items,
                    const std::wstring& label);
    // 真正执行撤销（逆操作）。返回 false = 没撤成（原因写进 why）。
    // 失败时会把"已经撤成功"的项从记录里剔掉，让重试只试剩下的。
    bool ApplyFileUndo(fileundo::Entry& e, std::wstring* why);
    // 标签切换器（Ctrl+Shift+A）：列出全部标签，支持键入前缀定位。
    // 选中后激活；取消不改状态。
    void ShowTabSwitcher();
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
    // 焦点面板判定（观测面）：按真实窗口层级判断焦点属于哪个面板，
    // 匹配不上就如实返回 unknown，不猜。AI 状态快照每次现取。
    std::wstring FocusPaneOf(HWND focus);

    // 会话选中恢复（P3）：启动时挂起，首次导航完成时消费一次。
    // pendingSelPath_ 是目标目录（匹配才恢复）；用户先导航去了别处即放弃。
    std::wstring pendingSelPath_;
    std::vector<std::wstring> pendingSelItems_;
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
    // 返回"最近访问"被清掉的条数（命令层要如实报数，不能只说成功）。
    int ClearHistory();
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
    // “选择详细信息”：反复弹出列勾选菜单，每次选择都走 view.columns.set 命令，
    // 直到用户按 Esc 或选“完成”。不自己模拟输入，只展示菜单并接命令。
    void ShowColumnMenu(POINT at);

    // 图标大小档位菜单（Ctrl+滚轮之外的离散入口）：每项是 view.zoom.set。
    void ShowZoomMenu(POINT at);

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
    // 复制文本到剪贴板。返回是否真的写入了 —— 命令 handler 直接把它当结果：
    // 剪贴板被别的程序占用时 OpenClipboard 会失败（已内部重试），
    // 这时报 ok=true 等于骗调用方"复制好了"。
    bool CopyText(const std::wstring& text);
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
    // MRU 循环进行中：CycleMru 自己调 Activate，不能让它把循环快照重置掉；
    // 其余任何激活（点击/命令/新建）都会结束循环。
    bool mruCycling_ = false;
    // 视图模式 0..7（与导航栏的 8 档一致）。排序/分组/方向不在这里留副本：
    // 它们只在 Config::Settings 里存一份，需要时现取，避免两份值漂移。
    int viewMode_ = 3;
    bool inDestroy_ = false;
    // 搜索视图的来源目录（非空 = 当前标签正在显示搜索结果）。
    // 搜索视图没有"上一级"，取消搜索必须靠它回到进来时的目录 ——
    // 否则取消之后用户只能自己重新输一遍路径。
    std::wstring searchFrom_;
    // 当前生效的搜索词。与 searchFrom_ 同生同灭：只给观测面（state.searchQuery）用，
    // 不参与判定 —— 判定一律看 searchFrom_ 是否为空。
    std::wstring searchQuery_;
    // 文件操作撤销栈（唯一一份）。
    //
    // 为什么需要它：shell 的撤销谓词在 IExplorerBrowser 宿主里取不到
    // （背景菜单没有 undo 行），于是"可撤销"要么是兑现不了的声明，
    // 要么就得自己记一份"做过什么"。见 src/file_undo.h。
    //
    // 边界：机器的撤销不穿透人工操作（栈顶是人工做的就不给机器撤）。
    fileundo::Stack fileUndo_;
};
