// 应用编排：窗口、布局、标签生命周期、命令注册、消息路由。
//
// 状态归属：
//   · 标签顺序/锁定/MRU/关闭历史 -> core::TabSet（唯一）
//   · 每个标签的 Shell 视图     -> App::tabs_（id 映射）
//   · 标签图标（HICON）         -> TabStrip（唯一所有者，App 不碰）
//   · 视觉令牌                 -> Theme（唯一）
//   · 键位/鼠标规则            -> InputRouter（唯一）
#pragma once
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bookmark_bar.h"
#include "commands.h"
#include "common.h"
#include "config.h"
#include "file_undo.h"
#include "aichat_panel.h"
#include "ai_status_peek.h"
#include "nav_pane.h"
#include "input_router.h"
#include "nav_bar.h"
#include "nav_history.h"
#include "split_bar.h"
#include "side_layout.h"
#include "settings_dialog.h"
#include "shell_view.h"
#include "status_bar.h"
#include "status_zoom_slider.h"
#include "layout_key.h"
#include "tab_set.h"
#include "tab_strip.h"
#include "theme.h"
#include "view_stats.h"

class App {
public:
    static App& Get();
    // initialPath：启动要打开的目录；selectTarget：/select,<文件> 要选中的完整路径（可空）。
    // 两者分开 because SelectPaths 要完整路径、导航目标是父目录。
    int Run(HINSTANCE hInst, const std::wstring& initialPath,
            const std::wstring& selectTarget = std::wstring());

    HWND hwnd() const { return hwnd_; }
    // AI 面板（命令层需要它判"可用"并做断言回读）。
    aiui::Panel& aiPanel() { return aiPanel_; }
    const aiui::Panel& aiPanel() const { return aiPanel_; }
    void OpenExternal(const std::wstring& path,
                      const std::wstring& selectTarget = std::wstring());
    // 把键盘焦点还给当前标签的视图片。热键总线上所有"离开编辑态"的路径都收口到这里，
    // 否则焦点停在 OWNER 主窗口上：type-ahead / Delete / Enter 全部失效（只剩热键还能用）。
    void FocusActiveView();

private:
    App() = default;

    // 一个窗格 = 一个系统 Shell 视图 + 它自己的导航栈与视图私有状态。
    // 双栏里两个窗格的目录可以完全不同；把历史放在标签级会让地址栏/后退栈互相串台。
    //
    // 【槽位与方向是两回事】槽位永远是 0/1（左右并排时 0=左 1=右，上下堆叠时 0=上 1=下）；
    // 路径/历史/选中/视图档位全挂在**槽位**上，方向只决定这两个槽位怎么摆 —— 切方向不丢状态。
    struct Pane {
        std::unique_ptr<ShellView> view;  // 第二窗格按需创建，关闭双栏后保留以保住状态
        nav::History nav;
        unsigned items = 0;
        std::wstring display;
        SplitCache split;  // 文件夹/文件分类缓存（状态栏 800ms 取数不再重算）
        std::wstring searchFrom;
        std::wstring searchQuery;
        std::wstring path() const { return nav.here(); }
    };

    struct Tab {
        int id = 0;
        std::array<std::unique_ptr<Pane>, 2> panes;
        int activePane = 0;  // 0 主窗格，1 第二窗格；单栏时永远是 0
        // 标签代表"用户正在看的那个视图"：路径随活动窗格走。
        // 两个窗格自己的目录/历史仍各自保存在 Pane 里，互不覆盖。
        std::wstring path() const {
            return panes[static_cast<size_t>(activePane)]
                       ? panes[static_cast<size_t>(activePane)]->path()
                       : std::wstring();
        }
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool CreateMainWindow(HINSTANCE hInst);
    void OnCreate();
    void OnDestroy();
    // 退出前确认（多标签 + confirmCloseTabs）。true = 继续退出。
    // WM_CLOSE 与 win.quit / 系统菜单退出共用这一份判定。
    bool ConfirmExit();
    void RegisterCommands();
    // 破坏性命令的人工确认：把弹框实现注入命令总线（实现见 app_confirm.cpp）。
    void InstallConfirmCallback();
    // 命令前置状态与 AI 状态快照的接线（实现见 app_state.cpp）。
    void RegisterStateProviders();
    // 命令注册按域分片（app_commands*.cpp），RegisterCommands 只做调度。
    void RegisterTabCommands();
    void RegisterFileCommands();
    // 文件视图的双栏：开关 / 切换活动窗格 / 比例。作用域是窗格，不是标签。
    void RegisterViewPaneCommands();
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
    // 设置域（config.*）：枚举/读写/导出/还原。单独成文件是因为它要落地到
    // 界面（栏高、主题、窗格、状态栏段…），混在杂项里会把那个文件推过欠账线。
    void RegisterConfigCommands();
    // 书签编辑族（收藏夹增删改查、拖放/重排的结构化入口）：作用域自成一体，
    // 实现见 app_commands_misc_bookmarks.cpp。
    void RegisterBookmarkCommands();
    // 带参外部入口（clipboard.copy / ext.terminal.at）与书签文件 IO
    // （bmk.import/export/reload）：作用域与 misc 不同，各自独立成文件。
    void RegisterExternalCommands();
    void RegisterBookmarkIoCommands();
    // 窗口管理（minimize/maximize/topmost/center/move/resize/list/tile/cascade）：
    // 作用域是顶层窗口，与 misc 的"进程级动作"分开。
    void RegisterWindowCommands();
    void RegisterDevCommands();
    void RegisterArchiveCommands();
    // 搜索域（导航栏搜索框的范围/历史）。
    void RegisterSearchCommands();
    // force=true 时跳过"输入没变就整段跳过"的指纹（见 app_layout.cpp 的 LayoutKey）。
    // 用于"某个控件刚刚才建出来"这类场景：新建这件事不在指纹里，必须显式要求重排。
    void Layout(bool force = false);
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
    // 文件视图自动刷新（M7）：shell 变更连发时合并成一次 RefreshActive。
    // 300ms 去抖，避免批量文件操作把视图刷几十次、也避免和用户自己的导航打架。
    static constexpr UINT_PTR kViewRefreshTimer = 4;
    void ScheduleViewAutoRefresh();
    // 根集发现完成后换成完整清单（见 nav_pane_root.h 的 Prefetch*）。
    // 发现要 461 ms（15 次绑定第三方网盘扩展 + Home 枚举），同步跑会卡住首帧，
    // 所以先挂内置三项出图、发现走工作线程，算完 PostMessage 回这里。
    // 幂等：没算完就什么都不做。
    void ApplyDiscoveredRootsIfReady();
    // 真正销毁关标签时摘下来的视图（WM_APP_RECLAIM_VIEWS）。幂等。
    void ReclaimPendingViews();
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
    // activate=false：建好标签但不切走当前视图（书签右键「在后台新标签中打开」）。
    // 新标签的 ShellView 仍然创建并导航，只是容器保持隐藏，用户点它时才显示。
    int NewTab(const std::wstring& path, int index = -1, bool activate = true);
    int NewTabIndex() const;  // tabNewPos: after_current = 当前右侧, end = 最右
    // 按需创建某个窗格的 Shell 视图；已创建时幂等。第二窗格首次打开双栏时才真正建。
    bool EnsurePaneView(Tab* tab, int paneIndex);
    void WirePaneCallbacks(Tab* tab, int paneIndex, ShellView* view);
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
    // 真正执行重做（正向操作 beforePath → afterPath，与撤销对称）。
    // 失败时同样把已成功的项从记录里剔掉，让重试只试剩下的。
    bool ApplyFileRedo(fileundo::Entry& e, std::wstring* why);
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
    std::wstring ActivePath();
    // 双栏：0 主窗格 / 1 第二窗格（左右并排=左/右，上下堆叠=上/下）。PaneAt 永不返回
    // null（标签创建时两个 Pane 都在），但 Pane::view 可能还没创建（按需创建）。
    static Pane* PaneAt(Tab* tab, int index);
    Pane* ActivePane();
    ShellView* ActiveShellViewHost();
    // 活动窗格变化时把 tabSet_ 的路径/标题/别名/颜色/锁定态改成"当前显示的视图"：只改模型，不重排标签（固定态属于标签本身）。
    void SyncTabIdentity(Tab* tab);
    // 全局导航窗格的显隐/宽度落地：整个窗口只有一棵，固定在内容区最左，与方向无关。
    void ApplyNavPane();
    void ApplyPaneChrome();
    // 从活动 ShellView 现读视图档位；读不到才退回 viewMode_。
    int ActiveViewModeIndex();
    void SetDualPane(bool on);
    void NextPane();
    void CreateSplitBars();   // 建三条布局边界条（窗格间 / 导航|文件 / 文件|AI）
    void SetPaneSplit(int percent);
    void OnSplitterDragged(int percent, bool final);   // final=false 只重排，true 才落盘
    // 侧栏边界拖动：落点是像素（边界中心，客户区坐标）。
    // 与 OnSplitterDragged 分开，是因为两条边界的"像素 -> 宽度"换算是相反的
    // （导航 = center，AI = 客户区宽 - center），混在一个函数里就得靠参数区分。
    void OnNavEdgeDragged(int centerX, bool final);
    void OnAiEdgeDragged(int centerX, bool final);
    // 双栏方向：false = 左右并排，true = 上下堆叠（垂直双栏）。只改摆法，不动窗格槽位
    // —— 两个窗格的路径/历史/选中全部保留（理由与翻转/应用两种语义见 app_panes.cpp）。
    void SetPaneVertical(bool vertical);
    void ApplyPaneVertical();   // 按当前 paneVertical 重排
    // 焦点进入某个 ShellView 时，把该视图所属窗格设为活动窗格：双栏下这是
    // "激活哪个视图就哪个响应"的触发点（点击/Tab 进入即切换），另一窗格保持不变。
    void OnPaneActivated(ShellView* view);
    // 焦点在哪个视图（返回窗格下标，-1 = 焦点不在本标签的任何视图里）。
    // 活动窗格的唯一真源：焦点从哪来（键盘/程序/鼠标/shell 内部）都一视同仁。
    int FocusedPaneIndex(Tab* tab) const;
    // 焦点事件到达时按真实焦点同步外壳（双栏：活动窗格跟随焦点）。
    // 幂等：焦点没落在视图里、或本来就在同一窗格，都只做无害的刷新。
    void OnFocusMaybeChanged();
    // 焦点事件观察（EVENT_OBJECT_FOCUS，只读）：任何来源的焦点变化都据此同步外壳。
    void StartFocusWatch();
    void StopFocusWatch();
    // Shell 回调反查所属窗格：FindTab 只保留给不关心左右的老调用点。
    bool FindPane(ShellView* view, Tab** tab, int* paneIndex) const;
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

    // AI 面板：创建（含拉起 node 服务）与显隐切换。
    // 服务是面板的前置条件 —— 没服务时面板显示占位页而不是白屏。
    void InitAiPanel();
    void ToggleAiPanel();
    // AI 页自绘标题栏的窗口动作（close/minimize/maximize/pin/unpin）：
    // 统一转投命令总线（ai.panel.toggle / win.minimize / win.maximize /
    // win.restore / win.topmost），与系统菜单、快捷键同源。
    void DispatchAiPanelWindowAction(const std::wstring& action);
    // 页面经 WebMessage 上报的 AI 状态：chip 文本 + peek 内容。
    // 参数用基本类型，避免把 WebView COM 绑定类型漏进 app.h 的公共面。
    void ApplyAiWebStatus(const std::wstring& state, const std::wstring& summary,
                          const std::wstring& detail, bool canUndo,
                          const std::wstring& undoDomain, int turnId);
    // 页面尚未上报前，Web 视图的原生启动进度。starting 会让页面状态归零；
    // 真实页面状态一旦到达，原生态不再回写（除非后来 WebView2 失败）。
    void RefreshAiNativeStatus();
    void ApplyAiNativeStatus(const std::wstring& state, const std::wstring& summary,
                             const std::wstring& detail);
    // 页面状态与原生态共用的落盘逻辑：未读、peek、chip 只有这一处写入。
    void ApplyAiStatusCore(const std::wstring& state, const std::wstring& summary,
                           const std::wstring& detail, bool canUndo,
                           const std::wstring& undoDomain, int turnId, bool fromPage);
    void ShowAiPeek();
    void ToggleAiPeek();
    void HideAiPeek();
    // 用户看过这次结果（chip / peek 关闭 / 打开对话 / 撤销）后清未读。
    // 只改 chip 文案，不改状态本身；面板展开时由 ApplyAiWebStatus 直接清。
    void ConsumeAiUnread();
    // chip 文案与 tooltip 的唯一写入点：状态与未读都要走这里。
    void RefreshAiChip();
    // ai.* 命令的注册入口（app_commands_ai.cpp）。
    void RegisterAiPanelCommands();
    void RefreshStats();
    // 显式标脏：选中/隐藏状态/窗格结构等命令改完后调它，再 RefreshStats()
    // 强制走一次全量取数；800ms 定时器只调 RefreshStats()，靠轻量判据决定是否全量。
    void MarkStatsDirty();
    // 把配置里的视图偏好（模式/排序/分组/窗格）套到指定标签。
    void ApplyPersistedViewState(ShellView* view);

    // 导航
    // 唯一出口：路径 -> 历史栈 + 一次 Navigate。所有入口都必须经过这里。
    bool NavigateActive(const std::wstring& path, bool push);
    // 同一套导航落到**指定窗格**：另一窗格打开用它，活动窗格由 NavigateActive 转调。
    // 收 Pane* 而不是下标，是为了让"另一窗格"的选取留在调用方（那里才知道谁是活动）。
    bool NavigateInPane(Pane* p, const std::wstring& path, bool push);
    // 书签右键「在另一窗格中打开」：单栏时先开双栏，再把路径打到非活动窗格。
    bool OpenPathInOtherPane(const std::wstring& path);
    void GotoHistory(int target);  // 历史游标移动的唯一入口（后退/前进/跳转共用）
    void GoBack();
    void GoForward();
    void GoUp();
    void GoHome();
    void RefreshActive();
    void JumpHistory(int depth, bool backward);
    // 历史跳转的**总线入口**：界面侧（地址框下拉 / 后退·前进下拉）走它，
    // 内部转投 nav.history.jump 命令 —— 与命令面板/AI 同源，有审计与事件。
    void JumpHistoryViaCommand(int depth, bool backward);
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
    // 从面板（命令/序列总览）派发一条命令：破坏性确认 + 必填参数追问统一在此。
    void InvokeFromPalette(const CommandDef& d);
    void ShowTabMenu(int index, int action);
    void ShowViewMenuAt();
    // 设置对话框：表（settings_reg）驱动的可调项列表，改动走 config.set 命令。
    // 【为什么是独立窗口而不是弹出菜单】旧的设置面板是一整块弹出菜单：39 项平铺，
    // 高度只够露出十几项，剩下的靠滚动找，而"这一项属于哪一类"完全没有表达。
    // 设置本来就有明确分组（栏 / 视图 / 序列键 / 标签 / 窗口 / 会话 / 状态栏 /
    // 键位 / AI），分组就该出现在界面上 ——
    //   左栏分类页签 → 中栏按类排 → 右栏讲清这一项的名称、键名与生效时机。
    // 详见 src/settings_dialog.h 顶部的取舍说明。
    //
    // 【非模态】多数设置立即生效，模态窗会把主窗口挡住，用户改完只能凭记忆
    // 猜"到底变了没有"。它是主窗口的 owned 窗口：主窗口关掉时它跟着销毁。
    void ShowSettingsDialog();
    // 把配置里的历史上限推到实际拥有它们的两个模型上：
    //   · recentLimit / searchLimit → Config 自己的两个 vector（超出的立刻裁掉）
    //   · closedTabLimit → core::TabSet（见 TabSet::SetClosedLimit）
    // 启动时调一次，config.set 之后调一次。收成一个函数，是因为"改了上限"
    // 的三条路径（启动 / 面板 / AI）必须落在同一处 —— 三处各写一遍的话，
    // 面板改了 AI 改不了（或反过来），而表现只是"有时生效"。
    void ApplyHistoryLimits();
    // 把配置里的四条栏高推给 Theme。建窗前调一次、config.set 后调一次：
    // Theme 只保存实例值，不直接读全局 Config，保持它作为渲染对象的独立性。
    void ApplyBarHeights();
    // “选择详细信息”：反复弹出列勾选菜单，每次选择都走 view.columns.set 命令，
    // 直到用户按 Esc 或选“完成”。不自己模拟输入，只展示菜单并接命令。
    void ShowColumnMenu(POINT at);

    // 图标大小档位菜单（Ctrl+滚轮之外的离散入口）：每项是 view.zoom.set。
    void ShowZoomMenu(POINT at);

    // 在搜索视图里搜当前目录（query 为空 = 取消搜索回到来源目录）。
    //
    // 导航栏搜索框与 search.find 命令共用这一份实现：两处各写一遍，表现迟早
    // 不同（"搜索框能搜、命令搜不到"这种最典型的假等价）。成功时把结果视图的
    // 标题写进 title，供命令返回结构化结果。
    bool RunSearch(const std::wstring& query, std::wstring* title);
    // 搜索框提交的总线入口：转投 search.find 命令，与命令面板/AI 同源。
    void RunSearchViaCommand(const std::wstring& query);

    // ---- 搜索域（导航栏搜索框）----
    //
    // 范围与历史的真相在 Config::Settings 里（App 不持有副本，R19 同源）。
    // 左带弹「搜索范围」、右带弹「搜索历史」—— 两个菜单里的每一项都走**命令**
    // （search.scope.set / search.query.set / search.history.clear）：
    // 鼠标路径与键盘/AI 路径必须同源，否则就是两套实现。
    //
    // widthPx = 搜索框宽度（物理像素，由导航栏给）：弹层与搜索框同宽、
    // 左右边缘齐平。宽度跟着锚点一起传，App 不自己量（几何在导航栏那一侧）。
    void ShowSearchScopeMenu(POINT at, int widthPx);
    void ShowSearchHistoryMenu(POINT at, int widthPx);
    // 当前搜索范围档位（0..3，见 searchscope::Range）。
    static int SearchRange();
    // 设置搜索范围。path 只对"自定义路径"有意义（其它档忽略它）。
    static void SetSearchRange(int range, const std::wstring& path);

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
    // 把视图里**真实**的图标像素回灌给滑块（读回而不是自己记一份 —— 见实现处注释）。
    void SyncZoomSlider();

    // ---- 序列总览弹窗（复刻 mod 的 ExBarSeqOverlay）----
    // 取数与派发都在本层：弹窗只管 UI/键盘，App 用既有数据源填行、
    // 统一经 CommandBus 派发（与人机等价 R20 同源）。
    std::vector<SequenceOverlay::Row> SequenceRows(const std::wstring& mode,
                                                   const std::wstring& filter);
    void SequenceExecute(const SequenceOverlay::Row& row);
    // 打开弹窗（Ctrl+Shift+P / win.palette 的应用入口）。
    void ShowSequenceOverlay(const std::wstring& mode = std::wstring());
    void SetupSequenceOverlay();

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND viewArea_ = nullptr;
    HWND viewTopCover_ = nullptr;    // 内容区上沿 1px 覆盖带（压住 Shell 原生边框）
    HWND viewBottomCover_ = nullptr; // 内容区下沿 1px 覆盖带
    Theme theme_;
    TabStrip tabStrip_;
    NavBar navBar_;
    BookmarkBar bookmarkBar_;
    StatusBar statusBar_;
    // 状态栏右端的图标大小滑块（独立子窗口，见 status_zoom_slider.h）。
    StatusZoomSlider zoomSlider_;
    // 设置对话框（成员而非全局：它需要一个 owner 窗口与一份主题，
    // 两者都在 App 里；做成全局就得多传参数或多存一份全局状态）。
    SettingsDialog settingsDialog_;
    // 全局导航窗格：窗口唯一一棵，固定在内容区最左侧（不属于任何窗格）。
    // 用的是 IExplorerBrowser 的原生 EP_NavPane（shell 自己绘制的树），
    // 见 src/nav_pane.h 顶部的取舍说明。
    navpane::NavPane navPane_;
    // AI 面板（右侧）：宿主是子窗口，内部挂 WebView2。
    // 与 navPane_ 对称 —— 一个占最左，一个占最右，中间区域归文件视图。
    // 见 src/aichat_panel.h。
    aiui::Panel aiPanel_;
    // 状态栏 AI chip 的 peek 气泡（面板收起后的完成摘要 / 撤销 / 打开对话）。
    aiui::StatusPeek aiPeek_;
    // 最近一次页面上报的 AI 状态。RefreshStats 每次重建 StatusModel，
    // 但 chip 不能回到"空闲"，所以这里保留一份权威值。
    std::wstring aiStatusState_ = L"idle";
    std::wstring aiStatusSummary_ = L"AI · 空闲";
    std::wstring aiStatusDetail_;
    bool aiStatusCanUndo_ = false;
    std::wstring aiStatusUndoDomain_;
    int aiStatusTurnId_ = 0;
    // 页面是否已经上报过真实状态。原生启动态只在它变 true 之前接管 chip；
    // 这样可以避免"页面已经显示完成，宿主又把 chip 改回准备中"。
    bool aiPageStatusSeen_ = false;
    // 面板收起期间到达的终态结果是否被用户看过。peek 只是呈现层，
    // 自动弹出不算"已读"；用户点 chip / 关闭 / 打开对话 / 撤销才算。
    bool aiUnread_ = false;
    // 三条边界共用 SplitBar 状态机，只在落点口径上不同：panesBar_=百分比、
    // navEdge_=导航宽像素、aiEdge_=AI 宽像素。分三套实现会各自漏捕获/悬停。
    SplitBar panesBar_;
    SplitBar navEdge_;
    SplitBar aiEdge_;
    InputRouter input_;

    core::TabSet tabSet_;
    std::map<int, std::unique_ptr<Tab>> tabs_;
    int activeId_ = 0;
    // MRU 循环进行中：CycleMru 自己调 Activate，不能让它把循环快照重置掉；
    // 其余任何激活（点击/命令/新建）都会结束循环。
    bool mruCycling_ = false;
    // 会话恢复期间不急建第二窗格 ShellView：路径/历史先入模型，最后只为活动标签建一次。
    bool restoringSession_ = false;
    // 后台新标签正在建：它自己的 ShellView 会同步报告"被激活"，
    // 若照单全收会把当前标签/窗格切走 —— 与"后台"语义相反。
    // 记下正在建的 Tab（视图回调发生在 id 分配之前，只能用指针身份），
    // OnPaneActivated 对属于它的视图忽略，创建完即清零。
    Tab* backgroundTab_ = nullptr;
    // 首帧还没画出来（Run 的 ShowWindow 之前）。
    //
    // 【为什么需要】建第二个窗格的 ShellView 实测 375 ms（双栏启动：
    // [+734] 建右视图 -> [+1109] 完成）。这段时间窗口还没显示，花掉的是
    // 用户"双击后等窗口"的耐心，而第二窗格对**第一眼**没有贡献 —— 首帧有主窗格就够。
    // 所以启动期把右视图延到首帧之后补（WM_APP_STARTUP_SETTLE），
    // 用户先看到窗口，第二窗格随后填上。非启动路径的 Activate 仍然同步建 ——
    // "点一下标签、第二窗格立刻有内容"这件事不能被拖。
    bool startupPending_ = false;
    // 焦点事件观察句柄（EVENT_OBJECT_FOCUS，out-of-context，只读）。
    // 它不改焦点、不注入输入，只用来在焦点落到另一个视图时同步外壳显示。
    void* focusWatch_ = nullptr;
    // 视图模式 0..7（与导航栏的 8 档一致）。排序/分组/方向不在这里留副本：
    // 它们只在 Config::Settings 里存一份，需要时现取，避免两份值漂移。
    int viewMode_ = 3;
    bool inDestroy_ = false;
    // 上一次 Layout 的输入指纹（定义见 layout_key.h）。相同就整段跳过。
    layout::LayoutKey lastLayout_{};
    // SyncChrome 的可见输入指纹：标题/标签条/地址栏/导航树用到的数据没变时，
    // 只刷新状态栏统计，不再重复 SetTabs（图标着色 + tooltip 重建）与 SyncTo。
    // 不把选中集放进来：它只影响状态栏，RefreshStats 每次仍会取。
    std::wstring lastChromeSignature_;
    // ---- 状态栏取数的节流状态（以前是 RefreshStats 里的函数内 static）----
    // 搬成成员的两个理由：① static 违反"组件内不得有文件级可变状态"（AGENTS §3）；
    // ② 多标签共用一个 lastPath 会让"路径变没变"判断错乱，本该节流的全量枚举被反复触发。
    // 这组状态是性能关键：探针 82 实测 System32（5003 项）只数总数 8.2 ms、
    // 逐项分类 85.7 ms —— 差 10 倍，所以"何时允许分类"必须由这里控。
    unsigned statsTick_ = 0;
    std::wstring statsLastPath_;
    // P0-2 轻量判据：只有这些量变化（或 MarkStatsDirty 标脏）才做全量取数。
    // 单看路径不够：同一目录里换选另一个文件，选中数不变但容量/类型/日期会变。
    bool statsDirty_ = true;
    std::wstring statsLastDisplay_;
    unsigned statsLastTotal_ = 0;
    unsigned statsLastSelCount_ = 0;
    int statsLastHidden_ = -1;
    IShellView* statsLastView_ = nullptr;

    // ---- 关标签时的视图延迟销毁队列 ----
    // 探针 84：ShellView::Destroy（IExplorerBrowser::Destroy + Release + DestroyWindow）
    // = 20~69 ms per view，全在 UI 线程；关一个标签销毁 1~2 个 → 20~138 ms。
    // 用户对"关闭"的期待是立刻消失，所以 CloseTab 只隐藏窗口 + 把 unique_ptr
    // 移进这里，真正的 release 交给 WM_APP_RECLAIM_VIEWS（消息循环空闲时）。
    // 生命周期：队列由 App 独占，视图已不在 tabs_ 里、无人能再拿到，所以安全；
    // 唯一风险是退出时队列非空 —— OnDestroy 必须清空它（见那里）。
    std::vector<std::unique_ptr<ShellView>> pendingDestroy_;
    // 文件操作撤销栈（唯一一份）。
    //
    // 为什么需要它：shell 的撤销谓词在 IExplorerBrowser 宿主里取不到
    // （背景菜单没有 undo 行），于是"可撤销"要么是兑现不了的声明，
    // 要么就得自己记一份"做过什么"。见 src/file_undo.h。
    //
    // 边界：机器的撤销不穿透人工操作（栈顶是人工做的就不给机器撤）。
    fileundo::Stack fileUndo_;
};
