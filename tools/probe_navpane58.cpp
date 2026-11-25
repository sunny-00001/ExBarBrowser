// 探针 58：拿 IShellItem **自身**的 IShellFolder，枚举它的子项。
//
// 【方法论修正】探针 56 用 SHBindToParent 拿到的是**父级**的 IShellFolder，
//   所以 Home/Gallery/OneDrive 全都枚举出"桌面命名空间"的同一份列表 ——
//   那是父级，不是自己。要拿自己的 folder，标准路径是：
//       SHGetIDListFromObject(item) -> pidl
//       SHBindToParent(pidl, IID_IShellFolder, &parent, &childLast)
//       parent->BindToObject(childLast, IID_IShellFolder, &self)   <<< 关键这一步
//
// 【目的】explorer 左栏组2 = 桌面/Downloads/文档/图片/音乐/视频/tools。
//   前 6 项是标准已知文件夹，"tools" 多出来。组2 很可能就是
//   **某个命名空间根（主页/Home?）自己的子项集合**。逐一试。
//
// 合规：只读 shell 命名空间，不发合成输入（R21）。
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <knownfolders.h>

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

// 拿 item **自身**的 IShellFolder（BindToObject 路径）。失败返回 nullptr。
IShellFolder* OwnFolder(IShellItem* item) {
    if (!item) return nullptr;
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
    // IShellFolder::BindToObject(pidl, pbc, riid, ppv) —— 4 个参数。
    hr = parent->BindToObject(childLast, nullptr, IID_IShellFolder,
                              reinterpret_cast<void**>(&self));
    parent->Release();
    if (FAILED(hr) || !self) return nullptr;
    return self;
}

void DumpChildren(IShellItem* item, const char* tag) {
    printf("  -- children(%s) --\n", tag);
    IShellFolder* sf = OwnFolder(item);
    if (!sf) { printf("     (no own IShellFolder)\n"); return; }
    IEnumIDList* eidl = nullptr;
    HRESULT hr = sf->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &eidl);
    printf("     EnumObjects hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && eidl) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        int n = 0;
        while (n < 45 && SUCCEEDED(eidl->Next(1, &pidl, nullptr))) {
            wchar_t disp[256] = L"";
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &p)) && p) {
                wcsncpy_s(disp, p, _TRUNCATE);
                CoTaskMemFree(p);
            }
            wchar_t path[400] = L"";
            SHGetPathFromIDListW(pidl, path);
            printf("       [%2d] %-36s %s\n", n, U8(disp).c_str(), U8(path).c_str());
            ++n;
            ILFree(pidl);
            pidl = nullptr;
        }
        eidl->Release();
    }
    sf->Release();
}

IShellItem* FromParsing(const wchar_t* p) {
    IShellItem* it = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(p, nullptr, IID_IShellItem,
                                             reinterpret_cast<void**>(&it));
    printf("  parse(%s) hr=0x%08lX item=%p\n", U8(p).c_str(), (unsigned long)hr, (void*)it);
    return it;
}

IShellItem* FromKnown(const GUID* id) {
    IShellItem* it = nullptr;
    HRESULT hr = SHGetKnownFolderItem(*id, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                                      reinterpret_cast<void**>(&it));
    printf("  known item=%p hr=0x%08lX\n", (void*)it, (unsigned long)hr);
    return it;
}

void Tag(const char* tag, IShellItem* it) {
    DumpChildren(it, tag);
    if (it) it->Release();
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    printf("===== A) Home 主文件夹 =====\n");
    Tag("Home", FromParsing(L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}"));
    printf("===== B) Gallery 图库 =====\n");
    Tag("Gallery", FromParsing(L"shell:::{E88865EA-0E1C-4E20-9AA6-EDCD0212C87C}"));
    printf("===== C) FOLDERID_Profile =====\n");
    Tag("Profile", FromKnown(&FOLDERID_Profile));
    printf("===== D) FOLDERID_Desktop =====\n");
    Tag("Desktop", FromKnown(&FOLDERID_Desktop));
    printf("===== E) FOLDERID_UsersLibraries =====\n");
    Tag("Libraries", FromKnown(&FOLDERID_UsersLibraries));
    printf("===== F) FOLDERID_ComputerFolder =====\n");
    Tag("Computer", FromKnown(&FOLDERID_ComputerFolder));
    printf("===== G) FOLDERID_NetworkFolder =====\n");
    Tag("Network", FromKnown(&FOLDERID_NetworkFolder));
    printf("===== H) FOLDERID_UserProfile(=Profile) 之外的 3rd =====\n");
    Tag("Downloads", FromKnown(&FOLDERID_Downloads));

    CoUninitialize();
    return 0;
}
