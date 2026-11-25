// ShellView 的选中项观测与恢复。
//
// 路径快照与 PIDL 快照放在一起：前者服务会话恢复/AI 观测，后者服务主题重建
// （虚拟项没有稳定路径，必须用 shell 自己的身份）。两者共用 CurrentView()，
// 口径一致，不另写一份枚举。
#include "shell_view.h"

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

unsigned ShellView::SelectionPaths(unsigned cap, std::vector<SelectionItem>& out) {
    out.clear();
    unsigned total = SelectedCount();   // 总数同源，用于截断判定
    if (total == 0 || cap == 0) return total;

    IShellView* view = CurrentView();   // 返回已 AddRef，本函数负责 Release
    if (!view) return 0;
    IFolderView* fv = nullptr;
    if (SUCCEEDED(view->QueryInterface(IID_IFolderView, reinterpret_cast<void**>(&fv))) && fv) {
        // Items() 交出的是**相对当前 folder 的子 PIDL**。以前用
        // SHCreateItemFromIDList 解析，等于按桌面根拼接：在 E:\x 里选中 a 会被
        // 读成 C:\Users\<me>\Desktop\a —— 文件名对、父目录错，且错得不容易看出来。
        // 必须带上父 folder 解析（与 StatItem 同一条口径）。
        IShellFolder* folder = nullptr;
        fv->GetFolder(IID_IShellFolder, reinterpret_cast<void**>(&folder));
        IEnumIDList* en = nullptr;
        if (SUCCEEDED(fv->Items(SVGIO_SELECTION, IID_IEnumIDList,
                                reinterpret_cast<void**>(&en))) && en) {
            out.reserve(total < cap ? total : cap);
            for (unsigned taken = 0; taken < cap; ++taken) {
                PITEMID_CHILD pidl = nullptr;
                if (en->Next(1, &pidl, nullptr) != S_OK || !pidl) break;
                IShellItem* item = nullptr;
                if (folder &&
                    SUCCEEDED(SHCreateItemWithParent(nullptr, folder, pidl, IID_IShellItem,
                                                     reinterpret_cast<void**>(&item))) && item) {
                    SelectionItem si;
                    PWSTR name = nullptr;
                    // DESKTOPABSOLUTEEDITING：文件系统给完整路径（C:\a\b），
                    // 命名空间给解析名（::{GUID}\子项）；比 PARSING 少一层显示转义。
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEEDITING, &name)) && name) {
                        si.path = name;
                        CoTaskMemFree(name);
                    }
                    SFGAOF attr = 0;
                    item->GetAttributes(SFGAO_FOLDER, &attr);
                    si.isFolder = (attr & SFGAO_FOLDER) != 0;
                    if (!si.path.empty()) out.push_back(std::move(si));
                    item->Release();
                }
                CoTaskMemFree(pidl);
            }
            en->Release();
        }
        if (folder) folder->Release();
        fv->Release();
    }
    view->Release();
    return total;
}

void ShellView::SelectPaths(const std::vector<std::wstring>& paths) {
    if (paths.empty() || !browser_) return;
    IShellView* view = CurrentView();
    if (!view) {
        LogDbg(L"[sel] restore aborted: no current view (%d)", static_cast<int>(paths.size()));
        return;
    }
    IFolderView* fv = nullptr;
    if (FAILED(view->QueryInterface(IID_IFolderView, reinterpret_cast<void**>(&fv))) || !fv) {
        LogDbg(L"[sel] restore aborted: no IFolderView (%d)", static_cast<int>(paths.size()));
        view->Release();
        return;
    }
    // SelectItem 要**相对当前 folder 的子 pidl**；SHParseDisplayName 给绝对 pidl。
    // 由 IPersistFolder2::GetCurPIDL 拿 folder 绝对 pidl，再 ILFindChild 切出子段
    // （返回值指向 abs 内部，不单独释放）。条目不存在/目录已变 → 解析失败静默跳过。
    IPersistFolder2* pf = nullptr;
    PIDLIST_ABSOLUTE folder = nullptr;
    if (SUCCEEDED(fv->GetFolder(IID_IPersistFolder2, reinterpret_cast<void**>(&pf))) && pf) {
        pf->GetCurFolder(&folder);   // MinGW 的 IPersistFolder2 方法名（SDK 叫 GetCurPIDL）
        pf->Release();
    }
    if (!folder) {
        LogDbg(L"[sel] restore aborted: no folder pidl (%d)", static_cast<int>(paths.size()));
        fv->Release();
        view->Release();
        return;
    }
    int restored = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        PIDLIST_ABSOLUTE abs = nullptr;
        if (FAILED(SHParseDisplayName(paths[i].c_str(), nullptr, &abs, 0, nullptr)) || !abs)
            continue;
        const PCUITEMID_CHILD child = ILFindChild(folder, abs);
        if (child) {
            // 首条清掉旧选中；后续追加。失败静默：恢复是尽力语义。
            const UINT flags = (i == 0) ? (SVSI_SELECT | SVSI_DESELECTOTHERS) : SVSI_SELECT;
            if (SUCCEEDED(view->SelectItem(child, flags))) ++restored;
        }
        ILFree(abs);
    }
    LogDbg(L"[sel] restored %d/%d", restored, static_cast<int>(paths.size()));
    ILFree(folder);
    fv->Release();
    view->Release();
}

void ShellView::ClearThemeRestorePidls() {
    for (PIDLIST_ABSOLUTE pidl : themeRestorePidls_) ILFree(pidl);
    themeRestorePidls_.clear();
}

unsigned ShellView::CaptureSelectionPidls(unsigned cap,
                                          std::vector<PIDLIST_ABSOLUTE>& out) {
    ClearThemeRestorePidls();
    if (!browser_ || cap == 0) return 0;
    IShellView* view = CurrentView();
    if (!view) return 0;
    IFolderView* fv = nullptr;
    if (FAILED(view->QueryInterface(IID_IFolderView, reinterpret_cast<void**>(&fv))) || !fv) {
        view->Release();
        return 0;
    }
    IPersistFolder2* pf = nullptr;
    PIDLIST_ABSOLUTE folder = nullptr;
    if (SUCCEEDED(fv->GetFolder(IID_IPersistFolder2, reinterpret_cast<void**>(&pf))) && pf) {
        pf->GetCurFolder(&folder);
        pf->Release();
    }
    if (!folder) {
        fv->Release();
        view->Release();
        return 0;
    }
    IEnumIDList* en = nullptr;
    if (SUCCEEDED(fv->Items(SVGIO_SELECTION, IID_IEnumIDList,
                            reinterpret_cast<void**>(&en))) &&
        en) {
        for (unsigned taken = 0; taken < cap; ++taken) {
            PITEMID_CHILD child = nullptr;
            if (en->Next(1, &child, nullptr) != S_OK || !child) break;
            // Items() 给的是相对当前文件夹的 child PIDL；与 folder 拼成绝对路径。
            // ILCombine 返回新分配的 PIDL，直接交给 out 持有。
            PIDLIST_ABSOLUTE abs = ILCombine(folder, child);
            if (abs) out.push_back(abs);
            CoTaskMemFree(child);
        }
        en->Release();
    }
    ILFree(folder);
    fv->Release();
    view->Release();
    return static_cast<unsigned>(out.size());
}

void ShellView::SelectPidls(const std::vector<PIDLIST_ABSOLUTE>& pidls) {
    if (pidls.empty() || !browser_) return;
    IShellView* view = CurrentView();
    if (!view) return;
    IFolderView* fv = nullptr;
    if (FAILED(view->QueryInterface(IID_IFolderView, reinterpret_cast<void**>(&fv))) || !fv) {
        view->Release();
        return;
    }
    IPersistFolder2* pf = nullptr;
    PIDLIST_ABSOLUTE folder = nullptr;
    if (SUCCEEDED(fv->GetFolder(IID_IPersistFolder2, reinterpret_cast<void**>(&pf))) && pf) {
        pf->GetCurFolder(&folder);
        pf->Release();
    }
    if (!folder) {
        fv->Release();
        view->Release();
        return;
    }
    int restored = 0;
    for (size_t i = 0; i < pidls.size(); ++i) {
        const PCUITEMID_CHILD child = ILFindChild(folder, pidls[i]);
        if (child) {
            const UINT flags = (i == 0) ? (SVSI_SELECT | SVSI_DESELECTOTHERS) : SVSI_SELECT;
            if (SUCCEEDED(view->SelectItem(child, flags))) ++restored;
        }
    }
    LogDbg(L"[sel] restored pidl %d/%d", restored, static_cast<int>(pidls.size()));
    ILFree(folder);
    fv->Release();
    view->Release();
}
