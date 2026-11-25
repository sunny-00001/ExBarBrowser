// 探针 72：量化 `DiscoverExplorerRoots()` 每一段到底多慢。
//
// 【为什么需要】用户报"启动速度慢"。产品日志（%TEMP%\ExBarBrowser.log）累计
// 34 次启动、188 条 `explorer 根清单` —— 也就是**每次启动约 5.5 次全量发现**，
// 同一份数据被反复重算。每次发现包含三类昂贵操作：
//   1. 注册表枚举 HKCU\...\Explorer\Desktop\NameSpace（便宜）
//   2. 每个子键一次 SHCreateItemFromParsingName("shell:::{GUID}")
//      —— 这一条会**加载第三方 shell 命名空间扩展的 DLL**（各网盘客户端），
//      是启动期最贵的单项
//   3. Home 命名空间 EnumObjects + 每项 GetDisplayNameOf + GetFileAttributes
//
// 本探针把同样的工作量拆开计时，给出"哪一段该缓存、缓存一次能省多少"的数字。
// 不调产品代码（导航根集正被另一处改动同时重写），只复刻它的调用序列。
//
// 合规：只读注册表 + 只读 shell 命名空间，不发任何合成输入（R21）。
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kNameSpaceKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace";

struct Timer {
    LARGE_INTEGER f{}, a{}, b{};
    Timer() {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
    }
    double Ms() {
        QueryPerformanceCounter(&b);
        return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 /
               static_cast<double>(f.QuadPart);
    }
};

std::wstring NormalizeClsid(const std::wstring& raw) {
    std::wstring s = raw;
    if (s.size() >= 2 && s.front() == L'{' && s.back() == L'}') s = s.substr(1, s.size() - 2);
    return s;
}

IShellItem* ItemFromClsid(const std::wstring& guid) {
    const std::wstring spec = L"shell:::{" + guid + L"}";
    IShellItem* item = nullptr;
    SHCreateItemFromParsingName(spec.c_str(), nullptr, IID_IShellItem,
                                reinterpret_cast<void**>(&item));
    return item;
}

bool HasFolderValueFlags(const std::wstring& guid) {
    const std::wstring path = L"CLSID\\{" + guid + L"}\\ShellFolder";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, value = 0, size = sizeof(value);
    const LONG r = RegQueryValueExW(key, L"FolderValueFlags", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_DWORD;
}

IShellFolder* OwnFolder(IShellItem* item) {
    if (!item) return nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetIDListFromObject(item, &pidl)) || !pidl) return nullptr;
    IShellFolder* parent = nullptr;
    IShellFolder* self = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent),
                                 &child)) &&
        parent && child) {
        parent->BindToObject(child, nullptr, IID_IShellFolder, reinterpret_cast<void**>(&self));
    }
    if (parent) parent->Release();
    ILFree(pidl);
    return self;
}

bool NameViaFolder(IShellFolder* parent, PCUITEMID_CHILD child, std::wstring* out) {
    if (!parent || !child || !out) return false;
    STRRET sr{};
    sr.uType = STRRET_WSTR;
    if (FAILED(parent->GetDisplayNameOf(child, SIGDN_NORMALDISPLAY, &sr))) return false;
    if (sr.uType != STRRET_WSTR || !sr.pOleStr) return false;
    out->assign(sr.pOleStr);
    CoTaskMemFree(sr.pOleStr);
    return true;
}

// 复刻一次完整的 DiscoverExplorerRoots 工作量，但分段计时。
void RunOnePass() {
    // ---- 1) 注册表枚举 ----
    std::vector<std::wstring> guids;
    {
        Timer t;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kNameSpaceKey, 0, KEY_READ, &key) ==
            ERROR_SUCCESS) {
            for (DWORD i = 0;; ++i) {
                wchar_t name[256] = L"";
                DWORD len = static_cast<DWORD>(std::size(name));
                const LONG r =
                    RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
                if (r == ERROR_NO_MORE_ITEMS) break;
                if (r != ERROR_SUCCESS) continue;
                guids.push_back(name);
            }
            RegCloseKey(key);
        }
        printf("  1) 注册表枚举         %8.2f ms   （%d 个子键）\n", t.Ms(),
               static_cast<int>(guids.size()));
    }

    // ---- 2) 逐个：读 flags + 解析 CLSID（后者加载第三方扩展 DLL）----
    int parsed = 0, kept = 0;
    double tFlags = 0, tParse = 0;
    for (const auto& raw : guids) {
        const std::wstring guid = NormalizeClsid(raw);
        if (guid.empty()) continue;
        {
            Timer t;
            const bool f = HasFolderValueFlags(guid);
            tFlags += t.Ms();
            if (!f) continue;
            ++kept;
        }
        Timer t;
        IShellItem* it = ItemFromClsid(guid);
        tParse += t.Ms();
        if (it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) && p) CoTaskMemFree(p);
            it->Release();
            ++parsed;
        }
    }
    printf("  2a) 读 FolderValueFlags %7.2f ms   （%d 个通过）\n", tFlags, kept);
    printf("  2b) 解析 CLSID->item  %8.2f ms   （%d 个成功 -> 加载了 %d 个第三方 DLL）\n",
           tParse, parsed, parsed);

    // ---- 3) Home 命名空间枚举（组1 七项）----
    {
        Timer t;
        IShellItem* home = ItemFromClsid(L"679F85CB-0220-4080-B29B-5540CC05AAB6");
        int dirs = 0, total = 0;
        if (home) {
            IShellFolder* self = OwnFolder(home);
            if (self) {
                IEnumIDList* eidl = nullptr;
                if (SUCCEEDED(self->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                                                &eidl)) &&
                    eidl) {
                    PIDLIST_ABSOLUTE pidl = nullptr;
                    while (eidl->Next(1, &pidl, nullptr) == S_OK) {
                        ++total;
                        PCUITEMID_CHILD child = ILFindLastID(pidl);
                        std::wstring name, path;
                        NameViaFolder(self, child, &name);
                        STRRET sr{};
                        sr.uType = STRRET_WSTR;
                        if (SUCCEEDED(self->GetDisplayNameOf(child, SIGDN_FILESYSPATH, &sr)) &&
                            sr.uType == STRRET_WSTR && sr.pOleStr) {
                            path.assign(sr.pOleStr);
                            CoTaskMemFree(sr.pOleStr);
                        }
                        if (!path.empty()) {
                            const DWORD a = GetFileAttributesW(path.c_str());
                            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                                ++dirs;
                            }
                        }
                        ILFree(pidl);
                        pidl = nullptr;
                    }
                    eidl->Release();
                }
                self->Release();
            }
            home->Release();
        }
        printf("  3) Home 枚举+属性     %8.2f ms   （%d 项，其中目录 %d 个）\n", t.Ms(), total,
               dirs);
    }
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    printf("===== 第 1 次（冷：第三方扩展 DLL 首次加载）=====\n");
    {
        Timer all;
        RunOnePass();
        printf("  >>> 合计 %.2f ms\n", all.Ms());
    }

    printf("\n===== 第 2~4 次（DLL 已加载，纯 shell 调用）=====\n");
    for (int i = 2; i <= 4; ++i) {
        Timer all;
        printf("  --- 第 %d 次 ---\n", i);
        RunOnePass();
        printf("  >>> 合计 %.2f ms\n", all.Ms());
    }

    printf("\n【判读】第 1 次 = 启动期真实成本；第 2~4 次 = 重复发现多付的钱。\n");
    printf("        如果每次启动跑 5.5 次，那么浪费 ≈ 4.5 × 单次耗时。\n");
    CoUninitialize();
    return 0;
}