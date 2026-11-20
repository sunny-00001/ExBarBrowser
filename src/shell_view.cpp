#include "shell_view.h"

#include "config.h"
#include "platform_win.h"
#include "shell_input.h"

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
    if (m == WM_SETFOCUS) {
        if (self) self->ActivateFocus();
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
    if (m == WM_APP_NAV_REPLAY) {  // 兼容旧的直接投递
        if (self) self->ReplayQueued();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}



// ---------------------------------------------------------------- 主题
//
// 【重要】不要对 shell 内部窗口调用 SetWindowTheme！
//
// DirectUI（Win11 文件列表就是 DirectUIHWND + UIFILE 样式表）把"当前解析到的
// 视觉样式字典"缓存起来，SetWindowTheme 会让它改用另一套 SubAppName 的字典重新
// 解析样式表。实测：只要给视图子树里的子窗口设过主题，选中项就不再绘制高亮底色
// （选中集本身是存在的，状态栏"已选 N"也正常，只是不画背景块）。
//
// 离屏实验（tools/probe_highlight.cpp，像素差分判据）实测结果：
//   基线                            changed=4784 peakCount=174  有高亮
//   只对顶层 ExplorerBrowserControl   changed=4784 peakCount=174  有高亮
//   对全子树 SetWindowTheme          changed=1224 peakCount=14   无高亮
//   先画坏再 SetWindowTheme(NULL)    changed=4784 peakCount=174  恢复
//
// 同类项目的做法一致：只对 ExplorerBrowserControl 这一层设主题，交给 shell 自己
// 把深色传递给内部 DirectUI；内部窗口一律不碰。
bool ClassIsName(HWND h, const wchar_t* name) {
    if (!h) return false;
    wchar_t cls[64] = L"";
    if (!GetClassNameW(h, cls, _countof(cls))) return false;
    return wcscmp(cls, name) == 0;
}

// 找 parent 子树里第一个类名为 name 的窗口（深度优先，命中即停）。
HWND FindChildByClass(HWND parent, const wchar_t* name) {
    struct Ctx {
        const wchar_t* name = nullptr;
        HWND found = nullptr;
    } ctx;
    if (!parent) return nullptr;
    ctx.name = name;
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (ClassIsName(h, c->name)) {
                c->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

void ApplyShellTheme(HWND container, bool dark) {
    if (!container) return;
    // 只戳 IExplorerBrowser 自己的宿主窗口（container 直接子窗口中的那一个）。
    if (HWND ebw = FindChildByClass(container, L"ExplorerBrowserControl")) {
        AllowDarkForWindow(ebw, dark);
        // shell_view 是唯一例外：见文件顶部的 DirectUI 说明，不能走 ui::ThemeControl
        SetWindowTheme(ebw, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        // SetWindowTheme 内部已经会发 WM_THEMECHANGED，不要再手动广播：
        // 对 DirectUI 反复广播会让它重建内部状态，实测会丢选中绘制。
    }
}

// 深色滚动条等"小而独立"的控件才是需要逐窗设主题的目标；这类窗口在 shell 视图里
// 只有滚动条，其余全部交给 shell 自己处理。
BOOL CALLBACK ThemeScrollbarOnly(HWND h, LPARAM lp) {
    const bool dark = *reinterpret_cast<const bool*>(lp);
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, _countof(cls));
    if (wcscmp(cls, L"ScrollBar") == 0) {
        AllowDarkForWindow(h, dark);
        SetWindowTheme(h, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    return TRUE;
}

// 预览窗格意图是**每实例**状态（成员 previewPaneWanted_），不能用文件级全局量：
// GetPaneState 是每实例的 COM 方法，全局量会让"标签 A 关了预览窗格"波及所有标签。
// FOLDERFLAGS 里没有"显示预览窗格"这一位（0x400 是 FWF_NOSCROLL，几乎所有视图都置位），
// 真正决定窗格是否创建的是 IExplorerPaneVisibility::GetPaneState。

// 预览窗格宿主窗口：窗格隐藏时窗口对象仍然存在（被 resize 成 0 宽并 SW_HIDE），
// 所以必须同时看"可见 + 有实际宽度"，只按类名存在性判断会永远为真。
BOOL CALLBACK FindPreviewHostProc(HWND h, LPARAM lp) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, _countof(cls));
    if (wcscmp(cls, L"Shell Preview Extension Host") == 0) {
        RECT rc{};
        GetWindowRect(h, &rc);
        if (IsWindowVisible(h) && rc.right - rc.left > 0) {
            *reinterpret_cast<bool*>(lp) = true;
            return FALSE;
        }
    }
    return TRUE;
}

bool PreviewPaneVisibleNow(HWND container) {
    bool found = false;
    if (container) EnumChildWindows(container, FindPreviewHostProc, reinterpret_cast<LPARAM>(&found));
    return found;
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
    if (ep == EP_PreviewPane) {
        // 打开态只给默认值，保留 shell 自带"预览窗格"开关的用户可切换性。
        // 关闭态必须 EPS_FORCE：否则切目录时 shell 会按每个目录保存的状态重新打开。
        *peps = previewPaneWanted_ ? (EPS_DEFAULT_ON | EPS_INITIALSTATE)
                                    : (EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE);
        return S_OK;
    }
    *peps = EPS_DONTCARE;
    return S_OK;
}

// ---------------------------------------------------------------- 事件接收器
// 注意：Windows 实际用 4 个参数调用 OnNavigationComplete(pidl, IDataObject*, UINT)，
// mingw 头只声明了 1 个。多出的参数走寄存器、被忽略即可，不影响 vtable 布局。
class ShellView::Sink : public IExplorerBrowserEvents {
public:
    explicit Sink(ShellView* owner) : owner_(owner) {}
    virtual ~Sink() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExplorerBrowserEvents) {
            *ppv = static_cast<IExplorerBrowserEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override {
        // 双击文件夹等 shell 内部导航不会走 ShellView::Navigate()/DoNavigate()，
        // 必须在这里采样当前目录的真实状态，否则只能拿到上一次的旧意图。
        if (owner_) owner_->CapturePreviewPaneBeforeNavigation();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView* view) override {
        if (owner_) owner_->OnViewCreated(view);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE pidl) override {
        if (owner_) owner_->OnNavigated(pidl);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override {
        if (owner_) owner_->OnNavigationFailed();
        return S_OK;
    }

    // 销毁前断开反向指针：Unadvise 期间可能还有回调进来，不能摸已析构的 ShellView。
    void Detach() { owner_ = nullptr; }

private:
    LONG ref_ = 1;
    ShellView* owner_;
};

// ---------------------------------------------------------------- ShellView
bool ShellView::Create(HWND parent, const std::wstring& path) {
    win::RegisterClassOnce(kTabViewClass, ViewProc);

    container_ = CreateWindowExW(0, kTabViewClass, L"", WS_CHILD | WS_CLIPCHILDREN,
                                 0, 0, 10, 10, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!container_) return false;
    SetThis(container_, this);

    // 窗格偏好必须在首次导航前就位：EBO_SHOWFRAMES / IExplorerPaneVisibility 只在
    // 建窗格时被消费，晚一步就要等下一次导航才生效（表现为"第一次打开时窗格还在"）。
    const Settings& s = Config::Get().s();
    navPaneWanted_ = s.showNavPane;
    previewPaneWanted_ = s.showPreviewPane;
    previewPaneWantedKnown_ = true;

    if (!InitBrowser()) return false;

    // 引用计数：new 得 1（我们的），Advise 让浏览器 AddRef 到 2，
    // 再 Release 交出自己的那份 → 只剩浏览器的 1 份。
    // （原来这里多了一次 AddRef，销毁时只 Unadvise 不减我们那份，Sink 永久泄漏。）
    sink_ = new Sink(this);
    if (FAILED(browser_->Advise(sink_, &cookie_))) {
        cookie_ = 0;
        sink_->Release();
        sink_ = nullptr;
        return false;
    }
    sink_->Release();  // 交给浏览器持有

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
    LogLine(L"[init] Initialize hr=0x%08lX", static_cast<unsigned long>(hr));
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
    if (container_) shellinput::Uninstall(container_);
    if (browser_) {
        // 先断开站点链，避免销毁过程中框架仍回调 GetPaneState 摸到将死对象。
        IUnknown_SetSite(browser_, nullptr);
        if (sink_) {
            // 顺序关键：Unadvise 会释放浏览器持有的最后一份引用并 delete Sink，
            // 必须先 Detach 断开 owner_，否则 Detach 落在已释放对象上（use-after-free）。
            sink_->Detach();
            if (cookie_ && browser_) browser_->Unadvise(cookie_);
            cookie_ = 0;
            sink_ = nullptr;
        }
        browser_->Destroy();
        browser_->Release();
        browser_ = nullptr;
    }
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
    SetWindowPos(container_, nullptr, rc.left, rc.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
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
    LogLine(L"[nav] request path=%s viewReady=%d", path.c_str(), viewReady_ ? 1 : 0);
    return DoNavigate(path);
}

bool ShellView::DoNavigate(const std::wstring& path) {
    if (!browser_) return false;

    // 离开当前目录前记下它的预览窗格状态。OnNavigationPending 也会做同样的采样，
    // 覆盖 shell 内部导航；这里保留一份，确保程序发起的导航在回调前已经锁定意图。
    CapturePreviewPaneBeforeNavigation();

    // 正斜杠在 SHCreateItemFromParsingName 里是 E_INVALIDARG（实测），
    // 而用户/命令行经常给 C:/x/y 形式，这里统一成反斜杠。
    std::wstring p = path;
    if (p.rfind(L"shell:", 0) != 0 && p.rfind(L"::", 0) != 0) {
        for (auto& c : p)
            if (c == L'/') c = L'\\';
        while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    }

    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_IShellItem,
                                             reinterpret_cast<void**>(&item));
    LogLine(L"[nav] parse '%s' hr=0x%08lX", p.c_str(), static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    PWSTR name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
        LogLine(L"[nav] item=%s", name);
        CoTaskMemFree(name);
    }

    hr = browser_->BrowseToObject(item.get(), 0);
    LogLine(L"[nav] browse hr=0x%08lX", static_cast<unsigned long>(hr));
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
        IFolderView2* fv2 = nullptr;
        if (SUCCEEDED(view->QueryInterface(IID_IFolderView2, reinterpret_cast<void**>(&fv2))) && fv2) {
            fv2->SetViewModeAndIconSize(static_cast<FOLDERVIEWMODE>(fvm), iconPx);
            // 读回校验：Set 成功但视图不重画时能立刻从日志看出来（不再盲目 Refresh）
            FOLDERVIEWMODE gotMode = FVM_AUTO;
            int gotIcon = 0;
            if (SUCCEEDED(fv2->GetViewModeAndIconSize(&gotMode, &gotIcon)))
                LogLine(L"[view] set %d/%d -> %d/%d", fvm, iconPx, static_cast<int>(gotMode),
                        gotIcon);
            // 临时诊断：列管理器里到底有哪些列，Size 是否可见。
            IColumnManager* cm = nullptr;
            if (SUCCEEDED(fv2->QueryInterface(IID_IColumnManager,
                                             reinterpret_cast<void**>(&cm))) &&
                cm) {
                UINT all = 0, visible = 0;
                cm->GetColumnCount(CM_ENUM_ALL, &all);
                cm->GetColumnCount(CM_ENUM_VISIBLE, &visible);
                LogLine(L"[cols] all=%u visible=%u", all, visible);
                std::vector<PROPERTYKEY> keys(all > 0 ? all : 1);
                if (all > 0 && SUCCEEDED(cm->GetColumns(CM_ENUM_ALL, keys.data(), all))) {
                    for (UINT i = 0; i < all; ++i) {
                        CM_COLUMNINFO ci{};
                        ci.cbSize = sizeof(ci);
                        ci.dwMask = static_cast<DWORD>(CM_MASK_NAME | CM_MASK_STATE |
                                                       CM_MASK_WIDTH);
                        HRESULT hci = cm->GetColumnInfo(keys[i], &ci);
                        LogLine(L"[cols] %u fmt=%08X pid=%u name=%s state=%u width=%u hr=0x%08X",
                                i, keys[i].fmtid.Data1, keys[i].pid,
                                hci == S_OK ? ci.wszName : L"?", ci.dwState, ci.uWidth,
                                static_cast<unsigned long>(hci));
                    }
                }
                cm->Release();
                // 临时诊断：当前视图前 12 项的 PKEY_Size（含文件夹）。
                ComPtr<IShellItemArray> items;
                HRESULT hrItems = fv2->Items(SVGIO_ALLVIEW, IID_IShellItemArray,
                                             reinterpret_cast<void**>(&items));
                LogLine(L"[cols] Items hr=0x%08X array=%d", static_cast<unsigned long>(hrItems),
                        items ? 1 : 0);
                int itemCount = 0;
                fv2->ItemCount(SVGIO_ALLVIEW, &itemCount);
                LogLine(L"[cols] ItemCount=%d", itemCount);
                for (int i = 0; i < itemCount && i < 12; ++i) {
                    PITEMID_CHILD pidl = nullptr;
                    HRESULT hrp = fv2->Item(i, &pidl);
                    if (FAILED(hrp) || !pidl) {
                        LogLine(L"[cols] item %d Item hr=0x%08X", i,
                                static_cast<unsigned long>(hrp));
                        continue;
                    }
                    ComPtr<IShellItem> item;
                    HRESULT hrs = SHCreateItemFromIDList(pidl, IID_IShellItem,
                                                         reinterpret_cast<void**>(&item));
                    CoTaskMemFree(pidl);
                    if (FAILED(hrs) || !item) {
                        LogLine(L"[cols] item %d shell hr=0x%08X", i,
                                static_cast<unsigned long>(hrs));
                        continue;
                    }
                    PWSTR name = nullptr;
                    item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
                    SFGAOF attr = 0;
                    item->GetAttributes(SFGAO_FOLDER, &attr);
                    ComPtr<IShellItem2> i2;
                    UINT64 size = 0;
                    HRESULT hs = E_FAIL;
                    if (SUCCEEDED(item->QueryInterface(IID_IShellItem2,
                                                       reinterpret_cast<void**>(&i2))) &&
                        i2)
                        hs = i2->GetUInt64(PKEY_Size, &size);
                    LogLine(L"[cols] item %d folder=%d size=%llu hr=0x%08X name=%s", i,
                            (attr & SFGAO_FOLDER) ? 1 : 0, size,
                            static_cast<unsigned long>(hs), name ? name : L"?");
                    if (name) CoTaskMemFree(name);
                }
            }
            fv2->Release();
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

unsigned ShellView::SelectedCount() {
    if (!browser_) return 0;
    IShellView* view = nullptr;
    if (FAILED(browser_->GetCurrentView(IID_IShellView, reinterpret_cast<void**>(&view))) || !view)
        return 0;

    unsigned count = 0;
    IFolderView* fv = nullptr;
    if (SUCCEEDED(view->QueryInterface(IID_IFolderView, reinterpret_cast<void**>(&fv))) && fv) {
        // SVGIO_SELECTION 只数选中项。与 view_stats 用 IDataObject 的做法相比，
        // 这条路径不构造数据对象，代价更低，适合"每次判前置都调一次"。
        int n = 0;
        if (SUCCEEDED(fv->ItemCount(SVGIO_SELECTION, &n)) && n > 0)
            count = static_cast<unsigned>(n);
        fv->Release();
    }
    view->Release();
    return count;
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

void ShellView::CapturePreviewPaneState(IShellView*) {
    // 不能读 FOLDERFLAGS：其中没有"显示预览窗格"位（0x400 是 FWF_NOSCROLL，几乎总是置位，
    // 早先按它采样会把"已隐藏"误判成"已打开"，于是每次导航都被强制打开）。
    // 唯一可靠的观测是窗格宿主窗口是否真的可见。
    const bool visible = PreviewPaneVisibleNow(container_);
    previewPaneWanted_ = visible;
    previewPaneWantedKnown_ = true;
    LogLine(L"[preview] capture visible=%d", visible ? 1 : 0);
}

void ShellView::ApplyPreviewPaneState(IShellView*) {
    // 窗格是否创建由 IExplorerPaneVisibility::GetPaneState 决定（见上面 Force 返回），
    // 框架每次建窗格都会来问，所以这里不再重导航去"抢修" —— 那会和 shell 自己的
    // 重建流程互相触发，表现为目录反复刷新。只做观测与日志，便于排查。
    if (!previewPaneWantedKnown_) return;
    const bool visible = PreviewPaneVisibleNow(container_);
    if (visible != previewPaneWanted_) {
        LogLine(L"[preview] mismatch visible=%d wanted=%d（下一次建窗格按 GetPaneState 纠正）",
                visible ? 1 : 0, previewPaneWanted_ ? 1 : 0);
    }
}

void ShellView::CapturePreviewPaneBeforeNavigation() {
    IShellView* view = CurrentView();
    CapturePreviewPaneState(view);
    if (view) view->Release();
}

void ShellView::SchedulePreviewReapply() {
    if (!container_) return;
    KillTimer(container_, kPreviewReapplyTimer);
    SetTimer(container_, kPreviewReapplyTimer, 120, nullptr);
}

void ShellView::ReapplyPreviewPane() {
    if (container_) KillTimer(container_, kPreviewReapplyTimer);
    IShellView* view = CurrentView();
    ApplyPreviewPaneState(view);
    if (view) {
        IFolderView2* fv2 = nullptr;
        if (SUCCEEDED(view->QueryInterface(IID_IFolderView2,
                                          reinterpret_cast<void**>(&fv2))) &&
            fv2) {
            int n = 0;
            fv2->ItemCount(SVGIO_ALLVIEW, &n);
            LogLine(L"[late] items=%d", n);
                ComPtr<IShellFolder2> sf2;
                HRESULT hsf = fv2->GetFolder(IID_IShellFolder2,
                                             reinterpret_cast<void**>(&sf2));
                LogLine(L"[late] GetFolder(IShellFolder2) hr=0x%08X",
                        static_cast<unsigned long>(hsf));
                int sizeCol = -1;
                for (int c = 0; c < 40; ++c) {
                    SHELLDETAILS sd{};
                    if (FAILED(sf2->GetDetailsOf(nullptr, c, &sd))) continue;
                    wchar_t buf[128] = L"";
                    if (SUCCEEDED(StrRetToBufW(&sd.str, nullptr, buf, _countof(buf))) &&
                        (_wcsicmp(buf, L"大小") == 0 || _wcsicmp(buf, L"Size") == 0)) {
                        sizeCol = c;
                        LogLine(L"[late] Size column index=%d", c);
                        break;
                    }
                }
                for (int i = 0; i < n && i < 4; ++i) {
                    PITEMID_CHILD pidl = nullptr;
                    if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
                    VARIANT var{};
                    HRESULT hdet = sf2 ? sf2->GetDetailsEx(pidl, &PKEY_Size, &var) : E_FAIL;
                    unsigned long long v = 0;
                    if (SUCCEEDED(hdet) && var.vt == VT_UI8) v = var.ullVal;
                    if (SUCCEEDED(hdet)) VariantClear(&var);
                    wchar_t detText[128] = L"";
                    HRESULT hdetText = E_FAIL;
                    if (sizeCol >= 0) {
                        SHELLDETAILS sdItem{};
                        hdetText = sf2->GetDetailsOf(pidl, sizeCol, &sdItem);
                        if (SUCCEEDED(hdetText))
                            StrRetToBufW(&sdItem.str, nullptr, detText, _countof(detText));
                    }
                    ComPtr<IShellItem> item;
                    HRESULT hrs = SHCreateItemFromIDList(pidl, IID_IShellItem,
                                                         reinterpret_cast<void**>(&item));
                    CoTaskMemFree(pidl);
                    if (FAILED(hrs) || !item) continue;
                    PWSTR name = nullptr;
                    item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
                    SFGAOF attr = 0;
                    item->GetAttributes(SFGAO_FOLDER, &attr);
                    LogLine(L"[late] item %d folder=%d details=%llu hr=0x%08X text=%s hrText=0x%08X name=%s", i,
                            (attr & SFGAO_FOLDER) ? 1 : 0, v, static_cast<unsigned long>(hdet),
                            detText, static_cast<unsigned long>(hdetText), name ? name : L"?");
                    if (name) CoTaskMemFree(name);
                }
            // 临时诊断：强制重建 Size 列，看是否能触发 shell 重新取文件夹大小。
            IColumnManager* cm2 = nullptr;
            if (SUCCEEDED(fv2->QueryInterface(IID_IColumnManager,
                                             reinterpret_cast<void**>(&cm2))) &&
                cm2) {
                UINT vc = 0;
                cm2->GetColumnCount(CM_ENUM_VISIBLE, &vc);
                std::vector<PROPERTYKEY> vk(vc > 0 ? vc : 1);
                HRESULT hget = cm2->GetColumns(CM_ENUM_VISIBLE, vk.data(), vc);
                int sizeIdx = -1;
                for (UINT k = 0; k < vc; ++k)
                    if (IsEqualPropertyKey(vk[k], PKEY_Size)) sizeIdx = static_cast<int>(k);
                LogLine(L"[late] toggle cols vc=%u get=0x%08X sizeIdx=%d", vc,
                        static_cast<unsigned long>(hget), sizeIdx);
                if (sizeIdx >= 0 && vc > 1) {
                    std::vector<PROPERTYKEY> without;
                    for (UINT k = 0; k < vc; ++k)
                        if (static_cast<int>(k) != sizeIdx) without.push_back(vk[k]);
                    HRESULT h1 = cm2->SetColumns(without.data(),
                                                 static_cast<UINT>(without.size()));
                    HRESULT h2 = cm2->SetColumns(vk.data(), vc);
                    LogLine(L"[late] toggle hr1=0x%08X hr2=0x%08X",
                            static_cast<unsigned long>(h1), static_cast<unsigned long>(h2));
                }
                cm2->Release();
            }
            fv2->Release();
        }
        if (container_) {
            HWND root = GetAncestor(container_, GA_ROOT);
            if (root) SetForegroundWindow(root);
        }
        HWND vh = nullptr;
        if (SUCCEEDED(view->GetWindow(&vh)) && vh) {
            ComPtr<IUIAutomation> uia;
            if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                          CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                          reinterpret_cast<void**>(&uia))) &&
                uia) {
                ComPtr<IUIAutomationElement> root;
                if (SUCCEEDED(uia->ElementFromHandle(vh, &root)) && root) {
                    VARIANT v{};
                    v.vt = VT_I4;
                    v.lVal = UIA_ListItemControlTypeId;
                    ComPtr<IUIAutomationCondition> cond;
                    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v,
                                                               &cond)) &&
                        cond) {
                        ComPtr<IUIAutomationElementArray> arr;
                        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond.get(), &arr)) &&
                            arr) {
                            int cnt = 0;
                            arr->get_Length(&cnt);
                            LogLine(L"[uia] listitems=%d", cnt);
                            for (int i = 0; i < cnt && i < 4; ++i) {
                                ComPtr<IUIAutomationElement> el;
                                if (FAILED(arr->GetElement(i, &el)) || !el) continue;
                                BSTR name = nullptr;
                                el->get_CurrentName(&name);
                                LogLine(L"[uia] item %d name=%s", i,
                                        name ? name : L"?");
                                if (name) SysFreeString(name);
                                ComPtr<IUIAutomationCondition> trueCond;
                                ComPtr<IUIAutomationElementArray> allKids;
                                if (SUCCEEDED(uia->CreateTrueCondition(&trueCond)) &&
                                    SUCCEEDED(el->FindAll(TreeScope_Descendants,
                                                          trueCond.get(), &allKids)) &&
                                    allKids) {
                                    int ac = 0;
                                    allKids->get_Length(&ac);
                                    for (int k = 0; k < ac && k < 20; ++k) {
                                        ComPtr<IUIAutomationElement> kid;
                                        if (FAILED(allKids->GetElement(k, &kid)) || !kid) continue;
                                        BSTR kn = nullptr;
                                        kid->get_CurrentName(&kn);
                                        LogLine(L"[uia]   desc %d name=%s", k,
                                                kn ? kn : L"?");
                                        if (kn) SysFreeString(kn);
                                    }
                                }
                                ComPtr<IUIAutomationElementArray> kids;
                                if (SUCCEEDED(uia->CreateTrueCondition(&trueCond)) &&
                                    SUCCEEDED(el->FindAll(TreeScope_Children, trueCond.get(),
                                                          &kids)) &&
                                    kids) {
                                    int kc = 0;
                                    kids->get_Length(&kc);
                                    for (int k = 0; k < kc; ++k) {
                                        ComPtr<IUIAutomationElement> kid;
                                        if (FAILED(kids->GetElement(k, &kid)) || !kid) continue;
                                        BSTR kn = nullptr;
                                        kid->get_CurrentName(&kn);
                                        RECT kr{};
                                        kid->get_CurrentBoundingRectangle(&kr);
                                        LogLine(L"[uia]   child %d name=%s rect=%d,%d,%d,%d", k,
                                                kn ? kn : L"?", kr.left, kr.top, kr.right,
                                                kr.bottom);
                                        if (kn) SysFreeString(kn);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        view->Release();
    }
}

void ShellView::TogglePreviewPane() {
    // 以"窗格现在是否可见"为准取反；FOLDERFLAGS 没有预览窗格位，不能作为状态源。
    const bool visible = PreviewPaneVisibleNow(container_);
    previewPaneWanted_ = !visible;
    previewPaneWantedKnown_ = true;
    LogLine(L"[preview] toggle visible=%d -> wanted=%d", visible ? 1 : 0,
            previewPaneWanted_ ? 1 : 0);

    IShellView* view = CurrentView();
    ApplyPreviewPaneState(view);
    if (view) view->Release();
    SchedulePreviewReapply();

    // 用户显式切换 = 改变偏好：写回配置，下次启动/新标签沿用。
    Settings& s = Config::Get().mutableSettings();
    if (s.showPreviewPane != previewPaneWanted_) {
        s.showPreviewPane = previewPaneWanted_;
        Config::Get().Save();
    }
}

namespace {

// 导航窗格是宿主画的子窗口（ShellView 的同级/子级 NamespaceTreeControl），
// 用 EnumChildWindows 递归找，不用 FindWindowEx 逐层拼类名（NSE 视图形状不一）。
BOOL CALLBACK FindTreeProc(HWND h, LPARAM p) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"NamespaceTreeControl") == 0) {
        *reinterpret_cast<HWND*>(p) = h;
        return FALSE;
    }
    return TRUE;
}

HWND FindNavTree(HWND container) {
    HWND found = nullptr;
    if (container) EnumChildWindows(container, FindTreeProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

}  // namespace

void ShellView::ToggleNavPane() {
    if (!browser_) return;
    // EBO_SHOWFRAMES 只在下次导航被消费：导航后置位要再导航一次才出现，清零永不消失。
    // 所以真实显隐直接操作窗格子窗口，并自己记账。
    navPaneWanted_ = !navPaneWanted_;
    ApplyNavPaneWanted();
    // 用户显式切换 = 改变偏好：写回配置，下次启动/新标签沿用。
    Settings& s = Config::Get().mutableSettings();
    if (s.showNavPane != navPaneWanted_) {
        s.showNavPane = navPaneWanted_;
        Config::Get().Save();
    }
}

// 按"当前记账值"把导航窗格真实显隐落地（不写配置）。
void ShellView::ApplyNavPaneWanted() {
    if (!browser_) return;
    HWND tree = FindNavTree(container_);
    if (tree) ShowWindow(tree, navPaneWanted_ ? SW_SHOW : SW_HIDE);
    // 同步 IExplorerBrowser 的意图值，避免下次导航又把窗格画回来。
    EXPLORER_BROWSER_OPTIONS opts = EBO_NONE;
    if (SUCCEEDED(browser_->GetOptions(&opts))) {
        if (navPaneWanted_)
            opts = static_cast<EXPLORER_BROWSER_OPTIONS>(opts | EBO_SHOWFRAMES);
        else
            opts = static_cast<EXPLORER_BROWSER_OPTIONS>(opts & ~EBO_SHOWFRAMES);
        browser_->SetOptions(opts);
    }
    // 窗格占位变了，让 shell 视图重新算布局
    if (container_) InvalidateRect(container_, nullptr, TRUE);
}

bool ShellView::NavPaneVisible() const {
    // 读我们自己的状态，不读 GetOptions：那是"意图"，关过一次后永远和真实状态相反。
    return navPaneWanted_;
}

void ShellView::ApplyTheme() {
    if (!container_) return;
    const bool dark = theme_.dark;
    AllowDarkForWindow(container_, dark);
    SetWindowTheme(container_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    // 只深色化滚动条这类独立小控件；shell 内部窗口（DirectUI/DefView）一概不碰，
    // 否则选中高亮会被破坏（见 ApplyShellTheme 的实测数据）。
    EnumChildWindows(container_, ThemeScrollbarOnly, reinterpret_cast<LPARAM>(&dark));
    ApplyShellTheme(container_, dark);
    RedrawWindow(container_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

void ShellView::EraseBackground(HDC dc, const RECT& rc) {
    FillRect(dc, &rc, theme_.brBg ? theme_.brBg : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
}

void ShellView::OnViewCreated(IShellView* view) {
    viewReady_ = true;
    if (!view) return;
    HWND h = nullptr;
    if (SUCCEEDED(view->GetWindow(&h)) && h) {
        // 主题只作用于 ExplorerBrowserControl 这一层（见 ApplyShellTheme 注释）：
        // 给 shell 内部子窗口 SetWindowTheme 会让 DirectUI 丢掉选中高亮绘制。
        // 这里再补一步：滚动条是独立小控件，可以单独深色化。
        AllowDarkForWindow(h, theme_.dark);
        EnumChildWindows(h, ThemeScrollbarOnly, reinterpret_cast<LPARAM>(&theme_.dark));
        // Shell 视图每次导航都可能重建；把鼠标子类重新挂到新的 DefView/ListView 上。
        shellinput::Install(h, container_, path_, mouseSink_);
    }
    // ExplorerBrowserControl 层：这是唯一应该设主题的地方
    ApplyShellTheme(container_, theme_.dark);
    RedrawWindow(container_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    // 新目录/新视图的 folder flags 会恢复默认值；把用户期望的预览窗格状态追回去。
    ApplyPreviewPaneState(view);
    SchedulePreviewReapply();
    // 导航会重建 shell 视图，重建后选中态/焦点回到未激活态；若本标签仍是活动标签
    // （焦点在容器或其子窗口内），立即重新激活，避免"选中项变淡灰"。
    HWND focus = GetFocus();
    if (container_ && (focus == container_ || IsChild(container_, focus))) ActivateFocus();
}

void ShellView::OnNavigationFailed() {
    LogLine(L"[nav] failed path=%s", path_.c_str());
    if (failFn_) failFn_();
}

void ShellView::ReplayQueued() {
    KillTimer(container_, kNavReplayTimer);
    if (queued_.empty() || !browser_) return;
    std::wstring p = queued_;
    queued_.clear();
    if (p == path_) {
        replayTries_ = 0;  // 已经到位
        return;
    }
    // 兜底：慢/远程文件系统（WebDAV / 离线 OneDrive / 慢网络共享）的 BUSY 可能持续数十秒，
    // 退避 20 次仍失败 → 回滚 + 记日志，避免 pending 悬着变成"地址栏 A / 视图 B"。
    // 本地磁盘几乎走不到这里：WM_TIMER 在消息队列里排在 shell 的完成消息之后，
    // 重放执行时浏览器通常已不忙。
    if (++replayTries_ > 20) {
        replayTries_ = 0;
        abandonedPath_ = p;
        LogLine(L"[nav] abandon %s", p.c_str());
        OnNavigationFailed();  // 回滚，保证 App 立刻可用
        return;
    }
    LogLine(L"[nav] replay #%d %s", replayTries_, p.c_str());
    Navigate(p);  // 若再 BUSY 会按当前 replayTries_ 重新 SetTimer
}

void ShellView::OnNavigated(PCIDLIST_ABSOLUTE pidl) {
    std::wstring p = PidlName(pidl);
    LogLine(L"[nav] complete pidl=%s", p.c_str());
    if (!p.empty()) {
        path_ = p;
        shellinput::UpdateDir(container_, p);
    }

    // 命名空间（::{GUID}）的 parsing name 不适合当标题，另取 NORMALDISPLAY
    std::wstring display;
    PWSTR disp = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &disp)) && disp) {
        display = disp;
        CoTaskMemFree(disp);
    }
    if (display.empty()) display = LeafOf(p);
    display_ = display;

    if (!abandonedPath_.empty() && p == abandonedPath_) {
        LogLine(L"[nav] late-complete after abandon %s", p.c_str());
        abandonedPath_.clear();
    }
    replayTries_ = 0;

    // OnViewCreated 通常已经回写过；这里兜住"同一视图内换目录、没有重建 IShellView"
    // 的情况。即使此刻拿到的是旧视图，也只是把同一个期望值再写一次，无副作用。
    {
        IShellView* view = CurrentView();
        ApplyPreviewPaneState(view);
        if (view) view->Release();
    }
    SchedulePreviewReapply();

    // 目录切换后 shell 会按 EBO_SHOWFRAMES 重建窗格：按用户意图再校正一次，
    // 否则"关掉窗格"在换目录后又会回来。
    if (!navPaneWanted_) {
        if (HWND tree = FindNavTree(container_)) ShowWindow(tree, SW_HIDE);
    }

    // 不要在 OnNavigationComplete 里调 ItemCount()：此刻 GetCurrentView 还没指向
    // 新视图，实测恒为 0。计数交给状态栏的取数（RefreshStats）填。
    if (navFn_) navFn_(path_, display, 0);
}
