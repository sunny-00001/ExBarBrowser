// 探针 69：把注册表 NameSpace 里的**每一项**原样列出来，
// 并对每一项给出：注册表 (default) 值 / 有没有 FolderValueFlags / shell 显示名 / 路径。
//
// 【目的】把"explorer 组3 的 14 项到底从哪来"这件事一次钉死：
//   哪些键在 HKCU、哪些在 HKLM、哪些带 FolderValueFlags（= 真正的 shell 命名空间扩展）、
//   显示名各是什么。这样组3 的构造规则就不需要再猜。
//
// 【顺带】核对 Home 命名空间子项里哪些是"文件夹"（用来把"最近使用的文件"剔掉）。
//
// 合规：只读注册表 + 只读 shell 命名空间，不发合成输入（R21）。
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string U8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

// 去掉花括号并大写。
std::wstring Norm(const std::wstring& s) {
    std::wstring t = s;
    if (t.size() >= 2 && t.front() == L'{' && t.back() == L'}') t = t.substr(1, t.size() - 2);
    for (auto& c : t) c = towupper(c);
    return t;
}

bool HasFolderValueFlags(const std::wstring& guid) {
    const std::wstring path = L"CLSID\\{" + guid + L"}\\ShellFolder";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, value = 0, size = sizeof(value);
    const LONG r = RegQueryValueExW(key, L"FolderValueFlags", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_DWORD;
}

std::wstring ShellFolderAttrs(const std::wstring& guid) {
    const std::wstring path = L"CLSID\\{" + guid + L"}\\ShellFolder";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return L"";
    DWORD type = 0, value = 0, size = sizeof(value);
    const LONG r = RegQueryValueExW(key, L"Attributes", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return L"";
    wchar_t buf[32];
    swprintf_s(buf, L"attr=0x%08lX", (unsigned long)value);
    return buf;
}

std::wstring InprocServer(const std::wstring& guid) {
    const std::wstring path = L"CLSID\\{" + guid + L"}\\InprocServer32";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return L"";
    wchar_t buf[512] = L"";
    DWORD len = sizeof(buf);
    const LONG r = RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &len);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return L"";
    return buf;
}

void DumpKey(HKEY root, const char* label, const wchar_t* sub) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        printf("[%s] 键不存在\n", label);
        return;
    }
    printf("===== %s =====\n", label);
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = L"";
        DWORD len = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) ==
            ERROR_NO_MORE_ITEMS) {
            break;
        }
        const std::wstring guid = Norm(name);

        // 子键的 (default) 值 = 注册名
        std::wstring defval;
        {
            HKEY sub2 = nullptr;
            if (RegOpenKeyExW(key, name, 0, KEY_READ, &sub2) == ERROR_SUCCESS) {
                wchar_t buf[512] = L"";
                DWORD sz = sizeof(buf);
                if (RegQueryValueExW(sub2, nullptr, nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS) {
                    defval = buf;
                }
                RegCloseKey(sub2);
            }
        }

        // 显示名 + 路径
        std::wstring disp, fspath;
        const std::wstring parsing = L"shell:::{" + guid + L"}";
        IShellItem* it = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(
            parsing.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&it));
        if (SUCCEEDED(hr) && it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) && p) {
                disp = p;
                CoTaskMemFree(p);
            }
            p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                fspath = p;
                CoTaskMemFree(p);
            }
            it->Release();
        }

        const bool fvf = HasFolderValueFlags(guid);
        printf("  {%.8s} fvf=%d %-22s disp=%-36s\n", U8(guid).c_str(), fvf ? 1 : 0,
               U8(ShellFolderAttrs(guid)).c_str(), U8(disp).c_str());
        printf("           def=%-44s path=%s\n", U8(defval).c_str(), U8(fspath).c_str());
        printf("           inproc=%s\n", U8(InprocServer(guid)).c_str());
    }
    RegCloseKey(key);
}

// ---- Home 命名空间的子项：区分"真文件夹"与"最近使用的文件" -----------------
void DumpHomeChildren() {
    printf("===== Home 子项（带 SFGAO_FOLDER 判定） =====\n");
    const wchar_t* clsid = L"679F85CB-0220-4080-B29B-5540CC05AAB6";
    wchar_t parsing[128] = L"";
    swprintf_s(parsing, L"shell:::{%s}", clsid);
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(parsing, nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item))) || !item) {
        printf("  Home 解析失败\n");
        return;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    SHGetIDListFromObject(item, &pidl);
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD childLast = nullptr;
    IShellFolder* self = nullptr;
    if (pidl && SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder,
                                         reinterpret_cast<void**>(&parent), &childLast))) {
        parent->BindToObject(childLast, nullptr, IID_IShellFolder,
                             reinterpret_cast<void**>(&self));
    }
    if (parent) parent->Release();
    if (pidl) ILFree(pidl);
    if (!self) {
        printf("  取不到 Home 自身\n");
        item->Release();
        return;
    }
    IEnumIDList* eidl = nullptr;
    self->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &eidl);
    int idx = 0;
    if (eidl) {
        PIDLIST_ABSOLUTE child = nullptr;
        while (eidl->Next(1, &child, nullptr) == S_OK) {
            PCUITEMID_CHILD c = ILFindLastID(child);
            STRRET sr{};
            sr.uType = STRRET_WSTR;
            std::wstring name, path;
            if (SUCCEEDED(self->GetDisplayNameOf(c, SIGDN_NORMALDISPLAY, &sr)) &&
                sr.uType == STRRET_WSTR && sr.pOleStr) {
                name = sr.pOleStr;
                CoTaskMemFree(sr.pOleStr);
            }
            sr = STRRET{};
            sr.uType = STRRET_WSTR;
            if (SUCCEEDED(self->GetDisplayNameOf(c, SIGDN_FILESYSPATH, &sr)) &&
                sr.uType == STRRET_WSTR && sr.pOleStr) {
                path = sr.pOleStr;
                CoTaskMemFree(sr.pOleStr);
            }
            // 是否文件夹
            bool isFolder = false;
            {
                IShellItem* ci = nullptr;
                if (SUCCEEDED(self->BindToObject(c, nullptr, IID_IShellItem,
                                                reinterpret_cast<void**>(&ci))) && ci) {
                    SFGAOF attrs = 0;
                    if (SUCCEEDED(ci->GetAttributes(SFGAO_FOLDER, &attrs))) {
                        isFolder = (attrs & SFGAO_FOLDER) != 0;
                    }
                    ci->Release();
                }
            }
            printf("  [%2d] folder=%d name=%-30s path=%s\n", idx, isFolder ? 1 : 0,
                   U8(name).c_str(), U8(path).c_str());
            ILFree(child);
            child = nullptr;
            ++idx;
        }
        eidl->Release();
    }
    self->Release();
    item->Release();
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    DumpKey(HKEY_CURRENT_USER, "HKCU NameSpace",
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace");
    DumpKey(HKEY_LOCAL_MACHINE, "HKLM NameSpace",
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace");
    DumpHomeChildren();

    CoUninitialize();
    return 0;
}
