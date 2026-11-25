// 导航树根集的**运行时发现**实现。规则说明见 nav_pane_explorer.h。
#include "nav_pane_explorer.h"

#include "log.h"
#include "str.h"
#include "text_file.h"   // tf::ToExtended：长路径必须过它，深层目录才能正确判存在

#include <knownfolders.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>

namespace navpane {

namespace {

// explorer 左栏组0 的两个 shell 内置命名空间。
constexpr wchar_t kClsidHome[] = L"679F85CB-0220-4080-B29B-5540CC05AAB6";
constexpr wchar_t kClsidGallery[] = L"E88865EA-0E1C-4E20-9AA6-EDCD0212C87C";

// OneDrive「个人」账号命名空间。它的显示名随账号变（TAN - 个人），
// 所以名字**不硬编码**，运行时从 IShellItem 取。
constexpr wchar_t kClsidOneDrivePersonal[] = L"018D5C66-4533-4307-9B53-224DE2ED1FE6";

// 注册表路径：explorer 把"被固定到导航树的第三方扩展"登记在 HKCU 这一处。
constexpr wchar_t kNameSpaceKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace";

// 取 IShellItem 的显示名（SIGDN_NORMALDISPLAY）。
std::wstring DisplayNameOf(IShellItem* item) {
    if (!item) return L"";
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) || !p) return L"";
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}

// "shell:::{GUID}" -> IShellItem。GUID 不带花括号。
IShellItem* ItemFromClsid(const std::wstring& guid) {
    if (guid.empty()) return nullptr;
    const std::wstring spec = L"shell:::{" + guid + L"}";
    IShellItem* item = nullptr;
    SHCreateItemFromParsingName(spec.c_str(), nullptr, IID_IShellItem,
                                reinterpret_cast<void**>(&item));
    return item;
}

// 去掉 GUID 的花括号，统一成大写无花括号形式（注册表键名带花括号）。
std::wstring NormalizeClsid(const std::wstring& raw) {
    std::wstring s = raw;
    if (s.size() >= 2 && s.front() == L'{' && s.back() == L'}') {
        s = s.substr(1, s.size() - 2);
    }
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return towupper(c); });
    return s;
}

// 枚举注册表某一级下的子键名。返回 false = 该键不存在。
bool EnumSubKeyNames(HKEY root, const wchar_t* sub, std::vector<std::wstring>* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = L"";
        DWORD len = static_cast<DWORD>(std::size(name));
        const LONG r = RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        out->push_back(name);
    }
    RegCloseKey(key);
    return true;
}

// 读 HKCR\CLSID\{G}\ShellFolder 的 FolderValueFlags。
//
// 【判据为什么是这个 —— 探针 69 全量实测】
// explorer 显示的第三方扩展，其 CLSID 下都有
// HKCR\CLSID\{G}\ShellFolder 且带 **FolderValueFlags**（同组属性 attr=0xF080004D）；
// 而**本项目自己注入的**两个（Everything 结果 / OpenList 裸）走的是 Windhawk mod
// 的 shellex 路径，ShellFolder 下只有 Attributes + WantsFORPARSING，**没有
// FolderValueFlags**，explorer 也不显示它们。
// HKCU NameSpace 里 17 个子键，有 FolderValueFlags 的恰好 15 个 —— 正好是
// "explorer 组2 的 14 个 + 组0 的 TAN - 个人"，差的那 2 个就是本项目注入的。
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

// 取命名空间项**自身**的 IShellFolder。
//
// 【为什么不能只用 SHBindToParent】SHBindToParent 返回的是**父级** folder。
// 对 Home/Gallery/OneDrive 这类项，父级都是"桌面命名空间"，用它枚举会拿到
// 同一份 31 项列表（探针 56 实测的坑）。必须再走一步
// parent->BindToObject(childLast, ...) 拿到自身（探针 58 确立）。
IShellFolder* OwnFolder(IShellItem* item) {
    if (!item) return nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetIDListFromObject(item, &pidl)) || !pidl) return nullptr;

    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD childLast = nullptr;
    IShellFolder* self = nullptr;
    if (SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder,
                                 reinterpret_cast<void**>(&parent), &childLast)) &&
        parent && childLast) {
        parent->BindToObject(childLast, nullptr, IID_IShellFolder,
                             reinterpret_cast<void**>(&self));
    }
    if (parent) parent->Release();
    ILFree(pidl);
    return self;
}

// 用**父 folder** 取子项显示名。
//
// 【为什么必须用父 folder】Home 的子项是虚拟 PIDL：IShellItem::GetDisplayName
// 与 SHGetNameFromIDList 都返回空串（探针 58/60 实测），只有
// IShellFolder::GetDisplayNameOf(childPidl) 才拿得到（探针 61 确立）。
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

// 路径是否指向一个**真实存在的目录**。
//
// 【为什么需要它】Home 命名空间的子项里前 12 项是"最近使用的文件"
// （index.html / ps_*.png / pulse-main(1).zip / *.md，探针 69 实测），它们**也是**
// 真实文件系统路径，光看"路径像不像盘符"区分不出来。
// 只有"存在且是目录"才能把最近文件挡在外面（探针 69：桌面/Downloads/文档/
// 图片/音乐/视频/tools 七个全是目录，最近文件十二个全是文件）。
bool IsExistingDirectory(const std::wstring& path) {
    if (path.size() < 2) return false;
    // 外部发现的目录可能在深层（>MAX_PATH），不过 tf::ToExtended 会静默判成不存在。
    const DWORD attr = GetFileAttributesW(tf::ToExtended(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 路径是否"像"真实文件系统路径（含盘符或 UNC 前缀）。
// 虚拟项的 path == 显示名本身（如"主文件夹"），真实文件夹是 "C:\Users\..."。
bool LooksLikeFileSystemPath(const std::wstring& path) {
    return path.size() > 2 &&
           (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\'));
}

// UTF-16 码位升序比较用的下一个码点（含代理对解码）。
uint32_t NextCodePoint(const std::wstring& s, size_t* i) {
    const uint32_t c = static_cast<uint32_t>(static_cast<uint16_t>(s[*i]));
    if (c >= 0xD800 && c <= 0xDBFF && *i + 1 < s.size()) {
        const uint32_t lo = static_cast<uint32_t>(static_cast<uint16_t>(s[*i + 1]));
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            *i += 2;
            return 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
    }
    *i += 1;
    return c;
}

// 按**码位**比两个显示名。
//
// 【为什么不能直接用 std::wstring 的 operator<】它走 char_traits<wchar_t>::compare，
// 在 libstdc++ 上是 memcmp —— 按**内存字节**比。UTF-16 小端下这不等于码位序：
// "天"(U+5929，小端字节 29 59) 会排到 "小"(U+5C0F，字节 0F 5C) **后面**，正好反了。
// explorer 的实际顺序是按码位（"天"在"小"前），所以必须自己解码再比。
int CompareCodePoints(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const uint32_t ca = NextCodePoint(a, &i);
        const uint32_t cb = NextCodePoint(b, &j);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

// 往结果里追加一个命名空间 CLSID 根。解析不出/没名字 = 这台机器没这个扩展，
// 直接跳过（与 explorer 的行为一致：没装的东西不显示）。
void AppendClsidRoot(std::vector<RootSpec>* out, const wchar_t* clsid, int group) {
    const std::wstring guid = NormalizeClsid(clsid);
    IShellItem* item = ItemFromClsid(guid);
    if (!item) return;
    const std::wstring name = DisplayNameOf(item);
    std::vector<unsigned short> pidl = PidlBytes(item);
    item->Release();
    if (name.empty()) return;
    RootSpec spec;
    spec.kind = RootSpec::Kind::Clsid;
    spec.value = guid;
    spec.label = name;
    spec.group = group;
    spec.pidl = std::move(pidl);
    out->push_back(spec);
}

}  // namespace

// ---------------------------------------------------------------------------
// 组1：Home（主页）命名空间自身的**固定到快速访问**的子项。
//
// 【探针 69 实测】Home 的子项枚举顺序：
//   [0..11] 最近使用的文件（index.html / ps_*.png / pulse-main(1).zip / *.md）
//   [12..18] 桌面 / Downloads / 文档 / 图片 / 音乐 / 视频 / tools  <- 就是 explorer 组1
//   后面还有一批名为"主文件夹"的虚拟项（path == name，无真实路径）
// 规则：**真实存在且是目录**的那批就是 explorer 组1（最近文件被"是目录"这一条挡掉，
// 虚拟项被"路径像文件系统路径"挡掉），按枚举顺序原样返回。
// "tools"（E:\ExBarBrowser\tools）就是这样混进来的 —— 它是用户自己固定到
// 快速访问的目录，explorer 会显示，我们也不能漏。
// ---------------------------------------------------------------------------
static std::vector<RootSpec> DiscoverQuickAccessRoots() {
    std::vector<RootSpec> out;

    IShellItem* home = ItemFromClsid(kClsidHome);
    if (!home) return out;
    // EnumObjects 给的是相对 self 的 child PIDL；存进 RootSpec 必须是绝对路径，
    // 所以先把 home 的绝对 PIDL 留下，后面用 ILCombine 拼成完整 IDList。
    PIDLIST_ABSOLUTE parentPidl = nullptr;
    if (FAILED(SHGetIDListFromObject(home, &parentPidl)) || !parentPidl) {
        home->Release();
        return out;
    }
    IShellFolder* self = OwnFolder(home);
    if (!self) {
        ILFree(parentPidl);
        home->Release();
        return out;
    }
    IEnumIDList* eidl = nullptr;
    if (FAILED(self->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &eidl)) ||
        !eidl) {
        self->Release();
        ILFree(parentPidl);
        home->Release();
        return out;
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    while (eidl->Next(1, &pidl, nullptr) == S_OK) {
        PCUITEMID_CHILD child = ILFindLastID(pidl);
        std::wstring name;
        NameViaFolder(self, child, &name);

        // 路径：用父 folder 的 SIGDN_FILESYSPATH
        std::wstring path;
        {
            STRRET sr{};
            sr.uType = STRRET_WSTR;
            if (SUCCEEDED(self->GetDisplayNameOf(child, SIGDN_FILESYSPATH, &sr)) &&
                sr.uType == STRRET_WSTR && sr.pOleStr) {
                path.assign(sr.pOleStr);
                CoTaskMemFree(sr.pOleStr);
            }
        }

        if (name.empty() || !LooksLikeFileSystemPath(path) || !IsExistingDirectory(path)) {
            ILFree(pidl);
            pidl = nullptr;
            continue;
        }

        RootSpec spec;
        // 是已知文件夹就用 Known 形式：图标与"点进去的行为"都与 explorer 一致
        // （桌面/文档/图片/音乐/视频 在 explorer 里用的是各自的专属图标，
        //  用普通路径只会拿到通用文件夹图标 —— 探针 53 截图能看出差别）。
        std::wstring knownName;
        if (KnownFolderNameForPath(path, &knownName)) {
            spec.kind = RootSpec::Kind::Known;
            spec.value = knownName;
        } else {
            spec.kind = RootSpec::Kind::Path;
            spec.value = path;
        }
        spec.label = name;
        spec.group = 1;
        // 发现线程顺手把绝对 PIDL 留下：主线程建树时不用再解析这 7 个路径。
        if (PIDLIST_ABSOLUTE abs = ILCombine(parentPidl, child)) {
            spec.pidl = PidlBytes(abs);
            ILFree(abs);
        }
        out.push_back(spec);

        ILFree(pidl);
        pidl = nullptr;
    }
    eidl->Release();
    self->Release();
    ILFree(parentPidl);
    home->Release();
    return out;
}

// ---------------------------------------------------------------------------
// 组0 第三项（TAN - 个人）+ 组2 前 14 项：来自 HKCU\...\Explorer\Desktop\NameSpace。
//
// 【探针 69 全量实测】HKCU NameSpace 下 17 个子键：
//   15 个带 FolderValueFlags（= 系统 shell 命名空间扩展，explorer 显示）
//   2 个不带（本项目 Windhawk mod 注入的 Everything 结果 / OpenList 裸）
// 这 15 个里：
//   {018D5C66}(OneDrive 个人账号, 显示名"TAN - 个人") -> 归**组0**末尾
//   其余 14 个                                     -> 归**组2**，按显示名码位升序
// ---------------------------------------------------------------------------
static std::vector<RootSpec> DiscoverNamespaceRoots() {
    struct Entry {
        std::wstring guid;
        std::wstring name;
        std::vector<unsigned short> pidl;
    };
    std::vector<Entry> entries;
    std::vector<RootSpec> out;

    std::vector<std::wstring> guids;
    if (!EnumSubKeyNames(HKEY_CURRENT_USER, kNameSpaceKey, &guids)) return out;

    for (const auto& raw : guids) {
        const std::wstring guid = NormalizeClsid(raw);
        if (guid.empty()) continue;
        // 不是系统 shell 命名空间扩展 = 本项目注入的，explorer 不显示。
        if (!HasFolderValueFlags(guid)) continue;
        IShellItem* item = ItemFromClsid(guid);
        if (!item) continue;
        const std::wstring name = DisplayNameOf(item);
        std::vector<unsigned short> pidl = PidlBytes(item);
        item->Release();
        if (name.empty()) continue;
        entries.push_back({guid, name, std::move(pidl)});
    }

    // OneDrive 个人账号 -> 组0 末尾（名字随账号变，运行时取到的就是"TAN - 个人"）。
    for (const auto& e : entries) {
        if (e.guid != kClsidOneDrivePersonal) continue;
        RootSpec spec;
        spec.kind = RootSpec::Kind::Clsid;
        spec.value = e.guid;
        spec.label = e.name;
        spec.group = 0;
        spec.pidl = e.pidl;
        out.push_back(spec);
    }

    // 其余 = 组2 前 14 项，按显示名码位升序（explorer 就是这个顺序，探针 45/53 实测）。
    std::vector<Entry> rest;
    for (const auto& e : entries) {
        if (e.guid == kClsidOneDrivePersonal) continue;
        rest.push_back(e);
    }
    std::stable_sort(rest.begin(), rest.end(), [](const Entry& a, const Entry& b) {
        return CompareCodePoints(a.name, b.name) < 0;
    });
    for (const auto& e : rest) {
        RootSpec spec;
        spec.kind = RootSpec::Kind::Clsid;
        spec.value = e.guid;
        spec.label = e.name;
        spec.group = 2;
        spec.pidl = e.pidl;
        out.push_back(spec);
    }
    return out;
}

std::vector<RootSpec> DiscoverExplorerRoots() {
    std::vector<RootSpec> out;

    // 组0：主文件夹 / 图库  +  （TAN - 个人 由注册表那一轮补进来）
    AppendClsidRoot(&out, kClsidHome, 0);
    AppendClsidRoot(&out, kClsidGallery, 0);

    // 注册表这一轮同时产出"组0 的 TAN - 个人"与"组2 的 14 项"。
    {
        const std::vector<RootSpec> ns = DiscoverNamespaceRoots();
        for (const auto& s : ns) {
            if (s.group == 0) out.push_back(s);   // TAN - 个人，接在图库后面
        }
        // 组1：快速访问（桌面/Downloads/文档/图片/音乐/视频/tools）
        for (const auto& s : DiscoverQuickAccessRoots()) out.push_back(s);
        // 组2：14 个第三方扩展（已按码位排好）
        for (const auto& s : ns) {
            if (s.group == 2) out.push_back(s);
        }
    }

    // 组2 末尾的固定尾巴：此电脑 / 库 / 网络。
    // 这三个**不在**注册表 NameSpace 的排序里 —— explorer 把它们固定放在最后
    // （探针 45 MSAA 基线：此电脑 y=1096 / 库 y=1126 / 网络 y=1156）。
    {
        const wchar_t* tail[] = {L"ComputerFolder", L"UsersLibraries", L"NetworkFolder"};
        for (const wchar_t* name : tail) {
            RootSpec spec;
            spec.kind = RootSpec::Kind::Known;
            spec.value = name;
            spec.group = 2;
            if (IShellItem* item = ItemFromRootSpec(spec)) {
                spec.pidl = PidlBytes(item);
                item->Release();
            }
            out.push_back(spec);
        }
    }

    int g0 = 0, g1 = 0, g2 = 0;
    for (const auto& s : out) {
        if (s.group == 0) ++g0;
        else if (s.group == 1) ++g1;
        else ++g2;
    }
    LogDbg(L"[navpane] explorer 根清单：%d 项（组0 %d / 组1 %d / 组2 %d）",
           static_cast<int>(out.size()), g0, g1, g2);
    for (const auto& s : out) {
        LogDbg(L"[navpane]   [组%d] %-40s %s", s.group, s.label.c_str(),
               RootSpec::Format(s).c_str());
    }
    return out;
}

}  // namespace navpane
