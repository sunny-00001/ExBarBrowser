#include "view_stats.h"

#include "shell_ops.h"

#include <propkey.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace {

// 磁盘空间缓存：每 800ms 直接 GetDiskFreeSpaceEx 在"已映射但离线的网络盘"上会
// 同步阻塞几十秒（Exbar.cpp:58754 也明确记录过这个坑）。按盘符缓存 5s；
// 远程盘查一次后 60s 再查，避免反复卡 UI。
struct DiskEntry {
    int drive = -1;  // 已缓存过哪个盘
    DWORD tick = 0;
    bool has = false;
    ULONGLONG freeB = 0;
    ULONGLONG totalB = 0;
    bool remote = false;
};
DiskEntry g_disk[26];

bool QueryDiskSpace(int driveLetter, ULONGLONG* freeB, ULONGLONG* totalB) {
    const int idx = (driveLetter | 32) - L'a';
    if (idx < 0 || idx >= 26) return false;
    DiskEntry& e = g_disk[idx];
    const DWORD now = GetTickCount();
    const DWORD ttl = (e.drive == driveLetter && e.remote) ? 60000 : 5000;
    if (e.drive == driveLetter && now - e.tick < ttl) {
        *freeB = e.freeB;
        *totalB = e.totalB;
        return e.has;
    }
    wchar_t root[4] = {static_cast<wchar_t>(driveLetter), L':', L'\\', 0};
    ULARGE_INTEGER callerFree{}, total{}, totalFree{};
    const bool ok = GetDiskFreeSpaceExW(root, &callerFree, &total, &totalFree) != FALSE;
    e.drive = driveLetter;
    e.tick = now;
    e.has = ok && total.QuadPart > 0;
    e.freeB = ok ? callerFree.QuadPart : 0;
    e.totalB = ok ? total.QuadPart : 0;
    e.remote = GetDriveTypeW(root) == DRIVE_REMOTE;
    *freeB = e.freeB;
    *totalB = e.totalB;
    return e.has;
}



bool SameKey(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return IsEqualGUID(a.fmtid, b.fmtid) && a.pid == b.pid;
}

bool IsFolderItem(IShellItem* item) {
    if (!item) return false;
    SFGAOF attrs = 0;
    if (FAILED(item->GetAttributes(SFGAO_FOLDER, &attrs))) return false;
    return (attrs & SFGAO_FOLDER) != 0;
}

bool ItemSize(IShellItem* item, unsigned long long* out) {
    if (!item || !out) return false;
    ComPtr<IShellItem2> i2;
    if (FAILED(item->QueryInterface(IID_IShellItem2, reinterpret_cast<void**>(&i2))) || !i2)
        return false;
    UINT64 v = 0;
    if (FAILED(i2->GetUInt64(PKEY_Size, &v))) return false;
    *out = v;
    return true;
}

bool ItemDate(IShellItem* item, FILETIME* out) {
    if (!item || !out) return false;
    ComPtr<IShellItem2> i2;
    if (FAILED(item->QueryInterface(IID_IShellItem2, reinterpret_cast<void**>(&i2))) || !i2)
        return false;
    return SUCCEEDED(i2->GetFileTime(PKEY_DateModified, out));
}

bool Newer(const FILETIME& a, const FILETIME& b) {
    return CompareFileTime(&a, &b) > 0;
}

std::wstring ItemPath(IShellItem* item) {
    if (!item) return std::wstring();
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return std::wstring();
    std::wstring s(p);
    CoTaskMemFree(p);
    return s;
}

// 选中项的扩展名（小写，含点，如 ".txt"）。无扩展名返回空串。
//
// PKEY_FileExtension 是正规来源，但虚拟项（此电脑、库、网络）没有这个属性 ——
// 那时退回显示名的末段扩展名。两条路都取不到就返回空（"无扩展名"也是一种类型）。
std::wstring ItemExtension(IShellItem* item) {
    if (!item) return std::wstring();
    ComPtr<IShellItem2> i2;
    if (SUCCEEDED(item->QueryInterface(IID_IShellItem2, reinterpret_cast<void**>(&i2))) && i2) {
        PWSTR p = nullptr;
        if (SUCCEEDED(i2->GetString(PKEY_FileExtension, &p)) && p) {
            std::wstring s(p);
            CoTaskMemFree(p);
            if (!s.empty()) {
                if (s[0] != L'.') s.insert(s.begin(), L'.');
                for (wchar_t& c : s) c = towlower(c);
                return s;
            }
        }
    }
    PWSTR dn = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &dn)) && dn) {
        std::wstring s(dn);
        CoTaskMemFree(dn);
        const size_t dot = s.rfind(L'.');
        // dot > 0：排除 ".git" 这种整名就是点的隐藏文件。
        if (dot != std::wstring::npos && dot > 0 && dot + 1 < s.size()) {
            std::wstring ext = s.substr(dot);
            for (wchar_t& c : ext) c = towlower(c);
            return ext;
        }
    }
    return std::wstring();
}

}  // namespace

bool CollectViewStats(IShellView* view, ViewStats* out, SplitCache* cache,
                      const std::wstring& ownerPath, bool allowSplit, bool lightOnly) {
    if (!view || !out) return false;
    *out = ViewStats{};

    ComPtr<IFolderView2> fv2 = shellops::FolderView2(view);


    if (!fv2) return false;

    int total = 0;
    if (SUCCEEDED(fv2->ItemCount(SVGIO_ALLVIEW, &total)) && total > 0)
        out->total = static_cast<unsigned>(total);

    // 轻量判据：只回答"状态栏要不要重算"。两次 COM 调用，不碰逐项属性、
    // 容器属性、排序方向与磁盘空间（后者在离线网络盘上能卡几十秒）。
    if (lightOnly) {
        ComPtr<IShellItemArray> sel;
        if (SUCCEEDED(fv2->GetSelection(FALSE, &sel)) && sel) {
            DWORD n = 0;
            sel->GetCount(&n);
            out->selCount = n;
            out->hasSelection = n > 0;
        }
        return true;
    }

    // 分类（文件夹/文件）：上限 8000，超出只给总数
    const int kSplitCap = 8000;
    // 【缓存键 = 路径（不是 total）】旧实现只比 total，两个不同目录项数相同时
    // 会命中对方缓存，状态栏显示另一个目录的文件/文件夹数 —— 见 view_stats.h
    // 里 SplitCache 的说明。ownerPath 为空时视为"不能判归属"，直接不用缓存。
    const bool cacheUsable = cache && !ownerPath.empty();
    const bool cached = cacheUsable && cache->valid && cache->path == ownerPath &&
                        cache->total == static_cast<unsigned>(total > 0 ? total : 0);
    if (cached) {
        out->folders = cache->folders;
        out->files = cache->files;
        out->splitKnown = true;
    } else if (allowSplit && total > 0 && total <= kSplitCap) {
        bool ok = true;
        // 一次 Items() 拿整数组，而不是逐项 fv2->Item(i) + SHCreateItemFromIDList：
        // 后者在 C://Windows//System32（5003 项）上每 800ms 要做两万次 COM 调用 + 5000 次
        // 两段内存分配，全在 UI 线程（拖窗口/滚轮掉帧）。Exbar.cpp:58913 用的就是这个办法。
        ComPtr<IShellItemArray> all;
        HRESULT hrAll = fv2->Items(SVGIO_ALLVIEW, IID_IShellItemArray,
                                   reinterpret_cast<void**>(&all));
        if (SUCCEEDED(hrAll) && all) {
            DWORD n = 0;
            all->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                ComPtr<IShellItem> item;
                if (FAILED(all->GetItemAt(i, &item)) || !item) {
                    ok = false;
                    break;
                }
                if (IsFolderItem(item.get()))
                    ++out->folders;
                else
                    ++out->files;
            }
        } else {
            // 个别第三方 NSE 视图不支持 Items()：退回逐项，但保持同一套统计口径。
            for (int i = 0; i < total; ++i) {
                PITEMID_CHILD pidl = nullptr;
                if (FAILED(fv2->Item(i, &pidl)) || !pidl) {
                    ok = false;
                    break;
                }
                ComPtr<IShellItem> item;
                HRESULT hr = SHCreateItemFromIDList(pidl, IID_IShellItem,
                                                    reinterpret_cast<void**>(&item));
                CoTaskMemFree(pidl);
                if (FAILED(hr) || !item) {
                    ok = false;
                    break;
                }
                if (IsFolderItem(item.get()))
                    ++out->folders;
                else
                    ++out->files;
            }
        }
        out->splitKnown = ok;
        if (cacheUsable && ok) {
            cache->path = ownerPath;   // 键是路径，见上面 cacheUsable 的说明
            cache->total = static_cast<unsigned>(total);
            cache->folders = out->folders;
            cache->files = out->files;
            cache->valid = true;
        }
    }

    // 选中集
    ComPtr<IShellItemArray> sel;
    if (SUCCEEDED(fv2->GetSelection(FALSE, &sel)) && sel) {
        DWORD n = 0;
        sel->GetCount(&n);
        out->selCount = n;
        out->hasSelection = n > 0;
        FILETIME newest{};
        bool haveNewest = false;
        // 类型统计：记出现过的扩展名集合（去重，上限 32 种 —— 够判"单/多种"，
        // 又不让内存随选中数无界增长）。
        std::vector<std::wstring> exts;
        // 上限：Ctrl+A 选中上万个文件时，逐项取大小/日期会让 UI 线程卡死好几秒。
        // 与 Exbar.cpp:58979 的 kSizeCap 同口径：只统计前 500 项，超出就明说"已截断"。
        constexpr DWORD kSelCap = 500;
        const DWORD scan = n < kSelCap ? n : kSelCap;
        out->selTruncated = n > kSelCap;
        for (DWORD i = 0; i < scan; ++i) {
            ComPtr<IShellItem> item;
            if (FAILED(sel->GetItemAt(i, &item)) || !item) continue;
            const bool isFolder = IsFolderItem(item.get());
            if (isFolder) {
                ++out->selFolders;
            } else {
                ++out->selFiles;
                // 类型只看文件：文件夹没有"扩展名"，把它算进来会让类型段永远"多种类型"。
                const std::wstring ext = ItemExtension(item.get());
                if (!ext.empty()) {
                    bool known = false;
                    for (const auto& e : exts)
                        if (e == ext) { known = true; break; }
                    if (!known && exts.size() < 32) exts.push_back(ext);
                }
            }
            unsigned long long size = 0;
            if (ItemSize(item.get(), &size)) {
                out->selBytes += size;
                out->selBytesKnown = true;
            }
            FILETIME ft{};
            if (ItemDate(item.get(), &ft)) {
                if (!haveNewest || Newer(ft, newest)) {
                    newest = ft;
                    haveNewest = true;
                }
            }
        }
        out->selExtKinds = static_cast<int>(exts.size());
        if (exts.size() == 1) out->selExt = exts[0];
        if (haveNewest) {
            FILETIME local{};
            if (!FileTimeToLocalFileTime(&newest, &local)) local = newest;
            FileTimeToSystemTime(&local, &out->modified);
            out->hasModified = true;
            out->modifiedFromSelection = true;
        }
    }

    // 容器
    ComPtr<IShellItem2> folder2;
    if (SUCCEEDED(fv2->GetFolder(IID_IShellItem2, reinterpret_cast<void**>(&folder2))) && folder2) {
        UINT64 size = 0;
        if (SUCCEEDED(folder2->GetUInt64(PKEY_Size, &size))) {
            out->containerBytes = size;
            out->containerBytesKnown = true;
        }
        FILETIME ft{};
        if (!out->hasModified && SUCCEEDED(folder2->GetFileTime(PKEY_DateModified, &ft))) {
            FILETIME local{};
            if (!FileTimeToLocalFileTime(&ft, &local)) local = ft;
            FileTimeToSystemTime(&local, &out->modified);
            out->hasModified = true;
        }
        PWSTR path = nullptr;
        if (SUCCEEDED(folder2->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            out->containerPath = path;
            CoTaskMemFree(path);
        } else {
            out->virtualRoot = true;
        }
    } else {
        out->virtualRoot = true;
    }

    // 盘符空间（带 TTL 缓存，别让离线网络盘卡住 UI 线程）
    if (out->containerPath.size() >= 2 && out->containerPath[1] == L':') {
        ULONGLONG freeB = 0, totalB = 0;
        if (QueryDiskSpace(static_cast<int>(out->containerPath[0]), &freeB, &totalB)) {
            out->diskFree = freeB;
            out->diskTotal = totalB;
            out->hasDisk = totalB > 0;
        }
    }

    // 日期排序方向
    int cols = 0;
    if (SUCCEEDED(fv2->GetSortColumnCount(&cols)) && cols > 0) {
        cols = std::min(cols, 8);  // 防御坏实现：不让 800ms 定时器每拍分配上万条
        std::vector<SORTCOLUMN> list(static_cast<size_t>(cols));
        if (SUCCEEDED(fv2->GetSortColumns(list.data(), cols))) {
            for (const auto& c : list) {
                if (SameKey(c.propkey, PKEY_DateModified)) {
                    out->dateSort = c.direction > 0 ? 1 : -1;
                    break;
                }
            }
        }
    }
    return true;
}

std::vector<std::wstring> SelectedPaths(IShellView* view, size_t max) {
    std::vector<std::wstring> out;
    if (!view) return out;
    ComPtr<IFolderView2> fv2 = shellops::FolderView2(view);

    if (!fv2) return out;
    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->GetSelection(FALSE, &sel)) || !sel) return out;
    DWORD n = 0;
    sel->GetCount(&n);
    for (DWORD i = 0; i < n && out.size() < max; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(sel->GetItemAt(i, &item)) || !item) continue;
        std::wstring p = ItemPath(item.get());
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

std::wstring ViewFolderPath(IShellView* view) {
    if (!view) return std::wstring();
    ComPtr<IFolderView2> fv2 = shellops::FolderView2(view);

    if (!fv2) return std::wstring();
    ComPtr<IShellItem2> folder2;
    if (FAILED(fv2->GetFolder(IID_IShellItem2, reinterpret_cast<void**>(&folder2))) || !folder2)
        return std::wstring();
    PWSTR p = nullptr;
    if (FAILED(folder2->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return std::wstring();
    std::wstring s(p);
    CoTaskMemFree(p);
    return s;
}

std::wstring FormatSize(unsigned long long bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    if (u == 0)
        swprintf(buf, 64, L"%llu B", bytes);
    else
        swprintf(buf, 64, L"%.1f %ls", v, units[u]);
    return buf;
}

std::wstring FormatDate(const SYSTEMTIME& st) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute);
    return buf;
}
