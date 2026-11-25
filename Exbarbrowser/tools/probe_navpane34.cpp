// 探针 34：主页(Home/快速访问) 与 图库(Gallery) 这两个根到底怎么拿到？
//
// 探针 33 用 "shell:::{CLSID}" 解析失败（0x80070002）。这个前缀是"运行对话框"的
// 语法，不是 SHCreateItemFromParsingName 的语法。这里把几种可能一次性测完：
//
//   A. "::{CLSID}"                  —— shell 命名空间解析名的标准写法
//   B. "shell:::{CLSID}"            —— 对照（预期失败，已验证）
//   C. CoCreateInstance(CLSID) + QI IShellFolder + 取根 PIDL
//   D. SHParseDisplayName("::{CLSID}") + SHCreateItemFromIDList
//   E. 对照：FOLDERID_ComputerFolder 的解析名长什么样
//
// 判定：能拿到 IShellItem 且显示名接近"主页"/"Home"/"快速访问"，即成功。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shlguid.h>

#include <cstdio>
#include <string>

static const GUID kCLSID_Home = {0x679f85cb, 0x0220, 0x4080,
                                 {0xb2, 0x9b, 0x55, 0x40, 0xcc, 0x05, 0xaa, 0xb6}};
static const GUID kCLSID_Gallery = {0xe88865ea, 0x0e1c, 0x4e20,
                                    {0x9a, 0xa6, 0xed, 0xcd, 0x02, 0x12, 0xc8, 0x7c}};

static std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

static void ClsidText(REFGUID g, wchar_t* out, size_t cch) {
    // 用官方 StringFromGUID2：产出 {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}。
    // 【为什么不用 swprintf】mingw 的 swprintf 在混用 %lX 与字面 { 时实测只写出 "{"，
    // 导致后面所有解析名都成了 "::{", 白白浪费一轮实验（探针 34 首跑即如此）。
    StringFromGUID2(g, out, static_cast<int>(cch));
}

static void ShowItem(const char* tag, IShellItem* it) {
    if (!it) {
        printf("  %-46s = (null)\n", tag);
        return;
    }
    PWSTR d = nullptr;
    PWSTR p = nullptr;
    it->GetDisplayName(SIGDN_NORMALDISPLAY, &d);
    it->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p);
    printf("  %-46s OK  name=\"%s\"  parsing=%s\n", tag, U8(d ? d : L"?").c_str(),
           U8(p ? p : L"?").c_str());
    if (d) CoTaskMemFree(d);
    if (p) CoTaskMemFree(p);
}

static void TryParsingForms(REFGUID clsid, const wchar_t* label) {
    wchar_t g[64];
    ClsidText(clsid, g, 64);
    wchar_t buf[160];
    printf("\n-- %s %s --\n", U8(label).c_str(), U8(g).c_str());

    // A. ::{CLSID}
    swprintf(buf, 160, L"shell:::%ls", g);
    IShellItem* it = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(buf, nullptr, IID_IShellItem,
                                             reinterpret_cast<void**>(&it));
    printf("  A SHCreateItemFromParsingName(\"%s\") hr=0x%08lX\n", U8(buf).c_str(),
           static_cast<unsigned long>(hr));
    ShowItem("  -> item", it);
    if (it) it->Release();
    it = nullptr;

    // B. shell:::{CLSID}
    swprintf(buf, 160, L"shell:::%ls", g);
    hr = SHCreateItemFromParsingName(buf, nullptr, IID_IShellItem,
                                     reinterpret_cast<void**>(&it));
    printf("  B SHCreateItemFromParsingName(\"%s\") hr=0x%08lX\n", U8(buf).c_str(),
           static_cast<unsigned long>(hr));
    if (it) it->Release();
    it = nullptr;

    // D. SHParseDisplayName + SHCreateItemFromIDList
    swprintf(buf, 160, L"shell:::%ls", g);
    PIDLIST_ABSOLUTE pidl = nullptr;
    hr = SHParseDisplayName(buf, nullptr, &pidl, 0, nullptr);
    printf("  D SHParseDisplayName(\"%s\") hr=0x%08lX pidl=%p\n", U8(buf).c_str(),
           static_cast<unsigned long>(hr), static_cast<void*>(pidl));
    if (SUCCEEDED(hr) && pidl) {
        hr = SHCreateItemFromIDList(pidl, IID_IShellItem, reinterpret_cast<void**>(&it));
        printf("    SHCreateItemFromIDList hr=0x%08lX\n", static_cast<unsigned long>(hr));
        ShowItem("    -> item", it);
        if (it) it->Release();
        CoTaskMemFree(pidl);
    }

    // C. CoCreateInstance + IShellFolder（命名空间壳对象）
    IShellFolder* folder = nullptr;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IShellFolder,
                          reinterpret_cast<void**>(&folder));
    printf("  C CoCreateInstance(IID_IShellFolder) hr=0x%08lX ptr=%p\n",
           static_cast<unsigned long>(hr), static_cast<void*>(folder));
    if (SUCCEEDED(hr) && folder) {
        // 壳命名空间对象的"自己"通常表现为 IPersistFolder，尝试取 display name
        IPersistFolder* pf = nullptr;
        if (SUCCEEDED(folder->QueryInterface(IID_IPersistFolder,
                                             reinterpret_cast<void**>(&pf))) &&
            pf) {
            printf("    QI IPersistFolder OK\n");
            pf->Release();
        }
        folder->Release();
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    printf("=== 对照：已知文件夹的解析名写法 ===\n");
    {
        IShellItem* pc = nullptr;
        SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                             reinterpret_cast<void**>(&pc));
        ShowItem("FOLDERID_ComputerFolder", pc);
        if (pc) pc->Release();
    }

    TryParsingForms(kCLSID_Home, L"主页/快速访问");
    TryParsingForms(kCLSID_Gallery, L"图库");

    printf("\n=== 结论 ===\n");
    printf("  任一路径 OK 且名字是 主页/Home/快速访问 => 该根可直接 AppendRoot\n");
    printf("  全部失败 => 这两个是 explorer 专属视图，NSTC 里无法作为根，需用已知文件夹凑\n");
    fflush(stdout);
    return 0;
}