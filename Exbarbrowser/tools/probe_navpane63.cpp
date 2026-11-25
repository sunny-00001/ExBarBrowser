// 探针 63：验证「注册表 NameSpace + System.IsPinnedToNameSpaceTree」这条规则
// 能否**完整**还原 explorer 左栏的 27 项。
//
// 【已建立的规则（来自注册表与枚举）】
//   Explorer 左栏组3 的 14 项 = 注册表 Desktop\NameSpace 中
//   System.IsPinnedToNameSpaceTree == 1 的第三方扩展，**按显示名码位升序**。
//   不显示的：{5A3E91C7} Everything 结果、{A17F3C21} OpenList —— 都**没有**该值。
//   组1：主文件夹(Home 679F85CB) / 图库(E88865EA) / TAN - 个人(OneDrive 个人)
//   组2：桌面/Downloads/文档/图片/音乐/视频/tools
//   组3 末尾：此电脑(ComputerFolder) / 库(UsersLibraries) / 网络(NetworkFolder)
//
// 【本探针】把上面每个 CLSID 解析成 IShellItem，打印它的**真实显示名**，
//   用来确认"哪个 CLSID 对应 Explorer 里的哪一行"。
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

std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

// 解析 "shell:::{CLSID}" 并取显示名。
void Show(const char* tag, const wchar_t* clsid) {
    wchar_t parsing[128] = L"";
    swprintf_s(parsing, L"shell:::{%s}", clsid);
    IShellItem* it = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(parsing, nullptr, IID_IShellItem,
                                             reinterpret_cast<void**>(&it));
    if (FAILED(hr) || !it) {
        printf("  %-14s {%.8s} hr=0x%08lX (不可解析)\n", tag, U8(clsid).c_str(),
               (unsigned long)hr);
        return;
    }
    wchar_t disp[256] = L"";
    PWSTR p = nullptr;
    if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) && p) {
        wcsncpy_s(disp, p, _TRUNCATE);
        CoTaskMemFree(p);
    }
    wchar_t path[400] = L"";
    p = nullptr;
    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        wcsncpy_s(path, p, _TRUNCATE);
        CoTaskMemFree(p);
    }
    printf("  %-14s %.8s -> %-34s %s\n", tag, U8(clsid).c_str(), U8(disp).c_str(),
           U8(path).c_str());
    it->Release();
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    printf("===== 组1 =====\n");
    Show("Home主文件夹", L"679F85CB-0220-4080-B29B-5540CC05AAB6");
    Show("Gallery图库", L"E88865EA-0E1C-4E20-9AA6-EDCD0212C87C");
    Show("OneDrive个人", L"018D5C66-4533-4307-9B53-224DE2ED1FE6");
    Show("MSGraphHome", L"f874310e-b6b7-47dc-bc84-b9e6b38f5903");

    printf("===== 组3：pinned=1 的第三方扩展（HKCU NameSpace） =====\n");
    Show("天翼云U盘", L"0AA5556A-C5C2-45B1-BBBF-1914C8AC6246");
    Show("Nextcloud-drive01", L"13a39d8b-2bcb-4c60-8af1-c52e61308844");
    Show("qiniu七牛云", L"26163A2F-ACC4-4322-B72B-C8772DCF1717");
    Show("OpenListAccount", L"265AC47E-276F-4480-99DD-862CC8EF0B8E");
    Show("VFS", L"44efaf2e-a5d5-4ac0-9a8b-8333567bfcb7");
    Show("Local", L"4965fda9-e310-41cf-84a3-037bfa358c12");
    Show("OneSync", L"9d66ef09-59ff-4cd8-9f64-be40414f16fc");
    Show("Roaming", L"ab1478d1-63bb-49fd-9982-1633e1cead7c");
    Show("Shortlink", L"b449dc5f-c050-4d48-8b2b-1eb68ff2d50d");
    Show("Links", L"c353a004-6db2-4a25-898b-dae18690bfdb");
    Show("pan123123云盘", L"C802A9DF-4AF3-48A2-B93A-85A8E7A919B9");
    Show("WPS云盘", L"EEEEFCF7-867B-4FA2-9ABD-884CF531B612");
    Show("Nextcloud-43131", L"f4e5cf7f-798c-4691-b384-3fe64603d664");
    Show("MiCloud小米云盘", L"F6845928-94BB-4A95-B3CE-7E6C13CC2E3C");

    printf("===== 对照：pinned 无值（Explorer 不显示） =====\n");
    Show("Everything结果", L"5A3E91C7-2D64-4B18-9F0A-6E7C4B21D835");
    Show("OpenList裸", L"A17F3C21-4B9A-4D2E-9E1D-2A6B84F05C13");

    printf("===== 组3 末尾（HKLM pinned=1） =====\n");
    Show("UsersLibraries库", L"031E4825-7B94-4dc3-B131-E946B44C8DD5");

    CoUninitialize();
    return 0;
}
