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

    explicit ShellView(const Theme& theme) : theme_(theme) {}
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
    // Ctrl+Shift+E：展开导航窗格整棵树（原生 SysTreeView32，逐节点 TVM_EXPAND）。
    // 返回 false = 没找到树控件或树是空的（调用方据此回报"没生效"，不假报成功）。
    bool ExpandNavTree();
    // 导航窗格的真实可见性由自己维护：EBO_SHOWFRAMES 只在"导航前"被消费，
    // 导航后再 SetOptions 无效、清零更是永不生效，GetOptions 读到的只是意图。
    bool navPaneWanted_ = true;
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
    // 文件视图鼠标事件汇入 InputRouter。必须在 Create() 前设置，
    // 因为首次 IShellView 可能在 Create() 内部就创建完成。
    void SetMouseSink(MouseSink fn) { mouseSink_ = std::move(fn); }
    // 导航失败（同步失败 / Shell 回调 OnNavigationFailed）。Sync 侧只处理"真失败"，
    // ERROR_BUSY 属于正常抖动，走排队重放而不是报错。
    void SetFailFn(std::function<void()> fn) { failFn_ = std::move(fn); }

    // 快速连续导航时 BrowseToObject 会返回 HRESULT_FROM_WIN32(ERROR_BUSY)：
    // 记下最后一次请求，等当前导航收尾后由容器窗口定时器重放（退避，不自旋）。
    void ReplayQueued();
    static constexpr UINT_PTR kNavReplayTimer = 1;
    // 导航完成后 shell 可能异步重建预览窗格，延后一拍再校正。
    void ReapplyPreviewPane();
    static constexpr UINT_PTR kPreviewReapplyTimer = 2;
#if defined(EXBAR_DIAG_PREVIEW)
    // 预览窗格/文件夹大小诊断（默认不编译；-DEXBAR_DIAG_PREVIEW 启用）。
    // 曾经直接写在 ReapplyPreviewPane 里，而后者由定时器反复调用 —— 见 .cpp 的说明。
    void DiagnosePreviewPaneState();
#endif

private:
    class Sink;
    friend class Sink;

    bool InitBrowser();
    bool DoNavigate(const std::wstring& path);
    void OnViewCreated(IShellView* view);
    void OnNavigated(PCIDLIST_ABSOLUTE pidl);
    void OnNavigationFailed();
    void CapturePreviewPaneState(IShellView* view);
    void ApplyPreviewPaneState(IShellView* view);
    void CapturePreviewPaneBeforeNavigation();
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
    std::wstring abandonedPath_;  // 重放耗尽时放弃的目标（迟到完成时只记日志）
    int replayTries_ = 0;
    bool viewReady_ = false;
    bool activating_ = false;  // ActivateFocus 重入守卫（SetFocus 会回投 WM_SETFOCUS）
    NavFn navFn_;
    MouseSink mouseSink_;
    std::function<void()> failFn_;
};
