// 探针 81：验证"后台线程预热 shell 扩展"能否让 UI 线程的解析变快。
//
// 【要回答什么】探针 79 实测：解析 27 个根 冷 424 ms / 热 138 ms（差 3 倍）。
// 探针 80 实测：STA->STA 走 GIT 拿不到可用代理（第三方扩展不支持封送），
// 所以"把 IShellItem 传回 UI 线程复用"这条路走不通。
//
// 于是只剩另一条：**不传对象，只预热**。让工作线程先把那些 shell 扩展
// 加载进来（解析一次再丢掉），UI 线程随后解析时就走已加载的 DLL 路径。
//
// 关键风险：DLL 只加载一次，但每次 SHCreateItemFromParsingName 仍要
// 走一遍 COM 活动（创建对象实例）。若"对象创建"本身才是大头，
// 预热就救不了 —— **必须实测**。
//
// 本探针对比三种顺序：
//   A. 冷启动直接解析（无预热）          -> 基线
//   B. 后台线程先解析一遍（预热），再解析 -> 是否变快
//   C. 再解析一遍                        -> "热"的极限
//
// 合规：只读 + 自建线程，不发合成输入、不动光标、不抢焦点（R21）。
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

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

const wchar_t* kGuids[] = {
    L"679F85CB-0220-4080-B29B-5540CC05AAB6", L"E88865EA-0E1C-4E20-9AA6-EDCD0212C87C",
    L"018D5C66-4533-4307-9B53-224DE2ED1FE6", L"C802A9DF-4AF3-48A2-B93A-85A8E7A919B9",
    L"C353A004-6DB2-4A25-898B-DAE18690BFDB", L"4965FDA9-E310-41CF-84A3-037BFA358C12",
    L"F4E5CF7F-798C-4691-B384-3FE64603D664", L"13A39D8B-2BCB-4C60-8AF1-C52E61308844",
    L"9D66EF09-59FF-4CD8-9F64-BE40414F16FC", L"265AC47E-276F-4480-99DD-862CC8EF0B8E",
    L"AB1478D1-63BB-49FD-9982-1633E1CEAD7C", L"B449DC5F-C050-4D48-8B2B-1EB68FF2D50D",
    L"44EFAF2E-A5D5-4AC0-9A8B-8333567BFCB7", L"EEEEFCF7-867B-4FA2-9ABD-884CF531B612",
    L"26163A2F-ACC4-4322-B72B-C8772DCF1717", L"0AA5556A-C5C2-45B1-BBBF-1914C8AC6246",
    L"F6845928-94BB-4A95-B3CE-7E6C13CC2E3C",
};

double ParseAll() {
    Timer t;
    for (const wchar_t* g : kGuids) {
        const std::wstring s = L"shell:::{" + std::wstring(g) + L"}";
        IShellItem* it = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(s.c_str(), nullptr, IID_IShellItem,
                                                  reinterpret_cast<void**>(&it))) &&
            it) {
            it->Release();
        }
    }
    return t.Ms();
}

HANDLE gWarmDone = nullptr;

DWORD WINAPI WarmProc(LPVOID) {
    // 工作线程自己的 apartment：与 UI 线程的 STA 不同。
    // 这里只"加载并丢弃"，不跨线程传对象 —— 所以不需要 GIT。
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const double ms = ParseAll();
    printf("  [工作线程] 预热解析 %d 个 = %.2f ms\n",
           static_cast<int>(std::size(kGuids)), ms);
    CoUninitialize();
    SetEvent(gWarmDone);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool warm = (argc > 1 && std::string(argv[1]) == "warm");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // 主线程 STA（与产品一致）

    if (warm) {
        printf("=== B: 先由工作线程预热，主线程再解析 ===\n");
        gWarmDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE th = CreateThread(nullptr, 0, WarmProc, nullptr, 0, nullptr);
        WaitForSingleObject(gWarmDone, 60000);
        if (th) CloseHandle(th);
        CloseHandle(gWarmDone);
        gWarmDone = nullptr;
    } else {
        printf("=== A: 冷启动直接解析（基线）===\n");
    }

    printf("  主线程解析 %d 个 = %.2f ms\n", static_cast<int>(std::size(kGuids)), ParseAll());
    printf("  主线程再解析一遍 = %.2f ms（热极限）\n", ParseAll());

    CoUninitialize();
    return 0;
}