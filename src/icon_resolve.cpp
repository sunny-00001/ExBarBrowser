// 统一图标识别实现（移植自 Exbar.cpp 的 NormalizeShellPath / ShellGuidFromParsing /
// VirtualNameMap / ResolveShellPath / IconFromLocation / LoadDefaultIconForGuid /
// GetPathIcon）。只保留通用链路，去掉 OpenList/Everything 专属品牌图标分支。
#include "icon_resolve.h"

#include <unordered_map>

#include "eb_ui.h"

namespace ico {
namespace {

// "shell:::{GUID}" → "::{GUID}"。NSE 的 FORPARSING 报的是前者，本工程其余部分
// （"::{" 前缀判定、PIDL 解析）一律按后者写，必须在这里统一收口。
std::wstring NormalizeShellPath(const std::wstring& s) {
    // ⚠ substr(8)："shell:::" 共 8 个字符（substr(7) 会砍出三冒号，解析必失败）。
    if (s.rfind(L"shell:::", 0) == 0) return std::wstring(L"::") + s.substr(8);
    return s;
}

// 从 "::{GUID}[...]" 取 "{GUID}" 段；非花括号命名空间返回空。
std::wstring ShellGuidFromParsing(const std::wstring& s) {
    if (s.rfind(L"::{", 0) != 0) return {};
    size_t end = s.find(L'}', 3);
    if (end == std::wstring::npos) return {};
    return s.substr(2, end - 1);
}

// 友好显示名 → 解析串。桌面命名空间种子：Explorer 的此电脑/回收站等没有文件
// 系统路径，UI 里存的是 SIGDN_NORMALDISPLAY 文本，回喂 SHParseDisplayName 会失败，
// 靠这张表把它们映射回 "::{GUID}" 才能取到图标。
std::unordered_map<std::wstring, std::wstring>& VirtualNameMap() {
    static std::unordered_map<std::wstring, std::wstring> m;
    return m;
}

void EnsureVirtualNameMap(bool force = false) {
    static bool seeded = false;
    if (seeded && !force) return;
    seeded = true;
    IShellFolder* pd = nullptr;
    if (FAILED(SHGetDesktopFolder(&pd)) || !pd) return;
    IEnumIDList* pe = nullptr;
    if (SUCCEEDED(pd->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_INCLUDEHIDDEN, &pe)) &&
        pe) {
        PIDLIST_ABSOLUTE child = nullptr;  // desktop-rooted => 已是绝对 PIDL
        while (pe->Next(1, &child, nullptr) == S_OK && child) {
            LPWSTR nm = nullptr, ps = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(child, SIGDN_NORMALDISPLAY, &nm)) && nm &&
                SUCCEEDED(SHGetNameFromIDList(child, SIGDN_DESKTOPABSOLUTEPARSING, &ps)) &&
                ps) {
                if (VirtualNameMap().find(nm) == VirtualNameMap().end())
                    VirtualNameMap()[nm] = ps;
            }
            if (nm) CoTaskMemFree(nm);
            if (ps) CoTaskMemFree(ps);
            CoTaskMemFree(child);
        }
        pe->Release();
    }
    pd->Release();
}

// 存储路径 → SHParseDisplayName/SHGetFileInfo 能解析的串：
//   "::{...}" 与真实文件系统路径原样通过；友好显示名（"此电脑"）映射回解析串。
std::wstring ResolveShellPath(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring n = NormalizeShellPath(s);
    if (n.rfind(L"::{", 0) == 0) return n;  // 已是规范解析串
    // 只有"看起来像名字"（无路径分隔符、非盘符）才需要虚拟名映射；普通路径原样返回。
    bool looksLikeName =
        n.find(L'\\') == std::wstring::npos && !(n.size() >= 2 && n[1] == L':');
    if (!looksLikeName) return n;
    EnsureVirtualNameMap();
    auto it = VirtualNameMap().find(n);
    if (it != VirtualNameMap().end()) return NormalizeShellPath(it->second);
    // 映射未命中：自定义命名空间可能在首次种子后才注册，强制重扫一次桌面（仅一次）。
    static bool reseeded = false;
    if (!reseeded) {
        reseeded = true;
        EnsureVirtualNameMap(true);
        it = VirtualNameMap().find(n);
        if (it != VirtualNameMap().end()) return NormalizeShellPath(it->second);
    }
    return n;
}

// 从 icon 路径+索引加载 HICON；small 决定填哪个尺寸槽（ExtractIconExW 的大/小是两个出参）。
HICON IconFromLocation(const wchar_t* iconPath, int idx, bool small) {
    if (!iconPath || !*iconPath) return nullptr;
    int px = GetSystemMetrics(small ? SM_CXSMICON : SM_CXICON);
    HICON h = nullptr;
    if (ExtractIconExW(iconPath, idx, small ? nullptr : &h, small ? &h : nullptr, 1) > 0 && h)
        return h;
    if (GetFileAttributesW(iconPath) != INVALID_FILE_ATTRIBUTES)
        h = (HICON)LoadImageW(nullptr, iconPath, IMAGE_ICON, px, px, LR_LOADFROMFILE);
    return h;
}

// 读命名空间声明的图标："CLSID\{GUID}\DefaultIcon"。guid 必须带花括号。
// 没有可加载图标时返回 nullptr（不分配），调用方可继续退 PIDL / stock 图标。
HICON LoadDefaultIconForGuid(const std::wstring& guid, bool small) {
    std::wstring subkey = L"CLSID\\" + guid + L"\\DefaultIcon";
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, subkey.c_str(), 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return nullptr;
    wchar_t val[MAX_PATH] = {};
    DWORD vlen = sizeof(val);
    HICON hIcon = nullptr;
    if (RegQueryValueExW(hk, nullptr, nullptr, nullptr, (LPBYTE)val, &vlen) == ERROR_SUCCESS &&
        val[0]) {
        wchar_t iconPath[MAX_PATH] = {};
        int idx = -1;
        if (wchar_t* comma = wcsrchr(val, L',')) {
            *comma = 0;
            idx = _wtoi(comma + 1);
            wcscpy_s(iconPath, val);
        } else {
            wcscpy_s(iconPath, val);
        }
        hIcon = IconFromLocation(iconPath, idx, small);
    }
    RegCloseKey(hk);
    return hIcon;
}

// 路径 → HICON：exbar.cpp GetPathIcon 的通用链路。
HICON GetPathIcon(const std::wstring& rawPath, bool small) {
    std::wstring resolved = ResolveShellPath(rawPath);
    const wchar_t* path = resolved.c_str();
    SHFILEINFOW sfi = {};
    DWORD flags = SHGFI_ICON | (small ? SHGFI_SMALLICON : SHGFI_LARGEICON);

    if (path && wcsncmp(path, L"::{", 3) == 0) {
        // 虚拟 shell 命名空间（此电脑/回收站/网络/库/快速访问…）：
        // "::{GUID}[\子项]" 不是文件系统路径，转 PIDL 取每命名空间准确图标。
        std::wstring guid = ShellGuidFromParsing(path);
        // 区分命名空间**根**与其中的**子项**：根给命名空间标识图标，子项给条目
        // 图标（虚拟子项全是目录，直接系统文件夹图标，避开串解析不稳/回退成根图标）。
        std::wstring ws(path);
        size_t closeBr = ws.find(L'}');
        bool isRootNs = (closeBr != std::wstring::npos);
        for (size_t k = (isRootNs ? closeBr + 1 : 0); isRootNs && k < ws.size(); ++k)
            if (ws[k] != L'\\' && ws[k] != L'/') isRootNs = false;
        if (!isRootNs && !guid.empty()) {
            // 命名空间子项：系统文件夹图标。
            if (HICON h = eb::StockIcon(SIID_FOLDER, small)) return h;
        }
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path, nullptr, &pidl, 0, nullptr)) && pidl) {
            SHFILEINFOW sfiP = {};
            if (SHGetFileInfoW((LPCWSTR)pidl, 0, &sfiP, sizeof(sfiP), flags | SHGFI_PIDL) &&
                sfiP.hIcon) {
                CoTaskMemFree(pidl);
                return sfiP.hIcon;
            }
            CoTaskMemFree(pidl);
        }
        // 回退 1：把 "::{GUID}" 串直接交给 SHGetFileInfo（Win10+ 对多数已知文件夹有效；
        // 此处用不带 SHGFI_PIDL 的干净 flags，避免串被误当 PIDL）。
        if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), flags) && sfi.hIcon) return sfi.hIcon;
        // 回退 2：注册表 CLSID DefaultIcon。
        if (!guid.empty()) {
            if (HICON hReg = LoadDefaultIconForGuid(guid, small)) return hReg;
        }
        // 落到下面的 stock 文件夹兜底。
    } else if (path && *path) {
        // 真实文件系统路径：先转 PIDL，让命名空间扩展（自定义注册的云盘等）返回
        // 真实图标，而不是通用文件系统文件夹图标。
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path, nullptr, &pidl, 0, nullptr)) && pidl) {
            SHFILEINFOW sfiP = {};
            if (SHGetFileInfoW((LPCWSTR)pidl, 0, &sfiP, sizeof(sfiP), flags | SHGFI_PIDL) &&
                sfiP.hIcon) {
                CoTaskMemFree(pidl);
                return sfiP.hIcon;
            }
            CoTaskMemFree(pidl);
        }
        DWORD attr = 0;
        DWORD fa = GetFileAttributesW(path);
        if (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY))
            attr = FILE_ATTRIBUTE_DIRECTORY;
        if (SHGetFileInfoW(path, attr, &sfi, sizeof(sfi), flags) && sfi.hIcon) return sfi.hIcon;
    }

    // 保证控件永不留空：系统 stock 文件夹图标。
    return eb::StockIcon(SIID_FOLDER, small);
}

}  // namespace

HICON Resolve(const std::wstring& path, bool small) {
    if (path.empty()) return nullptr;
    return GetPathIcon(path, small);
}

}  // namespace ico
