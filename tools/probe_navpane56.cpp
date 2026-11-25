// 探针 56：用 SHGetIDListFromObject + SHBindToParent 枚举命名空间的子项。
//
// 【为什么换路径】探针 55 证明 IShellItem::BindToHandler(BHID_SFUIObject) 对
//   这些命名空间根一律返回 E_NOTIMPL(0x80004001)，一个都枚举不出来。
//   shell 枚举命名空间子项的**标准**路径是：
//     SHGetIDListFromObject(item) -> 绝对 PIDL
//     SHBindToParent(pidl, IID_IShellFolder, &parent)  -> 父 IShellFolder
//     parent->EnumObjects(...)                        -> 子项
//
// 【目的】explorer 左栏组2 = 桌面/Downloads/文档/图片/音乐/视频/tools，
//   前 6 项是标准已知文件夹，"tools" 多出来（疑似快速访问固定项 pinned）。
//   枚举 Home / QuickAccess 的子项，看谁带得出 "tools"。
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

// 用 SHBindToParent 枚举 item 的子项。
void DumpChildren(IShellItem* item, const char* tag) {
    printf("  -- children(%s) --\n", tag);
    if (!item) { printf("     (item == null)\n"); return; }
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHGetIDListFromObject(item, &pidl);
    if (FAILED(hr) || !pidl) {
        printf("     SHGetIDListFromObject hr=0x%08lX\n", (unsigned long)hr);
        return;
    }
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD childLast = nullptr;
    // SHBindToParent 需要第 4 个参数（最后一个子项的指针），传 nullptr 即可。
    hr = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent),
                         &childLast);
    if (FAILED(hr) || !parent) {
        printf("     SHBindToParent hr=0x%08lX\n", (unsigned long)hr);
        ILFree(pidl);
        return;
    }
    IEnumIDList* eidl = nullptr;
    hr = parent->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &eidl);
    printf("     EnumObjects hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && eidl) {
        PIDLIST_ABSOLUTE child = nullptr;
        int n = 0;
        while (n < 50 && SUCCEEDED(eidl->Next(1, &child, nullptr))) {
            wchar_t disp[256] = L"";
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(child, SIGDN_NORMALDISPLAY, &p)) && p) {
                wcsncpy_s(disp, p, _TRUNCATE);
                CoTaskMemFree(p);
            }
            wchar_t path[512] = L"";
            SHGetPathFromIDListW(child, path);
            printf("       [%2d] %-40s %s\n", n, U8(disp).c_str(), U8(path).c_str());
            ++n;
            ILFree(child);
            child = nullptr;
        }
        eidl->Release();
    }
    parent->Release();
    ILFree(pidl);
}

IShellItem* FromParsing(const wchar_t* p) {
    IShellItem* it = nullptr;
    SHCreateItemFromParsingName(p, nullptr, IID_IShellItem, reinterpret_cast<void**>(&it));
    return it;
}

IShellItem* FromKnown(const GUID* id) {
    IShellItem* it = nullptr;
    SHGetKnownFolderItem(*id, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                         reinterpret_cast<void**>(&it));
    return it;
}

void DumpTag(const char* tag, IShellItem* it) {
    DumpChildren(it, tag);
    if (it) it->Release();
}

// QuickAccess: FOLDERID_QuickAccess = {B455F46E-E7AF-4EF5-BB9E-7D7F6503338F}
const GUID kQuickAccess = {0xb455f46e, 0xe7af, 0x4ef5, {0xbb, 0x9e, 0x7d, 0x7f, 0x65, 0x03, 0x33, 0x8f}};

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    printf("===== a) Home 主文件夹 {679F85CB} =====\n");
    DumpTag("Home", FromParsing(L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}"));
    printf("===== b) Gallery 图库 {E88865EA} =====\n");
    DumpTag("Gallery", FromParsing(L"shell:::{E88865EA-0E1C-4E20-9AA6-EDCD0212C87C}"));
    printf("===== c) OneDrive 个人 {018D5C66} =====\n");
    DumpTag("OneDrivePersonal",
            FromParsing(L"shell:::{018D5C66-4533-4307-9B53-224DE2ED1FE6}"));
    printf("===== d) FOLDERID_QuickAccess =====\n");
    DumpTag("QuickAccess", FromKnown(&kQuickAccess));
    printf("===== e) FOLDERID_Profile =====\n");
    DumpTag("Profile", FromKnown(&FOLDERID_Profile));

    CoUninitialize();
    return 0;
}
