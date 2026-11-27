// 一个标签页 = 一个真实 Shell 视图（IExplorerBrowser）
//
// 为什么用它：这是微软公开文档化的"在自有窗口里托管资源管理器视图"的 API。
// 右键菜单、拖放、就地重命名、缩略图、排序分组、命名空间全部由 shell 提供，
// 不需要注入 explorer，也不需要重写视图。
//
// ABI 注意（重要）：
//   mingw 的 shobjidl.h 把 Initialize 第三参声明成 const FOLDERSETTINGS*，
//   而 Windows 实际签名是 FOLDERVIEWOPTIONS 按值。按头文件调用会把指针当
//   枚举传进去。这里按 vtable 索引 3 直接调用真实签名（见 InitBrowser）。
#pragma once
#include "common.h"
#include "theme.h"

class ShellView : public IServiceProvider, public IExplorerPaneVisibility {
public:
    using NavFn = std::function<void(const std::wstring& path, const std::wstring& display,
                                     unsigned items)>;
    // 本视图被激活（焦点进入）时的回调：双栏下 App 用它把"活动窗格"改成本视图，
    // 使命令/导航/状态栏跟随焦点而不是固定左栏。与 DOpus/XYplorer 的"点击使面板激活"一致。
    using ActivateFn = std::function<void()>;
    // 窗格显隐偏好（R16：platform/shell 不直接读写 Config，改由 App 注入）。
    // GetShowPreview 供 TogglePreviewPane 判断"未知意图时跟随配置"；两个 Set 含落盘。
    struct PanePrefs {
        std::function<bool()> getShowNav;
        std::function<bool()> getShowPreview;
        std::function<void(bool)> setNavPane;
        std::function<void(bool)> setPreviewPane;
        explicit operator bool() const { return static_cast<bool>(setNavPane); }
    };

    explicit ShellView(const Theme& theme)
        : themeDarkApplied_(theme.dark ? 1 : 0), theme_(theme) {}
    // 虚析构：ShellView 实现了 COM 接口（有多张虚表），必须能经基类指针安全销毁，
    // 否则 unique_ptr<ShellView> 析构会触发 -Wdelete-non-abstract-non-virtual-dtor。
    virtual ~ShellView() { Destroy(); }

    ShellView(const ShellView&) = delete;
    ShellView& operator=(const ShellView&) = delete;

    // IExplorerBrowser 宿主站点：通过 IServiceProvider 提供 IExplorerPaneVisibility，
    // 让框架在每次创建窗格时都知道"预览窗格必须保持关闭"。
    // 注意：ShellView 仍由 App 独占管理，这里的引用计数只满足 COM 站点协议；
    // Release 归零不 delete，Destroy() 会显式断开站点。
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID guidService, REFIID riid,
                                           void** ppvObject) override;
    HRESULT STDMETHODCALLTYPE GetPaneState(REFEXPLORERPANE ep,
                                           EXPLORERPANESTATE* peps) override;

    bool Create(HWND parent, const std::wstring& path);
    void Destroy();

    void Resize(const RECT& rc);
    void SetVisible(bool visible);
    bool Navigate(const std::wstring& path);
    // 按 PIDL 导航（不是路径字符串）。
    //
    // 【为什么需要这条路】原生搜索文件夹（ISearchFolderItemFactory 造的 PIDL）
    // 只有 PIDL 形式，字符串形式是 search-ms: URL —— 而 URL 会被
    // ShellView::DoNavigate 按前缀拒绝（见 DoNavigate 里的说明）。
    // 走 PIDL 时那条拒绝不成立：它判的是**字符串**前缀，不是文件夹类型。
    bool NavigatePid(PIDLIST_ABSOLUTE pidl);
    void Refresh();
    void SetViewMode(int fvm, int iconPx);
    // 读回当前 FOLDERVIEWMODE 与图标像素（Ctrl+滚轮要在当前档位上改大小）。
    bool GetViewMode(int* fvm, int* iconPx);
    unsigned ItemCount();
    // 当前选中项数量。供"命令可否执行"的前置判定用，因此不走 statistics 缓存
    // （缓存 800ms 刷新一次，用作"能不能删"会得到过期答案）。
    unsigned SelectedCount();
    // 一次"观测选中"的条目：文件系统给完整路径，命名空间给解析名（::{GUID}\...）。
    struct SelectionItem {
        std::wstring path;
        bool isFolder = false;
    };
    // 枚举当前选中项（观测面：AI 要回答"选中的是哪些、是什么类型"）。
    // cap 是快照能携带的上限（防全选上万项撑爆管道响应）；返回选中总数 ——
    // out.size() < 返回值即发生截断。与 SelectedCount 同一条枚举路径，不出两份口径。
    unsigned SelectionPaths(unsigned cap, std::vector<SelectionItem>& out);
    // 恢复选中（会话重启）：逐条解析路径并 SelectItem。尽力语义 ——
    // 目录内容已变时部分条目会静默失败，不报错不阻塞启动。
    void SelectPaths(const std::vector<std::wstring>& paths);
    void ApplyTheme();
    void EraseBackground(HDC dc, const RECT& rc);  // 容器窗口 WndProc 用

    // 统计 / 排序 / 分组 / 文件操作都基于当前 IShellView（返回已 AddRef，调用方 Release）
    IShellView* CurrentView();
    void ToggleNavPane();
    bool NavPaneVisible() const;
    // 双栏下只让活动视图显示导航窗格：不写配置，只按值落地。
    void SetNavPaneVisible(bool visible) {
        if (!browser_ || navPaneWanted_ == visible) return;
        navPaneWanted_ = visible;
        ApplyNavPaneWanted();
    }
    // Ctrl+Shift+E：展开导航窗格整棵树（原生 SysTreeView32，逐节点 TVM_EXPAND）。
    // 返回 false = 没找到树控件或树是空的（调用方据此回报"没生效"，不假报成功）。
    bool ExpandNavTree();
    // 导航窗格的真实可见性由自己维护：EBO_SHOWFRAMES 只在"导航前"被消费，
    // 导航后再 SetOptions 无效、清零更是永不生效，GetOptions 读到的只是意图。
    bool navPaneWanted_ = true;
    // 上一次 Resize 的几何（缓存用来识别"没变"）。Rect 变了才真的动窗口 ——
    // 无条件 SetWindowPos 会触发 shell 视图整块重绘（双栏切栏时可见的闪烁）。
    int lastX_ = 0, lastY_ = 0, lastW_ = 0, lastH_ = 0;
    // 双栏右窗格用：Create 之前先置 true，告诉 Create"别用全局 showNavPane 覆盖
    // navPaneWanted_"。右窗格是后来出现的，它的导航树始终按活动窗格动态决定
    // （创建时默认关），不能跟着全局偏好走 —— 否则右栏会先建出一条 187px 的树列，
    // 隐藏后宽度不回收，左侧留一大块空白。
    bool navPanePreset_ = false;
    // 双栏右窗格在 Create 之前调用：声明"本窗格创建时不要导航树"。
    // 必须在 Create 前调，因为 EP_NavPane 只在建窗格时被消费。
    void PresetNoNavPane() {
        navPaneWanted_ = false;
        navPanePreset_ = true;
    }
    // 预览窗格意图：每实例独立（全局量会让一个标签的开关波及所有标签）。
    bool previewPaneWanted_ = true;
    void TogglePreviewPane();
    // 把键盘焦点交给 ExplorerBrowser 自己的宿主窗口，并通知视图激活。
    // 仅在缺少该步骤时选中高亮/键盘 type-ahead/右键上下文才会失效。
    void ActivateFocus();

    HWND hwnd() const { return container_; }
    const std::wstring& path() const { return path_; }
    const std::wstring& display() const { return display_; }
    void SetNavFn(NavFn fn) { navFn_ = std::move(fn); }
    // 导航守卫：**在 shell 真正导航之前**问一次（含双击文件夹这类 shell 内部导航 ——
    // 它们不走 ShellView::Navigate，只有 OnNavigationPending 能拦到）。
    // 返回 false = 取消这次导航（文档明确：OnNavigationPending 返回任意失败码即取消）。
    // 用途：锁定路径标签内点子目录 → 询问是否开新标签。设于 Create 之前。
    // 参数是新目标路径（已解析成显示路径）；未设置时一律放行。
    void SetNavGuard(std::function<bool(const std::wstring&)> fn) {
        navGuard_ = std::move(fn);
    }
    // 文件视图鼠标事件汇入 InputRouter。必须在 Create() 前设置，
    // 因为首次 IShellView 可能在 Create() 内部就创建完成。
    void SetMouseSink(MouseSink fn) { mouseSink_ = std::move(fn); }
    // 导航失败（同步失败 / Shell 回调 OnNavigationFailed）。Sync 侧只处理"真失败"，
    // ERROR_BUSY 属于正常抖动，走排队重放而不是报错。
    void SetFailFn(std::function<void()> fn) { failFn_ = std::move(fn); }
    // 焦点进入本视图时通知 App（双栏下由 App 更新活动窗格）。设于 Create 之前。
    void SetActivateFn(ActivateFn fn) { activateFn_ = std::move(fn); }
    // 窗格偏好读写回调。设于 Create 之前；未设置时 Toggle* 只改本视图、不落盘。
    void SetPanePrefs(PanePrefs prefs) { panePrefs_ = std::move(prefs); }
    void NotifyActivated();

    // 快速连续导航时 BrowseToObject 会返回 HRESULT_FROM_WIN32(ERROR_BUSY)：
    // 记下最后一次请求，等当前导航收尾后由容器窗口定时器重放（退避，不自旋）。
    void ReplayQueued();
    static constexpr UINT_PTR kNavReplayTimer = 1;
    // 主题切换的第一阶段：在进程颜色策略改变之前拆掉所有 EBC。
    // 必须与 FinishThemeSwitch() 成对使用；中间由 App::ApplyTheme 调用
    // ApplyProcessDarkMode()，最后再重建，避免 shell 用旧 DirectUI 的颜色缓存建新视图。
    void SuspendForThemeSwitch();
    void FinishThemeSwitch();
    // 导航完成后 shell 可能异步重建预览窗格，延后一拍再校正。
    void ReapplyPreviewPane();
    static constexpr UINT_PTR kPreviewReapplyTimer = 2;
    // 主题重建后的视图模式/选中项恢复：OnViewCreated 早于 GetCurrentView 就绪，
    // 必须延迟几拍重试，否则状态回灌会被静默丢掉。
    bool RestoreThemeState();
    static constexpr UINT_PTR kThemeRestoreTimer = 4;
    // 主题重建专用：保存选中项的绝对 PIDL。路径字符串会漏掉部分虚拟/特殊项，
    // PIDL 是 shell 自己的身份，回灌时不经过 SHParseDisplayName。
    unsigned CaptureSelectionPidls(unsigned cap, std::vector<PIDLIST_ABSOLUTE>& out);
    void SelectPidls(const std::vector<PIDLIST_ABSOLUTE>& pidls);
    void ClearThemeRestorePidls();

    // 搜索结果视图默认详细信息。
    //
    // 【为什么不能直接设】原生搜索文件夹的 DefView 由 Explorer 按 SearchResults
    // 模板创建，模板默认是"内容视图"；而且视图是**异步**创建的 —— 导航完成那一刻
    // GetCurrentView 还指着旧视图，此刻设视图模式会把用户原目录的视图改掉。
    // 所以挂一个定时器，等新视图就位（活动视图的当前目录就是我们导航到的那个
    // 搜索文件夹）再补设详细信息。同一套路见 exbar 的 MbEnsureSysSearchDetailsView。
    //
    // searchPidl 由本函数**接管**（内部 clone 一份），调用方仍负责释放自己那份。
    void EnsureSearchDetailsView(PIDLIST_ABSOLUTE searchPidl);
    static constexpr UINT_PTR kSearchViewTimer = 3;
    // 定时器回调（容器窗口的 WM_TIMER）：等搜索结果视图就位后补设详细信息。
    void OnSearchViewTimer();
    // 当前 IExplorerBrowser 是按哪一套深浅色建的。进程级 PreferredAppMode
    // 切换后，旧 EBC 不会跟着换色，只能重建（见 ApplyTheme 的说明）。
    int themeDarkApplied_ = 0;
    // 重建期间暂时寄存的视图状态，等新 IShellView 建好后由 OnViewCreated 恢复。
    std::vector<std::wstring> themeRestoreSelection_;
    std::vector<PIDLIST_ABSOLUTE> themeRestorePidls_;
    int themePendingFvm_ = -1;
    int themePendingIcon_ = 0;
    // SuspendForThemeSwitch() 拆浏览器前采样；FinishThemeSwitch() 用它们重建。
    bool themeSuspended_ = false;
    int themeRestoreTries_ = 0;
    RECT themePendingRect_{};
    PIDLIST_ABSOLUTE themePendingSearchPidl_ = nullptr;
#if defined(EXBAR_DIAG_PREVIEW)
    // 预览窗格/文件夹大小诊断（默认不编译；-DEXBAR_DIAG_PREVIEW 启用）。
    // 曾经直接写在 ReapplyPreviewPane 里，而后者由定时器反复调用 —— 见 .cpp 的说明。
    void DiagnosePreviewPaneState();
#endif

private:
    class Sink;
    friend class Sink;

    bool InitBrowser();
    // 销毁 IExplorerBrowser，但保留容器窗口与窗格状态。主题切换时用它重建
    // EBC：SetPreferredAppMode 只影响**新建**的 shell 窗口，旧 EBC 补发
    // WM_THEMECHANGED / SetWindowTheme 都不会换深浅色（实测）。
    void DestroyBrowser();
    bool AdviseBrowser();
    void DetachSink();
    bool RebuildBrowserForTheme(bool recreateContainer = false);
    bool DoNavigate(const std::wstring& path);
    void OnViewCreated(IShellView* view);
    void OnNavigated(PCIDLIST_ABSOLUTE pidl);
    void OnNavigationFailed();
    void CapturePreviewPaneState(IShellView* view);
    void ApplyPreviewPaneState(IShellView* view);
    void CapturePreviewPaneBeforeNavigation();
    // 导航守卫的实现（见 SetNavGuard 与 Sink::OnNavigationPending）。
    // 返回 true = 拦下这次导航（视图停在原目录）。
    bool ShouldBlockNavigation(PCIDLIST_ABSOLUTE pidl);
    void ApplyNavPaneWanted();
    // 是否已经从真实窗格窗口采样过状态（首次采样前不覆盖用户意图）。
    bool previewPaneWantedKnown_ = false;
    void SchedulePreviewReapply();
    HWND FocusTarget();
    const Theme& theme_;
    mutable LONG comRef_ = 1;  // COM 站点引用；归零不释放对象本身
    HWND container_ = nullptr;
    IExplorerBrowser* browser_ = nullptr;
    Sink* sink_ = nullptr;
    DWORD cookie_ = 0;
    std::wstring path_;
    std::wstring display_;
    std::wstring queued_;         // ERROR_BUSY 时排队的最后一次导航
    PIDLIST_ABSOLUTE queuedPidl_ = nullptr;  // 同上，PIDL 形式（搜索导航）
    PIDLIST_ABSOLUTE searchPidl_ = nullptr;   // 等就位的搜索结果文件夹（详情视图用）
    int searchViewTicks_ = 0;
    std::wstring abandonedPath_;  // 重放耗尽时放弃的目标（迟到完成时只记日志）
    int replayTries_ = 0;
    bool viewReady_ = false;
    bool activating_ = false;  // ActivateFocus 重入守卫（SetFocus 会回投 WM_SETFOCUS）
    NavFn navFn_;
    MouseSink mouseSink_;
    std::function<void()> failFn_;
    // 导航守卫（见 SetNavGuard）。返回 false 取消本次导航。
    std::function<bool(const std::wstring&)> navGuard_;
    ActivateFn activateFn_;
    PanePrefs panePrefs_;
};
