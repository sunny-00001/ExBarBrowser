// Shell 视图的查询/设置作用域：排序/分组、隐藏项开关与读面、只读取数。
//
// 与 shell_ops.cpp 的"文件操作作用域"分开：那边的入参是动作（删除/重命名/选中…），
// 这边的入参是查询条件或视图设置，只读或只改视图显示状态，不落盘操作用户文件。
// 两边共用的 QI/路径辅助只有一份，见 shell_ops_internal.h。
#include "shell_ops.h"

#include <algorithm>

#include "shell_ops_internal.h"
#include "shell_ops_path.h"
#include "view_stats.h"

namespace shellops {

bool SetSortByKey(IShellView* view, const PROPERTYKEY& key, int ascending) {
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;
    int dir = 1;
    if (ascending >= 0) {
        dir = ascending ? 1 : -1;
    } else {
        int cols = 0;
        if (SUCCEEDED(fv2->GetSortColumnCount(&cols)) && cols > 0) {
            cols = std::min(cols, 8);
            std::vector<SORTCOLUMN> cur(static_cast<size_t>(cols));
            if (SUCCEEDED(fv2->GetSortColumns(cur.data(), cols)) &&
                IsEqualGUID(cur[0].propkey.fmtid, key.fmtid) && cur[0].propkey.pid == key.pid &&
                cur[0].direction > 0)
                dir = -1;
        }
    }
    SORTCOLUMN col{};
    col.propkey = key;
    col.direction = dir;
    return SUCCEEDED(fv2->SetSortColumns(&col, 1));
}

bool SetSort(IShellView* view, int mode, int ascending) {
    const PROPERTYKEY* key = &PKEY_ItemNameDisplay;
    switch (mode) {
    case 1: key = &PKEY_DateModified; break;
    case 2: key = &PKEY_ItemTypeText; break;
    case 3: key = &PKEY_Size; break;
    default: break;
    }
    return SetSortByKey(view, *key, ascending);
}

bool GetSort(IShellView* view, int* mode, bool* ascending) {
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;
    int cols = 0;
    if (FAILED(fv2->GetSortColumnCount(&cols)) || cols <= 0) return false;
    cols = std::min(cols, 8);
    std::vector<SORTCOLUMN> cur(static_cast<size_t>(cols));
    int got = cols;
    if (FAILED(fv2->GetSortColumns(cur.data(), got)) || got <= 0) return false;

    // 只认本工程能表达的四列；其它列返回 false，让调用方保留原配置。
    const PROPERTYKEY& k = cur[0].propkey;
    int m = -1;
    if (IsEqualGUID(k.fmtid, PKEY_ItemNameDisplay.fmtid) && k.pid == PKEY_ItemNameDisplay.pid)
        m = 0;
    else if (IsEqualGUID(k.fmtid, PKEY_DateModified.fmtid) && k.pid == PKEY_DateModified.pid)
        m = 1;
    else if (IsEqualGUID(k.fmtid, PKEY_ItemTypeText.fmtid) && k.pid == PKEY_ItemTypeText.pid)
        m = 2;
    else if (IsEqualGUID(k.fmtid, PKEY_Size.fmtid) && k.pid == PKEY_Size.pid)
        m = 3;
    if (m < 0) return false;

    if (mode) *mode = m;
    if (ascending) *ascending = cur[0].direction >= 0;
    return true;
}

bool GetSortKey(IShellView* view, PROPERTYKEY* out, bool* ascending) {
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;
    int cols = 0;
    if (FAILED(fv2->GetSortColumnCount(&cols)) || cols <= 0) return false;
    cols = std::min(cols, 8);
    std::vector<SORTCOLUMN> cur(static_cast<size_t>(cols));
    int got = cols;
    if (FAILED(fv2->GetSortColumns(cur.data(), got)) || got <= 0) return false;
    if (out) *out = cur[0].propkey;
    if (ascending) *ascending = cur[0].direction >= 0;
    return true;
}

bool SetGroupByKey(IShellView* view, const PROPERTYKEY& key) {
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;
    return SUCCEEDED(fv2->SetGroupBy(key, TRUE));
}

bool GetGroupKey(IShellView* view, PROPERTYKEY* out) {
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;
    BOOL asc = FALSE;
    PROPERTYKEY k{};
    if (FAILED(fv2->GetGroupBy(&k, &asc))) return false;
    if (out) *out = k;
    return true;
}

bool SetGroup(IShellView* view, int mode) {
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;
    HRESULT hr = S_OK;
    if (mode == 0) {
        PROPERTYKEY none{};
        hr = fv2->SetGroupBy(none, FALSE);
    } else {
        const PROPERTYKEY* key = &PKEY_ItemTypeText;
        if (mode == 2) key = &PKEY_DateModified;
        if (mode == 3) key = &PKEY_Size;
        hr = fv2->SetGroupBy(*key, TRUE);
    }
    return SUCCEEDED(hr);
}

bool ToggleHidden(IShellView* view, int which, HWND owner) {
    (void)owner;
    SHELLSTATE ss{};
    SHGetSetSettings(&ss, detail::HiddenFlags(), FALSE);
    if (which == 0)
        ss.fShowAllObjects = ss.fShowAllObjects ? FALSE : TRUE;
    else if (which == 1)
        ss.fShowSuperHidden = ss.fShowSuperHidden ? FALSE : TRUE;
    else
        ss.fShowExtensions = ss.fShowExtensions ? FALSE : TRUE;
    SHGetSetSettings(&ss, detail::HiddenFlags(), TRUE);
    // SHCNE_ASSOCCHANGED 会让整个 shell 重建关联/图标缓存（系统级抖动）。
    // 这里只影响"当前目录显示哪些项"，用 UPDATEDIR 即可。
    const std::wstring dir = ViewFolderPath(view);
    if (!dir.empty())
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, dir.c_str(), nullptr);
    if (view) view->Refresh();
    return true;
}

int CycleHiddenState(IShellView* view, HWND owner) {
    SHELLSTATE ss{};
    SHGetSetSettings(&ss, detail::HiddenFlags(), FALSE);
    int state = 0;  // 0 全显 / 1 隐系统 / 2 隐全部
    if (!ss.fShowAllObjects)
        state = 2;
    else if (!ss.fShowSuperHidden)
        state = 1;
    int next = (state + 1) % 3;
    ss.fShowAllObjects = next == 2 ? FALSE : TRUE;
    ss.fShowSuperHidden = next == 0 ? TRUE : FALSE;
    SHGetSetSettings(&ss, detail::HiddenFlags(), TRUE);
    const std::wstring dir = ViewFolderPath(view);
    if (!dir.empty())
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, dir.c_str(), nullptr);
    (void)owner;
    if (view) view->Refresh();
    return next;
}

HiddenFlagsState GetHiddenFlags() {
    // 与 ToggleHidden / CycleHiddenState **同一个掩码**（见 detail::HiddenFlags，
    // 由 shell_ops_internal.h 唯一提供）。掩码不一致就会出现"读到的是另一个开关"。
    SHELLSTATE ss{};
    SHGetSetSettings(&ss, detail::HiddenFlags(), FALSE);
    HiddenFlagsState st;
    st.showAllObjects = ss.fShowAllObjects != FALSE;
    st.showSuperHidden = ss.fShowSuperHidden != FALSE;
    st.showExtensions = ss.fShowExtensions != FALSE;
    // 三态口径与 CycleHiddenState 的编码完全一致（否则读到的三态与写出的三态
    // 不是同一个刻度，断言 view.cycle.hidden=$changed 就会误判）。
    st.triState = !st.showAllObjects ? 2 : (!st.showSuperHidden ? 1 : 0);
    return st;
}

namespace {

// 从 IShellItem 读一组只读属性。失败项保持"未知"，不用 0 冒充。
void FillItemInfo(IShellItem* item, ItemInfo* info) {
    if (!item || !info) return;

    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) && p) {
        info->name = p;
        CoTaskMemFree(p);
    }
    p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        info->path = p;
        CoTaskMemFree(p);
    }

    SFGAOF attrs = 0;
    if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER | SFGAO_HIDDEN | SFGAO_READONLY, &attrs))) {
        info->isFolder = (attrs & SFGAO_FOLDER) != 0;
        info->hidden = (attrs & SFGAO_HIDDEN) != 0;
        info->readonly_ = (attrs & SFGAO_READONLY) != 0;
    }

    ComPtr<IShellItem2> i2;
    if (SUCCEEDED(item->QueryInterface(IID_IShellItem2, reinterpret_cast<void**>(&i2))) && i2) {
        UINT64 sz = 0;
        // 文件夹的 PKEY_Size 是"内容大小"，语义与文件不同 —— 只对文件报 size。
        if (!info->isFolder && SUCCEEDED(i2->GetUInt64(PKEY_Size, &sz))) {
            info->size = sz;
            info->sizeKnown = true;
        }
        FILETIME ft{};
        if (SUCCEEDED(i2->GetFileTime(PKEY_DateModified, &ft)) && ft.dwLowDateTime) {
            FileTimeToSystemTime(&ft, &info->modified);
            info->hasModified = true;
        }
        PWSTR t = nullptr;
        if (SUCCEEDED(i2->GetString(PKEY_ItemTypeText, &t)) && t) {
            info->typeName = t;
            CoTaskMemFree(t);
        }
    }
}

}  // namespace

bool ListView(IShellView* view, int limit, DirListing* out) {
    if (!view || !out) return false;
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;

    out->path = ViewFolderPath(view);
    out->entries.clear();
    out->truncated = false;

    int count = 0;
    if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return true;  // 空目录是合法结果

    for (int i = 0; i < count; ++i) {
        if (limit > 0 && static_cast<int>(out->entries.size()) >= limit) {
            out->truncated = true;
            break;
        }
        ComPtr<IShellItem> item;
        if (FAILED(fv2->GetItem(i, IID_IShellItem, reinterpret_cast<void**>(&item))) || !item)
            continue;
        DirEntry e;
        // DirEntry 是 ItemInfo 的子集，复用同一套读取再把要的字段搬过来。
        ItemInfo info;
        FillItemInfo(item.get(), &info);
        e.name = info.name;
        e.path = info.path;
        e.isFolder = info.isFolder;
        e.size = info.size;
        e.sizeKnown = info.sizeKnown;
        e.hidden = info.hidden;
        out->entries.push_back(std::move(e));
    }
    return true;
}

bool StatItem(IShellView* view, const std::wstring& fullPath, ItemInfo* out) {
    if (!view || fullPath.empty() || !out) return false;
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellFolder> folder = detail::GetFolder(fv2.get());
    if (!folder) return false;

    int count = 0;
    if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return false;

    for (int i = 0; i < count; ++i) {
        PITEMID_CHILD pidl = nullptr;
        if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
        std::wstring p;
        const bool got = detail::ItemFullPath(folder.get(), pidl, &p);
        if (got && detail::SamePath(p, fullPath)) {
            *out = ItemInfo{};
            // 用 pidl -> IShellItem，避免再按路径 Create（会有规范化差异）
            ComPtr<IShellItem> item;
            if (SUCCEEDED(SHCreateItemWithParent(nullptr, folder.get(), pidl, IID_IShellItem,
                                                 reinterpret_cast<void**>(&item))) &&
                item)
                FillItemInfo(item.get(), out);
            CoTaskMemFree(pidl);
            return !out->name.empty();
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

}  // namespace shellops
