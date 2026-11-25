// 探针 79：量化导航树重建的两个可疑重复点。
//
// 【要回答什么】日志实测：27 个根在 [+344] 已发现完毕，但树到 [+594] 才建好
// —— 中间 204 ms 不在"发现"里，而在"把 RootSpec 变成 IShellItem 再 AppendRoot"。
// 两处怀疑：
//   A. RebuildRoots 对 27 个根**逐个** SHCreateItemFromParsingName；
//      而发现层刚才已经把同一批 CLSID 解析过一遍（解析完就 Release 丢掉了）
//      —— 同一批第三方 shell 扩展被绑定两次。
//   B. UpdateSeparatorLines 在同一次重建里被调 2~3 次（日志可见"组界 0 个"连打两遍）。
//
// 本探针把 A 拆开计时：逐个 CLSID 解析 vs 逐个 AppendRoot。
// 不碰产品进程，只复刻调用序列。
//
// 合规：只读 + 自建隐藏窗口，不发合成输入、不动光标、不抢焦点（R21）。
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClsidHome[] = L"679F85CB-0220-4080-B29B-5540CC05AAB6";
constexpr wchar_t kClsidGallery[] = L"E88865EA-0E1C-4E20-9AA6-EDCD0212C87C";

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

// 产品发现层产出的 18 个 CLSID 根（取自实测日志）+ 组0 两个 + 组1 七个 Known。
// 这里只关心"解析"的代价，名字不参与。
struct Root {
    int kind;  // 0=clsid 1=known 2=path
    std::wstring value;
};

std::vector<Root> SampleRoots() {
    std::vector<Root> v;
    v.push_back({0, kClsidHome});
    v.push_back({0, kClsidGallery});
    v.push_back({0, L"018D5C66-4533-4307-9B53-224DE2ED1FE6"});
    v.push_back({1, L"Desktop"});
    v.push_back({1, L"Downloads"});
    v.push_back({1, L"Documents"});
    v.push_back({1, L"Pictures"});
    v.push_back({1, L"Music"});
    v.push_back({1, L"Videos"});
    v.push_back({2, L"E:\\ExBarBrowser\\tools"});
    const wchar_t* clsid[] = {
        L"C802A9DF-4AF3-48A2-B93A-85A8E7A919B9", L"C353A004-6DB2-4A25-898B-DAE18690BFDB",
        L"4965FDA9-E310-41CF-84A3-037BFA358C12", L"F4E5CF7F-798C-4691-B384-3FE64603D664",
        L"13A39D8B-2BCB-4C60-8AF1-C52E61308844", L"9D66EF09-59FF-4CD8-9F64-BE40414F16FC",
        L"265AC47E-276F-4480-99DD-862CC8EF0B8E", L"AB1478D1-63BB-49FD-9982-1633E1CEAD7C",
        L"B449DC5F-C050-4D48-8B2B-1EB68FF2D50D", L"44EFAF2E-A5D5-4AC0-9A8B-8333567BFCB7",
        L"EEEEFCF7-867B-4FA2-9ABD-884CF531B612", L"26163A2F-ACC4-4322-B72B-C8772DCF1717",
        L"0AA5556A-C5C2-45B1-BBBF-1914C8AC6246", L"F6845928-94BB-4A95-B3CE-7E6C13CC2E3C",
    };
    for (const wchar_t* g : clsid) v.push_back({0, g});
    v.push_back({1, L"ComputerFolder"});
    v.push_back({1, L"UsersLibraries"});
    v.push_back({1, L"NetworkFolder"});
    return v;
}

IShellItem* Resolve(const Root& r) {
    IShellItem* item = nullptr;
    if (r.kind == 0) {
        const std::wstring s = L"shell:::{" + r.value + L"}";
        SHCreateItemFromParsingName(s.c_str(), nullptr, IID_IShellItem,
                                    reinterpret_cast<void**>(&item));
    } else if (r.kind == 1) {
        // 复刻产品的 KnownByName 查表（这里只覆盖本探针用到的名字）。
        const GUID* id = nullptr;
        if (r.value == L"Desktop") id = &FOLDERID_Desktop;
        else if (r.value == L"Downloads") id = &FOLDERID_Downloads;
        else if (r.value == L"Documents") id = &FOLDERID_Documents;
        else if (r.value == L"Pictures") id = &FOLDERID_Pictures;
        else if (r.value == L"Music") id = &FOLDERID_Music;
        else if (r.value == L"Videos") id = &FOLDERID_Videos;
        else if (r.value == L"ComputerFolder") id = &FOLDERID_ComputerFolder;
        else if (r.value == L"UsersLibraries") id = &FOLDERID_UsersLibraries;
        else if (r.value == L"NetworkFolder") id = &FOLDERID_NetworkFolder;
        if (id) {
            SHGetKnownFolderItem(*id, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                                 reinterpret_cast<void**>(&item));
        }
    } else {
        SHCreateItemFromParsingName(r.value.c_str(), nullptr, IID_IShellItem,
                                    reinterpret_cast<void**>(&item));
    }
    return item;
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const auto roots = SampleRoots();
    printf("根总数 %d\n\n", static_cast<int>(roots.size()));

    // ---- A1：逐个解析（RebuildRoots 里做的那一遍）----
    std::vector<IShellItem*> items;
    {
        Timer t;
        for (const auto& r : roots) {
            IShellItem* it = Resolve(r);
            if (it) items.push_back(it);
        }
        printf("A1) 解析 %d 个 RootSpec -> IShellItem   %7.2f ms\n",
               static_cast<int>(items.size()), t.Ms());
    }
    for (auto* it : items) it->Release();
    items.clear();

    // 再跑一次（DLL 已加载），看"热"成本
    {
        Timer t;
        for (const auto& r : roots) {
            IShellItem* it = Resolve(r);
            if (it) items.push_back(it);
        }
        printf("A1) 第二次（DLL 已加载）                 %7.2f ms\n", t.Ms());
    }

    // ---- A2：真实 NSTC 上逐个 AppendRoot ----
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Probe79Host";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, L"Probe79Host", L"", WS_OVERLAPPEDWINDOW, 0, 0, 240, 600,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!host) {
        printf("建宿主窗口失败\n");
        CoUninitialize();
        return 1;
    }

    INameSpaceTreeControl* nstc = nullptr;
    CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                     IID_INameSpaceTreeControl, reinterpret_cast<void**>(&nstc));
    if (!nstc) {
        printf("CoCreate INameSpaceTreeControl 失败\n");
        DestroyWindow(host);
        CoUninitialize();
        return 1;
    }
    RECT rc{0, 0, 240, 600};
    const DWORD style = NSTCS_HASEXPANDOS | NSTCS_ROOTHASEXPANDO | NSTCS_FULLROWSELECT |
                        NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL | NSTCS_NOINFOTIP |
                        NSTCS_TABSTOP | NSTCS_BORDER;
    {
        Timer t;
        HRESULT hr = nstc->Initialize(host, &rc, static_cast<NSTCSTYLE>(style));
        printf("\nB0) NSTC Initialize                   %7.2f ms (hr=0x%08lX)\n", t.Ms(),
               static_cast<unsigned long>(hr));
    }

    {
        Timer t;
        for (auto* it : items) {
            nstc->AppendRoot(it, SHCONTF_FOLDERS, NSTCRS_VISIBLE, nullptr);
        }
        printf("B1) AppendRoot × %d                    %7.2f ms\n",
               static_cast<int>(items.size()), t.Ms());
    }
    {
        Timer t;
        nstc->RemoveAllRoots();
        printf("B2) RemoveAllRoots                     %7.2f ms\n", t.Ms());
    }
    {
        Timer t;
        for (auto* it : items) {
            nstc->AppendRoot(it, SHCONTF_FOLDERS, NSTCRS_VISIBLE, nullptr);
        }
        printf("B3) 再 AppendRoot × %d（第二次）        %7.2f ms\n",
               static_cast<int>(items.size()), t.Ms());
    }

    printf("\n【判读】A1 是 RebuildRoots 里重新解析的钱；B1/B3 是真正建树。\n");
    printf("        发现层已经解析过同一批 CLSID（探针 72 的 2b 段），\n");
    printf("        所以 A1 属于**重复劳动**，可复用发现层的 IShellItem。\n");

    for (auto* it : items) it->Release();
    nstc->Release();
    DestroyWindow(host);
    CoUninitialize();
    return 0;
}