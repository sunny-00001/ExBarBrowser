// ShellView 的搜索相关实现：按 PIDL 导航到原生搜索文件夹 + 结果视图默认详细信息。
//
// 【为什么单独成文件】这两件事只服务"搜索结果视图"这一条路，与目录导航、视图
// 模式、预览窗格都不是一回事；混在 shell_view.cpp 里会让那个文件继续涨
// （行数棘轮只许降不许涨，见 data/file-length-baseline.json）。
//
// 【搜索结果视图怎么来的】原生搜索文件夹（ISearchFolderItemFactory 造的 PIDL，
// 见 src/search_folder.h）是**真的文件夹**，但它的字符串形式是 search-ms: URL，
// 而 ShellView::DoNavigate 按前缀拒绝那个前缀 —— 于是搜索只能以 **PIDL** 形式
// 导航过去。那条拒绝判的是字符串前缀，不是文件夹类型。
//
// 同一套做法见 D:\Windhawk\exbar.cpp:26215 RunSystemSearchFolder。
#include "shell_view.h"

#include <shlobj.h>
#include <shobjidl.h>

#include "log.h"
#include "shell_ops.h"
#include "str.h"

// 按 PIDL 导航（原生搜索文件夹只能走这条路）。
bool ShellView::NavigatePid(PIDLIST_ABSOLUTE pidl) {
    if (!browser_ || !pidl) return false;
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromIDList(pidl, IID_IShellItem, reinterpret_cast<void**>(&item));
    if (FAILED(hr) || !item) {
        LogDbg(L"[nav] pidl→item hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    // 【必须带 SBSP_ABSOLUTE】不带这个标志时，shell 会把 PIDL 当成**当前文件夹的
    // 相对项**来解析 —— 当前文件夹此刻正是上一个搜索文件夹，于是新搜索被当成
    // "在旧搜索结果里再找一层"，结果集不刷新（换范围/换"仅本层"条件后看到的
    // 还是上一轮的结果）。exbar 那边传的是 SBSP_SAMEBROWSER | SBSP_ABSOLUTE
    // （D:\Windhawk\exbar.cpp:26276 BrowseObject），这里对齐。
    hr = browser_->BrowseToObject(item.get(), SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
    LogDbg(L"[nav] browse(pidl) hr=0x%08lX", static_cast<unsigned long>(hr));
    if (hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
        // 上一次浏览还没收尾：这是正常抖动，不是失败。记下这次的目标，
        // 用导航重放定时器退避重试（与字符串路径同一套路）。
        if (queuedPidl_) CoTaskMemFree(queuedPidl_);
        queuedPidl_ = ILCloneFull(pidl);
        if (container_) {
            static const UINT kDelay[] = {50, 100, 200, 500, 1000};
            UINT d = kDelay[replayTries_ < 5 ? replayTries_ : 4];
            SetTimer(container_, kNavReplayTimer, d, nullptr);
        }
        return true;
    }
    return FAILED(hr) ? false : true;
}

// 搜索结果视图默认详细信息。
//
// 【为什么要等】原生搜索文件夹的视图是**异步**创建的：导航完成那一刻
// GetCurrentView 还指着旧视图，此刻设视图模式会把用户原目录的视图误改成
// 详细信息。所以挂定时器，等新视图就位再补设（每 120ms 一拍，最多 ~3 秒）。
void ShellView::EnsureSearchDetailsView(PIDLIST_ABSOLUTE searchPidl) {
    if (!container_) return;
    if (searchPidl_) CoTaskMemFree(searchPidl_);
    searchPidl_ = searchPidl ? ILCloneFull(searchPidl) : nullptr;
    searchViewTicks_ = 0;
    SetTimer(container_, kSearchViewTimer, 120, nullptr);
}

void ShellView::OnSearchViewTimer() {
    IShellView* view = CurrentView();
    ComPtr<IFolderView2> fv2 = shellops::FolderView2(view);
    if (view) view->Release();
    if (!fv2) return;  // 视图还没建好，下一拍再来

    // 判据：活动视图的当前目录就是我们导航到的那个搜索文件夹。
    // 两条任一成立即可：
    //   · 解析名一致（我们的 PIDL 与视图当前目录指向同一项）
    //   · 解析名以 "search-ms:" 开头（shell 规范化后的搜索文件夹解析名）
    bool isSearchView = false;
    ComPtr<IPersistFolder2> pf;
    if (SUCCEEDED(fv2->GetFolder(IID_IPersistFolder2, reinterpret_cast<void**>(&pf))) && pf) {
        PIDLIST_ABSOLUTE cur = nullptr;
        if (SUCCEEDED(pf->GetCurFolder(&cur)) && cur) {
            PWSTR nm = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(cur, SIGDN_DESKTOPABSOLUTEPARSING, &nm)) && nm) {
                isSearchView = (wcsncmp(nm, L"search-ms:", 10) == 0);
                if (!isSearchView && searchPidl_) {
                    PWSTR mine = nullptr;
                    if (SUCCEEDED(SHGetNameFromIDList(searchPidl_, SIGDN_DESKTOPABSOLUTEPARSING,
                                                     &mine)) &&
                        mine) {
                        isSearchView = str::IEquals(nm, mine);
                        CoTaskMemFree(mine);
                    }
                }
                CoTaskMemFree(nm);
            }
            CoTaskMemFree(cur);
        }
    }

    if (!isSearchView) {
        if (++searchViewTicks_ > 25) {  // ~3 秒还没就位：放弃，别无限等
            LogDbg(L"[search] 详情视图：等不到搜索结果视图，放弃");
            KillTimer(container_, kSearchViewTimer);
            if (searchPidl_) {
                CoTaskMemFree(searchPidl_);
                searchPidl_ = nullptr;
            }
        }
        return;
    }

    KillTimer(container_, kSearchViewTimer);
    if (searchPidl_) {
        CoTaskMemFree(searchPidl_);
        searchPidl_ = nullptr;
    }
    // 默认详细信息：搜索结果的视图模板默认是"内容视图"（大图标内容），
    // 不是列表。图标尺寸跟本工程"详细信息"档一致（16）。
    HRESULT hr = fv2->SetViewModeAndIconSize(FVM_DETAILS, 16);
    FOLDERVIEWMODE got = FVM_AUTO;
    int icon = 0;
    fv2->GetViewModeAndIconSize(&got, &icon);
    LogDbg(L"[search] 详情视图 hr=0x%08lX now=%d icon=%d", static_cast<unsigned long>(hr),
           static_cast<int>(got), icon);
}
