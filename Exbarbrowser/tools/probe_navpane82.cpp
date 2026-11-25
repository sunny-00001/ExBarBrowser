// 探针 82：量化导航路径上三个"每次导航都跑"的 UI 线程开销。
//
// 【要回答什么】启动慢已经查过。这次是**导航/切标签/关闭**这些高频路径。
// 从代码审查看出三个疑点，都需要数字支撑：
//
//   A. CollectViewStats（src/view_stats.cpp）
//      total>0 且 <=8000 时会遍历**每个 item**：GetItemAt + GetAttributes(SFGAO_FOLDER)。
//      这是 O(N) 次 COM 调用，全在 UI 线程，而 SyncChrome 每次导航都调它。
//      （App::RefreshStats 里只有"路径没变"才节流，路径一变就无条件全量枚举。）
//
//   B. NavPane::SyncTo（src/nav_pane.cpp）
//      每次路径变化都 ItemFromPath -> SHCreateItemFromParsingName（shell 调用），
//      再 SetItemState。切标签时路径必然变，所以每次切都付一次。
//
//   C. ItemCount 是 O(1) 还是 O(N)：如果 ItemCount 本身就要枚举，那
//      "超过 8000 只给总数"这条保护也没省下钱。
//
// 本探针用 IShellItemArray 复刻 A 的算法（不依赖产品的界面），
// 在几个典型规模的目录上分别计时。不碰任何窗口、不碰产品进程。
//
// 合规：只读目录 + 只读 shell 视图；不发合成输入、不动光标、不抢焦点（R21）。
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
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

struct ComPtr {
    IUnknown* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    template <class T>
    T* as() const {
        return reinterpret_cast<T*>(p);
    }
};

// 复刻 CollectViewStats 的分类循环（同一套调用序列）
void MeasureStats(const wchar_t* path) {
    printf("\n--- %ls\n", path);

    // 1) 解析路径 + 绑定 folder（导航层也要做的）
    IShellItem* item = nullptr;
    {
        Timer t;
        const HRESULT hr = SHCreateItemFromParsingName(
            path, nullptr, IID_IShellItem, reinterpret_cast<void**>(&item));
        printf("  1) SHCreateItemFromParsingName  %7.2f ms (hr=0x%08lX)\n", t.Ms(),
               static_cast<unsigned long>(hr));
        if (!item) return;
    }

    IShellFolder* folder = nullptr;
    {
        Timer t;
        if (FAILED(item->BindToHandler(nullptr, BHID_SFObject, IID_IShellFolder,
                                       reinterpret_cast<void**>(&folder))) ||
            !folder) {
            printf("  BindToHandler 失败\n");
            item->Release();
            return;
        }
        printf("  2) BindToHandler(IShellFolder)   %7.2f ms\n", t.Ms());
    }

    // 3) ItemCount —— 判它是不是 O(1)
    int total = 0;
    {
        Timer t;
        // 产品走 IShellView -> IFolderView2，这里改走 IShellFolder::EnumObjects
        // 等价量（都是 shell 侧一次性枚举）。先测 EnumObjects 的"只取总数"成本。
        IEnumIDList* eidl = nullptr;
        if (SUCCEEDED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                                          &eidl)) &&
            eidl) {
            ULONG got = 0;
            ITEMID_CHILD* one = nullptr;
            while (eidl->Next(1, &one, &got) == S_OK && one) {
                ++total;
                CoTaskMemFree(one);
                one = nullptr;
            }
            eidl->Release();
        }
        printf("  3) 纯枚举计数（EnumObjects）  %7.2f ms  -> %d 项\n", t.Ms(), total);
    }

    // 4) 逐项 GetAttributes(SFGAO_FOLDER) —— 产品的分类循环核心
    {
        Timer t;
        IEnumIDList* eidl = nullptr;
        int folders = 0, files = 0, n = 0;
        if (SUCCEEDED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                                          &eidl)) &&
            eidl) {
            ITEMID_CHILD* one = nullptr;
            ULONG got = 0;
            while (eidl->Next(1, &one, &got) == S_OK && one) {
                ++n;
                if (n <= 8000) {   // 复刻产品的 kSplitCap
                    SFGAOF attrs = SFGAO_FOLDER;
                    const HRESULT hr = folder->GetAttributesOf(1, const_cast<PCUITEMID_CHILD*>(&one), &attrs);
                    if (SUCCEEDED(hr) && (attrs & SFGAO_FOLDER))
                        ++folders;
                    else
                        ++files;
                }
                CoTaskMemFree(one);
                one = nullptr;
            }
            eidl->Release();
        }
        printf("  4) 逐项 GetAttributes（分类）  %7.2f ms  （%d 项，上限 8000：文件夹 %d / 文件 %d）\n",
               t.Ms(), n, folders, files);
    }

    if (folder) folder->Release();
    item->Release();
}

}  // namespace

void MeasureCountOnly(const wchar_t* path) {
    printf("\n--- 只取总数（不做分类）: %ls\n", path);
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path, nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item))) || !item) {
        printf("  解析失败\n"); return;
    }
    IShellFolder* folder = nullptr;
    if (FAILED(item->BindToHandler(nullptr, BHID_SFObject, IID_IShellFolder,
                                   reinterpret_cast<void**>(&folder))) || !folder) {
        item->Release(); printf("  绑定失败\n"); return;
    }
    {
        Timer t;
        IEnumIDList* e = nullptr;
        int n = 0;
        if (SUCCEEDED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e)) && e) {
            ITEMID_CHILD* one = nullptr; ULONG got = 0;
            while (e->Next(1, &one, &got) == S_OK && one) { ++n; CoTaskMemFree(one); one = nullptr; }
            e->Release();
        }
        printf("  枚举到总数                %7.2f ms  (%d 项)\n", t.Ms(), n);
    }
    folder->Release();
    item->Release();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    printf("=== 导航路径开销（每个目录各测一次，含冷解析）===\n");

    MeasureStats(L"C:\\Windows\\System32");   // 5000+ 项，产品注释里点名的极端案例
    MeasureStats(L"C:\\Windows");             // 中等
    MeasureStats(L"C:\\Users");

    printf("\n=== 单纯取总数 与 分类 的差价 ===\n");
    MeasureCountOnly(L"C:\\Windows\\System32");

    printf("\n【判读】\n");
    printf("  · 第 3 步是只数个数的底线成本；第 4 步是产品的分类成本。\n");
    printf("  · 若第 4 步在 System32 上达到几十毫秒，那么每次导航都全量枚举\n");
    printf("    就是导航卡顿的直接来源 —— 该改成后台算 + 只算可视区。\n");
    printf("  · 若第 3 步就很贵，说明连只给总数这条保护也没省下钱。\n");

    CoUninitialize();
    return 0;
}