// ShellView 的浏览器生命周期：销毁、主题切换后的重建。
//
// 【为什么单独成文件】深浅色切换的真实修法必须重建 IExplorerBrowser：
// SetPreferredAppMode 是进程级开关，只对**新建**的 shell 窗口生效；旧 EBC
// 补发 WM_THEMECHANGED、再 SetWindowTheme 或 Refresh 都不会换色（离屏与运行时
// 都实测过）。这段只管"怎么把浏览器安全拆掉再按当前主题建回来"，导航与
// 视图状态恢复都走既有入口，不在这里另写一份。
#include "shell_view.h"

#include <shlobj.h>
#include <shobjidl.h>

#include "log.h"
#include "platform_win.h"
#include "shell_input.h"
#include "shell_view_helpers.h"

void ShellView::DestroyBrowser() {
    if (container_) {
        // 浏览器一拆，挂在旧 DirectUI 上的鼠标子类和排队重放都必须一起清掉，
        // 否则新 EBC 建好前定时器会摸到已经 Release 的 browser_。
        KillTimer(container_, kNavReplayTimer);
        KillTimer(container_, kPreviewReapplyTimer);
        KillTimer(container_, kSearchViewTimer);
        KillTimer(container_, kThemeRestoreTimer);
        shellinput::Uninstall(container_);
    }
    if (queuedPidl_) {
        CoTaskMemFree(queuedPidl_);
        queuedPidl_ = nullptr;
    }
    if (searchPidl_) {
        CoTaskMemFree(searchPidl_);
        searchPidl_ = nullptr;
    }
    queued_.clear();
    abandonedPath_.clear();
    replayTries_ = 0;
    searchViewTicks_ = 0;
    viewReady_ = false;

    if (browser_) {
        // 与 Destroy() 同样的顺序：先断站点链，避免销毁过程中回调 GetPaneState
        // 摸到将死对象；Unadvise 会释放浏览器持有的最后一份 Sink 引用。
        IUnknown_SetSite(browser_, nullptr);
        if (sink_) {
            DetachSink();
            if (cookie_ && browser_) browser_->Unadvise(cookie_);
            cookie_ = 0;
            sink_ = nullptr;
        }
        browser_->Destroy();
        browser_->Release();
        browser_ = nullptr;
    }
}

void ShellView::SuspendForThemeSwitch() {
    if (themeSuspended_ || !container_ || !browser_ || path_.empty()) return;
    const ULONGLONG t0 = GetTickCount64();
    themeSuspended_ = true;

    // 在拆 EBC 之前采样。DestroyBrowser() 会释放 searchPidl_，所以搜索文件夹
    // 必须 clone 出来；path_ 属于 ShellView 自己，DestroyBrowser 不会清。
    GetClientRect(container_, &themePendingRect_);
    if (themePendingSearchPidl_) {
        CoTaskMemFree(themePendingSearchPidl_);
        themePendingSearchPidl_ = nullptr;
    }
    themePendingSearchPidl_ = searchPidl_ ? ILCloneFull(searchPidl_) : nullptr;

    themeRestoreSelection_.clear();
    std::vector<SelectionItem> selected;
    if (SelectionPaths(512, selected) > 0) {
        for (const SelectionItem& item : selected)
            themeRestoreSelection_.push_back(item.path);
    }
    // 优先用 PIDL 回灌；路径列表只作诊断/兜底（SHParseDisplayName 会漏特殊项）。
    CaptureSelectionPidls(512, themeRestorePidls_);
    int fvm = -1;
    int icon = 0;
    GetViewMode(&fvm, &icon);
    themePendingFvm_ = fvm;
    themePendingIcon_ = icon;
    themeRestoreTries_ = 0;

    LogDbg(L"[theme] SuspendForThemeSwitch path=%s dark=%d savedSel=%d/%d fvm=%d icon=%d ms=%llu",
           path_.c_str(), theme_.dark ? 1 : 0,
           static_cast<int>(themeRestorePidls_.size()),
           static_cast<int>(themeRestoreSelection_.size()),
           themePendingFvm_, themePendingIcon_, GetTickCount64() - t0);
    // 关键：进程模式还没改，先把所有旧的 DirectUI/EBC 窗口拆干净。
    DestroyBrowser();
}

void ShellView::FinishThemeSwitch() {
    if (!themeSuspended_) return;
    // 实测保留旧容器并不会更快（1.3~1.4s vs 0.9~1.0s）：旧 DirectUI 状态
    // 会让新 EBC 做更多清理。连容器一起重建仍是最快且最稳的路径。
    RebuildBrowserForTheme(true);
    // 主题重建才需要全子树擦除重绘；普通导航由 OnViewCreated 的轻量失效即可。
    if (container_)
        RedrawWindow(container_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

bool ShellView::RebuildBrowserForTheme(bool recreateContainer) {
    const ULONGLONG t0 = GetTickCount64();
    const bool suspended = themeSuspended_;
    LogDbg(L"[theme] RebuildBrowserForTheme ENTER path=%s dark=%d recreate=%d suspended=%d",
           path_.c_str(), theme_.dark ? 1 : 0, recreateContainer ? 1 : 0,
           suspended ? 1 : 0);
    // 容器窗口也要重建：只重建 EBC 时，旧容器上留下过 AllowDarkModeForWindow(true)
    // 的窗口状态，ForceDark -> ForceLight 后新建的 DirectUI 仍按深色画（实测）。
    // 真正"重启就好"的差异就在这一层；因此这里连容器一起换掉，保持它的样式、
    // 位置、Z 序和可见性，调用方随后照常 Layout()。
    HWND parent = nullptr;
    HWND insertAfter = nullptr;
    LONG_PTR style = 0;
    DWORD exStyle = 0;
    RECT containerRect{};
    BOOL visible = FALSE;
    bool focusInside = false;
    const bool doRecreate = recreateContainer && container_ && (parent = GetParent(container_));
    if (doRecreate) {
        style = GetWindowLongPtrW(container_, GWL_STYLE);
        exStyle = static_cast<DWORD>(GetWindowLongPtrW(container_, GWL_EXSTYLE));
        GetWindowRect(container_, &containerRect);
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&containerRect), 2);
        insertAfter = GetNextWindow(container_, GW_HWNDNEXT);
        visible = IsWindowVisible(container_);
        HWND focus = GetFocus();
        focusInside = focus && (focus == container_ || IsChild(container_, focus));
    }

    // 两阶段切换时，状态已在 SuspendForThemeSwitch() 中采样，且浏览器已拆掉；
    // 普通重建路径仍在这里即时采样，再自行 DestroyBrowser()。
    RECT rc{};
    if (suspended) {
        rc = themePendingRect_;
    } else if (container_) {
        GetClientRect(container_, &rc);
    }
    const std::wstring target = path_;
    PIDLIST_ABSOLUTE search = nullptr;
    if (suspended) {
        // 接管 Suspend 时的 clone；下面无论成功失败都由本函数释放。
        search = themePendingSearchPidl_;
        themePendingSearchPidl_ = nullptr;
    } else {
        search = searchPidl_ ? ILCloneFull(searchPidl_) : nullptr;
        themeRestoreSelection_.clear();
        std::vector<SelectionItem> selected;
        if (SelectionPaths(512, selected) > 0) {
            for (const SelectionItem& item : selected)
                themeRestoreSelection_.push_back(item.path);
        }
        CaptureSelectionPidls(512, themeRestorePidls_);
        int fvm = -1;
        int icon = 0;
        GetViewMode(&fvm, &icon);
        themePendingFvm_ = fvm;
        themePendingIcon_ = icon;
    }

    auto fail = [&](const wchar_t* what) {
        LogDbg(L"[theme] 重建 Shell 视图失败（%s）", what);
        if (search) CoTaskMemFree(search);
        if (themePendingSearchPidl_) CoTaskMemFree(themePendingSearchPidl_);
        search = nullptr;
        themePendingSearchPidl_ = nullptr;
        themeRestoreSelection_.clear();
        ClearThemeRestorePidls();
        themePendingFvm_ = -1;
        themePendingIcon_ = 0;
        themeSuspended_ = false;
        // 【必须给一条出路】doRecreate 路径先 DestroyWindow(container_)；若随后的
        // CreateWindowEx 失败，container_ 已是 nullptr —— 此后 Resize / SetVisible /
        // ApplyNavPaneWanted / Destroy 全部早退，界面是一块永不重绘的空白，
        // 日志之外无人知晓。这里重建一个**最小容器**（不带 EBC，但能挂导航与重试），
        // 并通知 App 走既有的失败通道（它可以提示/重建标签），不留无声空白。
        if (!container_ && parent) {
            container_ = CreateWindowExW(
                0, kTabViewClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                containerRect.left, containerRect.top, containerRect.right - containerRect.left,
                containerRect.bottom - containerRect.top, parent, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (container_) {
                SetThis(container_, this);
                AllowDarkForWindow(container_, theme_.dark);
                LogDbg(L"[theme] 已用最小容器兜底（视图待下次导航重建）");
            }
        }
        if (!container_ && failFn_) failFn_();   // 连容器都建不起来：交给 App 处理
    };

    if (!suspended) DestroyBrowser();
    if (doRecreate && container_) {
        DestroyWindow(container_);
        container_ = nullptr;
        container_ = CreateWindowExW(exStyle, kTabViewClass, L"", static_cast<DWORD>(style),
                                     containerRect.left, containerRect.top,
                                     containerRect.right - containerRect.left,
                                     containerRect.bottom - containerRect.top,
                                     parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!container_) {
            fail(L"CreateWindowEx");
            return false;
        }
        SetThis(container_, this);
        // 与 ShellView::Create 相同的顺序：先放行深浅色，再建 EBC。
        const bool dark = theme_.dark;
        AllowDarkForWindow(container_, dark);
        SetWindowTheme(container_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        if (visible) ShowWindow(container_, SW_SHOWNA);
        SetWindowPos(container_, insertAfter ? insertAfter : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (focusInside) SetFocus(container_);
    }
    if (!InitBrowser()) {
        fail(L"InitBrowser");
        return false;
    }
    if (!AdviseBrowser()) {
        fail(L"AdviseBrowser");
        return false;
    }
    // InitBrowser 只建 10x10 的初始矩形；重建后必须按容器当前尺寸落地，
    // 否则 App::Layout 的 ShellView::Resize 会因"矩形没变"直接跳过 SetRect。
    if (browser_ && rc.right > rc.left && rc.bottom > rc.top) browser_->SetRect(nullptr, rc);

    bool ok = false;
    if (search) {
        // 搜索结果文件夹只能按 PIDL 导航（字符串是 search-ms: URL，会被拒绝）。
        ok = NavigatePid(search);
        if (ok) EnsureSearchDetailsView(search);  // 内部 clone，外部仍持有 search
        CoTaskMemFree(search);
        search = nullptr;
    } else {
        ok = Navigate(target);
    }
    if (!ok) {
        LogDbg(L"[theme] 重建 Shell 视图失败：%s", target.c_str());
        themeRestoreSelection_.clear();
        ClearThemeRestorePidls();
        themePendingFvm_ = -1;
        themePendingIcon_ = 0;
    }
    if (themePendingSearchPidl_) {
        CoTaskMemFree(themePendingSearchPidl_);
        themePendingSearchPidl_ = nullptr;
    }
    themeSuspended_ = false;
    LogDbg(L"[theme] RebuildBrowserForTheme done path=%s ok=%d ms=%llu",
           path_.c_str(), ok ? 1 : 0, GetTickCount64() - t0);
    return ok;
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
namespace {

void ApplyShellTheme(HWND container, bool dark) {
    if (!container) return;
    // 只戳 IExplorerBrowser 自己的宿主窗口（container 直接子窗口中的那一个）。
    if (HWND ebw = FindChildByClass(container, L"ExplorerBrowserControl")) {
        AllowDarkForWindow(ebw, dark);
        // 这里刻意不走 win::ThemeControl：那是个"给我们的原生控件套主题"的入口，
        // 而这个窗口是 Shell 的 DirectUI 宿主，套法有额外约束（见上面的实测记录）。
        // 行为上两者等价，分开是为了让"哪些窗口归我们管"一眼可辨。
        // 【浅色必须传 NULL 复位】传 L"Explorer" 不会清掉先前 SetWindowTheme 塞进
        // 窗口的 SUBAPPNAME 覆盖：DirectUI 仍按 DarkMode_Explorer 的字典画，表现是
        // dark->light 后文件区留在深色。NULL 才是"回到类默认视觉样式"。
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

}  // namespace

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
    // 【进程级深浅色只对新建的 EBC 生效】实测：切完主题后给两个 EBC 补发
    // WM_THEMECHANGED、重刷 SetWindowTheme、Refresh 都没有让文件区换色；重启
    // （= 重新创建 EBC）立刻正确。所以这里必须重建浏览器，旧容器与 Pane 状态留住。
    const bool changed = dark != (themeDarkApplied_ != 0);
    themeDarkApplied_ = dark ? 1 : 0;
    // 两阶段切换：App::ApplyTheme 已经让本视图拆掉 EBC，这里只重建。
    // 不能再用 browser_ 判空 —— 那会让等待中的重建永远不发生。
    if (themeSuspended_) {
        FinishThemeSwitch();
    } else if (changed && browser_ && !path_.empty()) {
        // 返回值必须看：false = 重建失败（容器可能已被销毁）。fail() 内部已经
        // 做了最小容器兜底 / 失败回调，这里再记一句，避免"界面空着但日志安静"。
        if (!RebuildBrowserForTheme(true))
            LogDbg(L"[theme] 主题重建失败：视图已降级，等待下次导航重试");
    }
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
        // 返回值必须看：false = 一个窗口都没挂上（Win 版本层级变化），
        // 此时鼠标规则会整体静默失效，只在日志里留下一句明确原因。
        if (!shellinput::Install(h, container_, path_, mouseSink_))
            LogDbg(L"[nav] 鼠标规则安装失败：视图层级与预期不符（版本变化？）");
    }
    // ExplorerBrowserControl 层：这是唯一应该设主题的地方
    ApplyShellTheme(container_, theme_.dark);
    // 普通导航/视图重建不做全子树擦除重绘（RDW_ALLCHILDREN|RDW_ERASE 会让
    // DirectUI 同步重排整棵子树）。主题切换的全量重绘由 FinishThemeSwitch 负责。
    if (container_) InvalidateRect(container_, nullptr, FALSE);
    // 主题重建后的状态回灌：OnViewCreated 比 GetCurrentView 就绪更早，
    // 这里先试一次，不成就挂短定时器重试（见 RestoreThemeState）。
    if (themePendingFvm_ >= 0 || !themeRestorePidls_.empty() ||
        !themeRestoreSelection_.empty()) {
        if (!RestoreThemeState() && container_)
            SetTimer(container_, kThemeRestoreTimer, 60, nullptr);
    }
    // 新目录/新视图的 folder flags 会恢复默认值；把用户期望的预览窗格状态追回去。
    ApplyPreviewPaneState(view);
    SchedulePreviewReapply();
    // 导航会重建 shell 视图，重建后选中态/焦点回到未激活态；若本标签仍是活动标签
    // （焦点在容器或其子窗口内），立即重新激活，避免"选中项变淡灰"。
    HWND focus = GetFocus();
    if (container_ && (focus == container_ || IsChild(container_, focus))) ActivateFocus();
}

bool ShellView::RestoreThemeState() {
    if (!browser_ || !container_) return true;
    auto selectionPending = [this] {
        return !themeRestorePidls_.empty() || !themeRestoreSelection_.empty();
    };
    if (themePendingFvm_ < 0 && !selectionPending()) {
        KillTimer(container_, kThemeRestoreTimer);
        return true;
    }

    // 实测 OnViewCreated 到达时 GetCurrentView 仍可能为空；此时直接回灌选中
    // 会静默丢弃全部待恢复项。先探一次，没就绪就让定时器下一拍再来。
    IShellView* probe = CurrentView();
    const bool viewReady = probe != nullptr;
    if (probe) probe->Release();
    if (!viewReady) {
        LogDbg(L"[theme] restore deferred: current view not ready (try=%d)",
               themeRestoreTries_ + 1);
    } else {
        if (themePendingFvm_ >= 0) {
            LogDbg(L"[theme] restore view mode fvm=%d icon=%d", themePendingFvm_,
                   themePendingIcon_);
            SetViewMode(themePendingFvm_, themePendingIcon_);
            themePendingFvm_ = -1;
            themePendingIcon_ = 0;
        }
        if (!themeRestorePidls_.empty()) {
            LogDbg(L"[theme] restore selection pidl=%d",
                   static_cast<int>(themeRestorePidls_.size()));
            SelectPidls(themeRestorePidls_);
            ClearThemeRestorePidls();
            themeRestoreSelection_.clear();
        } else if (!themeRestoreSelection_.empty()) {
            LogDbg(L"[theme] restore selection path=%d",
                   static_cast<int>(themeRestoreSelection_.size()));
            SelectPaths(themeRestoreSelection_);
            themeRestoreSelection_.clear();
        }
    }

    const bool done = themePendingFvm_ < 0 && !selectionPending();
    if (done) {
        KillTimer(container_, kThemeRestoreTimer);
        themeRestoreTries_ = 0;
        return true;
    }
    // 60ms × 20 = 1.2s：覆盖一次慢导航，又不会在 shell 卡住时无限重投。
    if (++themeRestoreTries_ >= 20) {
        LogDbg(L"[theme] restore abandoned after %d tries (fvm=%d sel=%d)",
               themeRestoreTries_, themePendingFvm_,
               static_cast<int>(themeRestorePidls_.size() + themeRestoreSelection_.size()));
        themeRestoreSelection_.clear();
        ClearThemeRestorePidls();
        themePendingFvm_ = -1;
        themePendingIcon_ = 0;
        themeRestoreTries_ = 0;
        KillTimer(container_, kThemeRestoreTimer);
    }
    return false;
}
