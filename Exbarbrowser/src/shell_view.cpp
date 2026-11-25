#include "shell_view.h"

#include "path_util.h"
#include "shell_ops.h"

#include "platform_win.h"
#include "shell_view_helpers.h"

#include <propkey.h>
#include <uiautomation.h>

// 诊断日志走唯一实现（log.cpp）：以前这里有一份逐字复制的 LogLine，
// 且不做轮转 —— shell 侧日志量远大于 UI 侧，能悄悄写满磁盘。

// ---------------------------------------------------------------- 容器窗口
namespace {

LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<ShellView>(h);
    if (m == WM_ERASEBKGND) {
        if (self) {
            RECT rc;
            GetClientRect(h, &rc);
            self->EraseBackground(reinterpret_cast<HDC>(w), rc);
        }
        return 1;
    }
    // 容器拿到焦点时，必须转交给内部 Shell 视图并 UIActivate，否则列表一直是
    // "未激活"态：选中项画成淡灰、type-ahead/方向键/右键上下文都失效。
    // 同时通知 App"本视图被激活"：双栏下活动窗格由此跟随焦点，而不是固定左栏。
    if (m == WM_SETFOCUS) {
        if (self) {
            self->ActivateFocus();
            self->NotifyActivated();
        }
        return 0;
    }
    if (m == WM_TIMER && w == ShellView::kNavReplayTimer) {
        if (self) self->ReplayQueued();
        return 0;
    }
    if (m == WM_TIMER && w == ShellView::kPreviewReapplyTimer) {
        if (self) self->ReapplyPreviewPane();
        return 0;
    }
    if (m == WM_TIMER && w == ShellView::kSearchViewTimer) {
        if (self) self->OnSearchViewTimer();
        return 0;
    }
    if (m == WM_TIMER && w == ShellView::kThemeRestoreTimer) {
        if (self) self->RestoreThemeState();
        return 0;
    }
    if (m == WM_APP_NAV_REPLAY) {  // 兼容旧的直接投递
        if (self) self->ReplayQueued();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

// IExplorerBrowser 宿主站点。真正决定"打开/关闭预览窗格"的是框架的
// IExplorerPaneVisibility，而不是 IFolderView2 的当前目录标志；后者只是当前目录
// 的即时状态。Force-off 保证新目录不会按各自保存的状态重新把预览窗格画出来。
HRESULT STDMETHODCALLTYPE ShellView::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IServiceProvider) {
        *ppv = static_cast<IServiceProvider*>(this);
    } else if (riid == IID_IExplorerPaneVisibility) {
        *ppv = static_cast<IExplorerPaneVisibility*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE ShellView::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&comRef_));
}

ULONG STDMETHODCALLTYPE ShellView::Release() {
    // ShellView 由 App 拥有；站点引用归零不能 delete，Destroy() 会显式清站点。
    ULONG r = static_cast<ULONG>(InterlockedDecrement(&comRef_));
    return r;
}

HRESULT STDMETHODCALLTYPE ShellView::QueryService(REFGUID guidService, REFIID riid,
                                                  void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (guidService == SID_ExplorerPaneVisibility &&
        (riid == IID_IExplorerPaneVisibility || riid == IID_IUnknown)) {
        if (riid == IID_IUnknown)
            *ppvObject = static_cast<IUnknown*>(static_cast<IServiceProvider*>(this));
        else
            *ppvObject = static_cast<IExplorerPaneVisibility*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE ShellView::GetPaneState(REFEXPLORERPANE ep,
                                                  EXPLORERPANESTATE* peps) {
    if (!peps) return E_POINTER;
    // 命令栏（ribbon）整组强制关：只要导航树 + 文件列表。
    //
    // 这是**官方途径**，不是隐藏窗口。IExplorerPaneVisibility::GetPaneState 的
    // 窗格清单里本来就含命令栏 —— EP_Commands（命令栏本体）、
    // EP_Commands_Organize / EP_Commands_View（组织/视图两组）、EP_Ribbon（整个 ribbon）。
    // 对它们回 EPS_DEFAULT_OFF | EPS_FORCE，框架就不再创建命令栏，而不是"创建了再
    // ShowWindow(SW_HIDE)"。这也是其他托管系统视图的项目去掉命令栏的做法。
    //
    // 为什么必须 EPS_FORCE：EPS_DEFAULT_OFF 只是"默认值"，shell 会用**每个目录自己
    // 保存的窗格状态**把它盖回去（EP_PreviewPane 就是踩过这个坑，见下）。用户意图是
    // "这个宿主不要命令栏"，不该被目录历史状态推翻。
    //
    // 与 EP_PreviewPane 的差别：预览窗格是用户可切换的（previewPaneWanted_），命令栏
    // 是本宿主的固定设计（本工程用自绘导航栏/命令面板替代它），所以无条件关。
    if (ep == EP_Commands || ep == EP_Commands_Organize || ep == EP_Commands_View ||
        ep == EP_Ribbon) {
        *peps = (EXPLORERPANESTATE)(EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        return S_OK;
    }
    // 导航窗格同理走官方开关，而不是"建好树再 SW_HIDE"。
    //
    // 双栏下只有活动窗格该有导航树（app 的共享侧栏语义）。旧做法是隐藏内层
    // SysTreeView32：树看不见了，但外层 NamespaceTreeControl 占的那 187px 列宽
    // 还留着 —— 非活动窗格左侧凭空多出一条空白，文件列表被挤到右边（实测
    // 空白 x 差 196px）。SetOptions/SetRect 都改不动已建好的窗格布局。
    //
    // 按 EPS_FORCE 回"关"时，框架**根本不创建** EP_NavPane，宽度自然是文件列表的。
    // 这是与 EP_PreviewPane 同一条已验证的路径。
    if (ep == EP_NavPane) {
        *peps = navPaneWanted_
                    ? (EXPLORERPANESTATE)(EPS_DEFAULT_ON | EPS_INITIALSTATE | EPS_FORCE)
                    : (EXPLORERPANESTATE)(EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        return S_OK;
    }
    if (ep == EP_PreviewPane) {
        // **开与关都必须是 EPS_FORCE**（2026-11-20 修正）。
        //
        // 原来是"开给 EPS_DEFAULT_ON（非强制）、关给 EPS_FORCE"，理由是想保留 shell
        // 自带预览开关的用户可切换性。但这造成了一个静默失效：EPS_DEFAULT_ON 只是
        // "默认值"，shell 会用**每个目录自己保存的窗格状态**覆盖它 —— 于是配置里
        // showPreviewPane=true，实际窗格仍然不可见（实测窗格宿主窗口被创建，但
        // 一直是 0x0、SW_HIDE）。
        //
        // 语义上这也说不通：本工程的预览窗格偏好是**每实例**状态（previewPaneWanted_，
        // 见 shell_view.h 的注释），由命令 view.toggle.preview 显式控制并写回配置。
        // 既然用户意图明确，"按目录的历史状态"就不该有权覆盖它。
        //
        // 代价：shell 自带的"预览窗格"菜单项不再能单独改变这一项（会被本值压回）。
        // 这是有意的取舍 —— 一个偏好只有一处真相，好过"两个开关互相打架"。
        *peps = previewPaneWanted_
                    ? (EXPLORERPANESTATE)(EPS_DEFAULT_ON | EPS_INITIALSTATE | EPS_FORCE)
                    : (EXPLORERPANESTATE)(EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        return S_OK;
    }
    *peps = EPS_DONTCARE;
    return S_OK;
}

// ---------------------------------------------------------------- ShellView
bool ShellView::Create(HWND parent, const std::wstring& path) {
    win::RegisterClassOnce(kTabViewClass, ViewProc);
    // WS_CLIPSIBLINGS：双栏分隔条是兄弟窗口，本容器不得覆盖它（HWND_TOP 也挡不住）。
    container_ = CreateWindowExW(0, kTabViewClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                 0, 0, 10, 10, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!container_) return false;
    SetThis(container_, this);

    // 容器先放行深浅色，再建 EBC：IExplorerBrowser 在 Initialize 时会按父窗口
    // 当前的 dark allowance 决定 DirectUI 用哪套样式。只在 OnViewCreated 补
    // ApplyShellTheme 已经晚了（实测强制深色启动时文件区仍是浅色）。
    const bool dark = theme_.dark;
    AllowDarkForWindow(container_, dark);
    SetWindowTheme(container_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);

    // 窗格偏好必须在首次导航前就位：EBO_SHOWFRAMES / IExplorerPaneVisibility 只在
    // 建窗格时被消费，晚一步就要等下一次导航才生效（表现为"第一次打开时窗格还在"）。
    // 偏好经 App 注入回调读取（R16：本层不碰 Config）；未注入时按关闭兜底。
    // navPaneWanted_ 若已被调用方预置（双栏右窗格），保留那个值，不拿全局偏好盖掉。
    if (!navPanePreset_) {
        navPaneWanted_ = panePrefs_.getShowNav ? panePrefs_.getShowNav() : false;
    }
    previewPaneWanted_ = panePrefs_.getShowPreview ? panePrefs_.getShowPreview() : false;
    previewPaneWantedKnown_ = true;

    if (!InitBrowser()) return false;
    if (!AdviseBrowser()) return false;

    if (!path.empty()) Navigate(path);
    return true;
}

bool ShellView::InitBrowser() {
    HRESULT hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IExplorerBrowser, reinterpret_cast<void**>(&browser_));
    if (FAILED(hr) || !browser_) return false;

    // 关键：把宿主放进 ExplorerBrowser 的站点链，框架才会在建窗格时查询
    // IExplorerPaneVisibility。没有这一步，EBO_SHOWFRAMES 每次导航都按各目录的
    // 保存状态重新创建预览窗格，单纯 SetCurrentFolderFlags 守不住。
    IUnknown_SetSite(browser_, static_cast<IUnknown*>(static_cast<IServiceProvider*>(this)));

    // 真实签名：Initialize(HWND, const RECT*, FOLDERVIEWOPTIONS 按值)。
    // vtable: 0 QueryInterface / 1 AddRef / 2 Release / 3 Initialize。
    using InitFn = HRESULT(STDMETHODCALLTYPE*)(IExplorerBrowser*, HWND, const RECT*, int);
    void** vt = *reinterpret_cast<void***>(browser_);
    auto init = reinterpret_cast<InitFn>(vt[3]);

    RECT rc{0, 0, 10, 10};
    hr = init(browser_, container_, &rc, 0 /* FVO_DEFAULT */);
    LogDbg(L"[init] Initialize hr=0x%08lX", static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        IUnknown_SetSite(browser_, nullptr);
        browser_->Release();
        browser_ = nullptr;
        return false;
    }

    // 原生导航窗格：由 shell 自己绘制（快速访问/此电脑/驱动器/展开收起/右键/拖放），
    // 不自己造树。EBO_SHOWFRAMES 必须在首次导航前设置。
    // 用户关了导航窗格时不要置位，否则首个视图仍会画出窗格（随后只被隐藏、布局留下空位）。
    browser_->SetOptions(navPaneWanted_ ? EBO_SHOWFRAMES : EBO_NONE);
    // browser_->SetPropertyBag(L"ExBarBrowser");  // 临时诊断：禁用持久 bag
    return true;
}

void ShellView::Destroy() {
    // 只拆浏览器，不拆容器：重建 IExplorerBrowser 时容器窗口及其类名、坐标、
    // 子类上下文都要原样留住（DestroyBrowser 见 shell_view_theme.cpp）。
    DestroyBrowser();
    // 主题两阶段切换若在中途被销毁，清掉尚未消费的搜索 PIDL。
    if (themePendingSearchPidl_) {
        CoTaskMemFree(themePendingSearchPidl_);
        themePendingSearchPidl_ = nullptr;
    }
    ClearThemeRestorePidls();
    themeSuspended_ = false;
    if (container_) {
        DestroyWindow(container_);
        container_ = nullptr;
    }
}

void ShellView::Resize(const RECT& rc) {
    if (!container_) return;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    // 【几何没变就不动窗口】SetWindowPos 同一个位置同一尺寸也会触发 WM_SIZE ->
    // shell 视图整块重绘。双栏下 Layout() 会被调用多次（切窗格、显隐、比例），
    // 每次都无条件 SetWindowPos 就是"切到右栏时闪烁"的直接成因。
    // 这里比一次缓存的矩形，相同就整体跳过（SetRect 同理：它内部按"矩形变了没有"
    // 决定要不要重排，传一样的值本来就会被忽略）。
    // 比的是**整个矩形**（含位置）：位置变了尺寸没变时也必须移动窗口。
    if (lastW_ == w && lastH_ == h && lastX_ == rc.left && lastY_ == rc.top) return;
    lastW_ = w;
    lastH_ = h;
    lastX_ = rc.left;
    lastY_ = rc.top;
    // 【SWP_NOCOPYBITS：拖动分割条时不残留】缺了它，SetWindowPos 会把原有客户区
    // 像素**拷贝**到新位置（默认优化），于是拖动每走一步旧内容就贴一份；缩小时
    // 缩掉的那一条会留在屏幕上。加上它改为直接重排、由窗口自己重绘，不留副本。
    SetWindowPos(container_, nullptr, rc.left, rc.top, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    if (browser_) {
        RECT inner{0, 0, w, h};
        browser_->SetRect(nullptr, inner);
    }
}

void ShellView::SetVisible(bool visible) {
    if (visible) {
        // 标签切回前台时记一次当前目录的预览状态：用户可能用 shell 菜单/关闭按钮改过，
        // 这次采样能让"隐藏"在随后导航到其他目录时继续生效。
        IShellView* view = CurrentView();
        CapturePreviewPaneState(view);
        if (view) view->Release();
    }
    if (container_) ShowWindow(container_, visible ? SW_SHOWNA : SW_HIDE);
}

bool ShellView::Navigate(const std::wstring& path) {
    if (!browser_ || path.empty()) return false;
    LogDbg(L"[nav] request path=%s viewReady=%d", path.c_str(), viewReady_ ? 1 : 0);
    return DoNavigate(path);
}

bool ShellView::DoNavigate(const std::wstring& path) {
    if (!browser_) return false;

    // 离开当前目录前记下它的预览窗格状态。OnNavigationPending 也会做同样的采样，
    // 覆盖 shell 内部导航；这里保留一份，确保程序发起的导航在回调前已经锁定意图。
    CapturePreviewPaneBeforeNavigation();

    // 【两个归一化，各管一维】正斜杠会让 SHCreateItemFromParsingName 返回
    // E_INVALIDARG（实测），所以外部路径一律过 ToShellPath；而命名空间
    // （裸 `::{GUID}`）不吃分隔符规则，主页只认 `shell:::` 前缀 —— 会话恢复
    // 把裸 ::{679F85CB-…} 直接喂进来时解析失败，右窗格一次都导航不成功，
    // 用户看到"右栏空白"。ToParsingName 补前缀，谁都不能替另一个。
    const std::wstring p = pathutil::ToParsingName(pathutil::ToShellPath(path));

    // search-ms: 是**协议处理器 URL**，不是可浏览的 folder。IExplorerBrowser 没有
    // 搜索带（search band），BrowseToObject 对它返回 S_OK 却把视图留在原地 ——
    // 于是"搜索成功"变成一句假话（实测：零结果查询后 itemCount 一个不少）。
    // 在这里明确拒绝，让调用方拿到失败而不是假成功。
    //
    // 【真搜索怎么走】同一个搜索结果文件夹以 **PIDL** 形式交给 BrowseToObject 就
    // 能导航（原生搜索文件夹，见 src/search_folder.h 与 ShellView::NavigatePid）：
    // 那条拒绝判的是字符串前缀，不是文件夹类型。
    if (p.rfind(L"search-ms:", 0) == 0) {
        LogDbg(L"[nav] 宿主不支持 search-ms URL（无搜索带）：%s", p.c_str());
        return false;
    }

    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_IShellItem,
                                             reinterpret_cast<void**>(&item));
    LogDbg(L"[nav] parse '%s' hr=0x%08lX", p.c_str(), static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    hr = browser_->BrowseToObject(item.get(), 0);
    LogDbg(L"[nav] browse hr=0x%08lX", static_cast<unsigned long>(hr));
    if (hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
        // 上一次浏览还没收尾：这是正常抖动，不是失败。记下最后一次请求并重放，
        // 否则快速连点会弹"无法打开"、历史栈还会被回滚。
        queued_ = p;
        // 用定时器退避重放，不要 PostMessage 立刻重试：同一消息循环里 PostMessage
        // 会立刻自旋，实测连重 6 次；定时器排在 shell 完成消息之后，通常 1 次即成功。
        if (container_) {
            static const UINT kDelay[] = {50, 100, 200, 500, 1000};
            UINT d = kDelay[replayTries_ < 5 ? replayTries_ : 4];
            SetTimer(container_, kNavReplayTimer, d, nullptr);
        }
        return true;
    }
    if (FAILED(hr)) return false;  // 同步失败由 App::NavigateActive 统一弹框+回滚，避免双触发
    return true;
}

void ShellView::SetViewMode(int fvm, int iconPx) {
    if (!browser_) return;
    IShellView* view = nullptr;
    if (SUCCEEDED(browser_->GetCurrentView(IID_IShellView, reinterpret_cast<void**>(&view))) && view) {
        // 走唯一实现：手写 QI 会绕过 ComPtr 的 Reset 语义（见 shell_ops.h 注释）。
        ComPtr<IFolderView2> fv2 = shellops::FolderView2(view);
        if (fv2) {
            fv2->SetViewModeAndIconSize(static_cast<FOLDERVIEWMODE>(fvm), iconPx);
            // 读回校验：Set 成功但视图不重画时能立刻从日志看出来（不再盲目 Refresh）
            FOLDERVIEWMODE gotMode = FVM_AUTO;
            int gotIcon = 0;
            if (SUCCEEDED(fv2->GetViewModeAndIconSize(&gotMode, &gotIcon)))
                LogDbg(L"[view] set %d/%d -> %d/%d", fvm, iconPx, static_cast<int>(gotMode),
                        gotIcon);
            // 列枚举/逐项取大小诊断已移到 EXBAR_DIAG_PREVIEW 路径；启动期不再跑。
        }
        view->Release();
    }
}

void ShellView::Refresh() {
    if (!browser_) return;
    IShellView* view = nullptr;
    if (SUCCEEDED(browser_->GetCurrentView(IID_IShellView, reinterpret_cast<void**>(&view))) && view) {
        view->Refresh();
        view->Release();
    }
}

bool ShellView::GetViewMode(int* fvm, int* iconPx) {
    if (!browser_) return false;
    IShellView* view = nullptr;
    if (FAILED(browser_->GetCurrentView(IID_IShellView, reinterpret_cast<void**>(&view))) || !view)
        return false;
    ComPtr<IFolderView2> fv2 = shellops::FolderView2(view);
    view->Release();
    if (!fv2) return false;
    FOLDERVIEWMODE mode = FVM_AUTO;
    int icon = 0;
    if (FAILED(fv2->GetViewModeAndIconSize(&mode, &icon))) return false;
    if (fvm) *fvm = static_cast<int>(mode);
    if (iconPx) *iconPx = icon;
    return true;
}

unsigned ShellView::ItemCount() {
    if (!browser_) return 0;
    IShellView* view = nullptr;
    if (FAILED(browser_->GetCurrentView(IID_IShellView, reinterpret_cast<void**>(&view))) || !view)
        return 0;

    unsigned count = 0;
    IFolderView* fv = nullptr;
    if (SUCCEEDED(view->QueryInterface(IID_IFolderView, reinterpret_cast<void**>(&fv))) && fv) {
        int n = 0;
        if (SUCCEEDED(fv->ItemCount(SVGIO_ALLVIEW, &n)) && n > 0) count = static_cast<unsigned>(n);
        fv->Release();
    }
    view->Release();
    return count;
}

IShellView* ShellView::CurrentView() {
    if (!browser_) return nullptr;
    IShellView* view = nullptr;
    if (FAILED(browser_->GetCurrentView(IID_IShellView, reinterpret_cast<void**>(&view))))
        return nullptr;
    return view;
}

// 键盘焦点落点。顺序很重要：
//   ExplorerBrowserControl 是 IExplorerBrowser 自己的宿主窗口，它收到 WM_SETFOCUS
//   后会自己走 OnViewWindowActive → 内部视图 UIActivate。焦点只停在外层容器
//   （ExBarBrowser_TabView）时，框架完全不知道"本视图被激活了"。
HWND ShellView::FocusTarget() {
    if (!container_) return nullptr;
    if (HWND ebw = FindChildByClass(container_, L"ExplorerBrowserControl")) return ebw;
    if (HWND def = FindChildByClass(container_, L"SHELLDLL_DefView")) return def;
    return container_;
}

void ShellView::ActivateFocus() {
    if (!browser_ || activating_) return;
    activating_ = true;

    HWND target = FocusTarget();
    // 1) 把焦点交给浏览器窗口：这是 IExplorerBrowser 认可的激活路径
    if (target && GetFocus() != target) SetFocus(target);
    // 2) 再显式通知视图激活态（菜单/命令栏启用态依赖这一步）
    if (IShellView* view = CurrentView()) {
        view->UIActivate(SVUIA_ACTIVATE_FOCUS);
        view->Release();
    }
    // 3) 视图在 UIActivate 里可能把焦点搬进自己的子窗口；只在焦点跑到容器外时才纠回
    if (target && IsWindow(target)) {
        HWND focus = GetFocus();
        if (!focus || focus == container_) SetFocus(target);
    }
    activating_ = false;
}

void ShellView::NotifyActivated() {
    // 焦点真正进入本视图（容器 WM_SETFOCUS）时通知 App。双栏下 App 据此把
    // 活动窗格改成本视图，使命令/导航/状态栏跟随焦点。仅在回调存在时触发，
    // 避免无 App 的场景（理论上不会发生，但保持与 navFn_/failFn_ 一致的惰性判空）。
    if (activateFn_) activateFn_();
}
