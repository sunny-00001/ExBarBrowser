// 探针 61：用**经典 IShellFolder::GetDisplayNameOf(childPidl)** 取 Home 子项的名字。
//
// 【背景】
//   探针 58/60：Home 自己的子项枚举出 45 项，但显示名全是空串。
//   探针 60 走的是 SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY) 和
//   IShellItem::GetDisplayName，两条都空。经典写法应该用
//   **父 folder 的 GetDisplayNameOf(childPidl, ...)**（拿到的是 folder 自己
//   对这个 child 的命名），而不是从 PIDL 反查。
//
// 【要回答】Home 的子项里有没有 explorer 左栏组2 的
//   桌面/Downloads/文档/图片/音乐/视频/**tools**。
//
// 合规：只读 shell 命名空间，不发合成输入（R21）。
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>

namespace {

std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

// STRRET -> std::wstring（STRRET_WSTR 时 pOleStr 由 CoTaskMem 分配）。
std::wstring RetW(const STRRET& sr) {
    if (sr.uType != STRRET_WSTR || !sr.pOleStr) return L"";
    std::wstring s = sr.pOleStr;
    CoTaskMemFree(sr.pOleStr);
    return s;
}

std::wstring NameViaFolder(IShellFolder* sf, PCUITEMID_CHILD child) {
    STRRET sr{};
    sr.uType = STRRET_WSTR;
    if (FAILED(sf->GetDisplayNameOf(child, SIGDN_NORMALDISPLAY, &sr))) return L"";
    return RetW(sr);
}

std::wstring PathViaFolder(IShellFolder* sf, PCUITEMID_CHILD child) {
    STRRET sr{};
    sr.uType = STRRET_WSTR;
    if (FAILED(sf->GetDisplayNameOf(child, SIGDN_FILESYSPATH, &sr))) return L"";
    return RetW(sr);
}

IShellFolder* OwnFolder(IShellItem* item) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetIDListFromObject(item, &pidl)) || !pidl) return nullptr;
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD childLast = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent),
                                 &childLast);
    ILFree(pidl);
    if (FAILED(hr) || !parent || !childLast) {
        if (parent) parent->Release();
        return nullptr;
    }
    IShellFolder* self = nullptr;
    hr = parent->BindToObject(childLast, nullptr, IID_IShellFolder,
                              reinterpret_cast<void**>(&self));
    parent->Release();
    if (FAILED(hr) || !self) return nullptr;
    return self;
}

void Dump(const wchar_t* parsing, const char* tag) {
    printf("===== %s =====\n", tag);
    IShellItem* it = nullptr;
    SHCreateItemFromParsingName(parsing, nullptr, IID_IShellItem,
                                reinterpret_cast<void**>(&it));
    if (!it) { printf("  (no item)\n"); return; }
    IShellFolder* sf = OwnFolder(it);
    if (!sf) { printf("  (no own folder)\n"); it->Release(); return; }
    IEnumIDList* eidl = nullptr;
    HRESULT hr = sf->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &eidl);
    printf("  EnumObjects hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && eidl) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        int n = 0;
        while (n < 60 && SUCCEEDED(eidl->Next(1, &pidl, nullptr))) {
            PCUITEMID_CHILD child = ILFindLastID(pidl);
            std::wstring nm = NameViaFolder(sf, child);
            std::wstring path = PathViaFolder(sf, child);
            printf("    [%2d] %-34s %s\n", n, U8(nm.c_str()).c_str(), U8(path.c_str()).c_str());
            ++n;
            ILFree(pidl);
            pidl = nullptr;
        }
        eidl->Release();
    }
    sf->Release();
    it->Release();
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Dump(L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}", "Home 主页");
    Dump(L"shell:::{E88865EA-0E1C-4E20-9AA6-EDCD0212C87C}", "Gallery 图库");
    CoUninitialize();
    return 0;
}
