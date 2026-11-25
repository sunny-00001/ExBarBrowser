// ShellView 的 IExplorerBrowserEvents 接收器与导航完成/重放。
//
// Sink 的反向指针必须在 Unadvise 期间保持安全，所以 Sink 类定义与
// AdviseBrowser/DetachSink 放在同一个 TU；其他 TU 只通过 shell_view.h
// 里的前向声明调用，不会碰到 incomplete type。
#include "shell_view.h"

#include "path_util.h"
#include "shell_input.h"

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
    // InterlockedIncrement/Decrement 返回 LONG，COM 的 AddRef/Release 要 ULONG。
    // 引用计数不可能为负（多减一次就是 bug，届时 0xFFFFFFFF 也是"极大"，
    // 与 LONG 的解释无关），所以这里的位模式转换是安全的。
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = static_cast<ULONG>(InterlockedDecrement(&ref_));
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

bool ShellView::AdviseBrowser() {
    if (!browser_) return false;
    // 引用计数：new 得 1（我们的），Advise 让浏览器 AddRef 到 2，
    // 再 Release 交出自己的那份 → 只剩浏览器的 1 份。
    sink_ = new Sink(this);
    if (FAILED(browser_->Advise(sink_, &cookie_))) {
        cookie_ = 0;
        sink_->Release();
        sink_ = nullptr;
        return false;
    }
    sink_->Release();
    return true;
}

void ShellView::DetachSink() {
    if (sink_) sink_->Detach();
}

void ShellView::OnNavigationFailed() {
    LogDbg(L"[nav] failed path=%s", path_.c_str());
    if (failFn_) failFn_();
}

void ShellView::ReplayQueued() {
    KillTimer(container_, kNavReplayTimer);
    if (!browser_) return;
    // PIDL 形式（搜索导航）优先：它排队在前，先重放它。
    if (queuedPidl_) {
        PIDLIST_ABSOLUTE p = queuedPidl_;
        queuedPidl_ = nullptr;
        if (++replayTries_ > 20) {
            CoTaskMemFree(p);
            replayTries_ = 0;
            LogDbg(L"[nav] abandon pidl");
            OnNavigationFailed();
            return;
        }
        LogDbg(L"[nav] replay pidl #%d", replayTries_);
        NavigatePid(p);  // 再 BUSY 会重新排队
        CoTaskMemFree(p);
        return;
    }
    if (queued_.empty()) return;
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
        LogDbg(L"[nav] abandon %s", p.c_str());
        OnNavigationFailed();  // 回滚，保证 App 立刻可用
        return;
    }
    LogDbg(L"[nav] replay #%d %s", replayTries_, p.c_str());
    Navigate(p);  // 若再 BUSY 会按当前 replayTries_ 重新 SetTimer
}

void ShellView::OnNavigated(PCIDLIST_ABSOLUTE pidl) {
    std::wstring p = PidlName(pidl);
    LogDbg(L"[nav] complete pidl=%s", p.c_str());
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
        LogDbg(L"[nav] late-complete after abandon %s", p.c_str());
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
    //
    // 必须走 ApplyNavPaneWanted() 而不是裸 SW_HIDE：后者只让树窗口不可见，
    // Shell 的内部框架**不会重算布局**，树占的那一列宽度原样留着 —— 双栏下
    // 非活动窗格就会出现"左侧一大块空白、文件列表被挤到右边"（D4 同源问题）。
    // ApplyNavPaneWanted 会 SetRect 让框架重排，把宽度还给文件列表。
    if (!navPaneWanted_) ApplyNavPaneWanted();

    // 不要在 OnNavigationComplete 里调 ItemCount()：此刻 GetCurrentView 还没指向
    // 新视图，实测恒为 0。计数交给状态栏的取数（RefreshStats）填。
    if (navFn_) navFn_(path_, display, 0);
}
