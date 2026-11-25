// 导航树根集的实现：配置文本 <-> RootSpec <-> IShellItem。
// 全部不碰窗口与消息循环，可单独链接、单独单测。见 nav_pane_root.h 的说明。
#include "nav_pane_root.h"

#include "nav_pane_explorer.h"
#include "path_util.h"
#include "str.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cstring>
#include <mutex>
#include <thread>

namespace navpane {

namespace {

// explorer 左栏的两项不是 FOLDERID，而是 shell 命名空间 CLSID。
// 主页（Win10 叫"快速访问"，Win11 22593+ 改名"主页"）与图库。
constexpr wchar_t kClsidHome[] = L"679F85CB-0220-4080-B29B-5540CC05AAB6";
constexpr wchar_t kClsidGallery[] = L"E88865EA-0E1C-4E20-9AA6-EDCD0212C87C";

// 已知文件夹名 -> REFKNOWNFOLDERID。配置里写名字而不是裸 GUID，可读性优先。
struct KnownEntry {
    const wchar_t* name;
    REFKNOWNFOLDERID id;
};
const KnownEntry kKnown[] = {
    {L"ComputerFolder", FOLDERID_ComputerFolder},
    {L"NetworkFolder", FOLDERID_NetworkFolder},
    {L"UsersLibraries", FOLDERID_UsersLibraries},
    {L"Profile", FOLDERID_Profile},
    {L"Desktop", FOLDERID_Desktop},
    {L"Downloads", FOLDERID_Downloads},
    {L"Documents", FOLDERID_Documents},
    {L"Pictures", FOLDERID_Pictures},
    {L"Music", FOLDERID_Music},
    {L"Videos", FOLDERID_Videos},
    {L"RecycleBinFolder", FOLDERID_RecycleBinFolder},
    {L"ControlPanelFolder", FOLDERID_ControlPanelFolder},
};

// mingw 把 REFKNOWNFOLDERID 定义成 const 引用（不是指针），所以这里返回指针：
// 找不到 = nullptr，找到 = 指向表里那个 GUID。调用方 SHGetKnownFolderItem(*id, ...)。
const GUID* KnownByName(const std::wstring& name) {
    for (const auto& e : kKnown) {
        if (str::IEquals(name, e.name)) return &e.id;
    }
    return nullptr;
}

std::wstring GuidText(REFGUID g) {
    wchar_t buf[64] = L"";
    StringFromGUID2(g, buf, 64);
    std::wstring s = buf;
    // 配置里不写花括号，解析时再补 —— "shell:::{GUID}" 需要花括号。
    if (s.size() >= 2 && s.front() == L'{' && s.back() == L'}') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

bool ParseGuid(const std::wstring& text, GUID* out) {
    if (!out) return false;
    std::wstring s = text;
    if (!s.empty() && s.front() == L'{' && s.back() == L'}') s = s.substr(1, s.size() - 2);
    const std::wstring braced = L"{" + s + L"}";
    return SUCCEEDED(CLSIDFromString(braced.c_str(), out));
}

}  // namespace

bool IsKnownFolderName(const std::wstring& name) { return KnownByName(name) != nullptr; }

bool KnownFolderNameForPath(const std::wstring& path, std::wstring* name) {
    if (path.empty() || !name) return false;
    for (const auto& e : kKnown) {
        PWSTR p = nullptr;
        if (FAILED(SHGetKnownFolderPath(e.id, KF_FLAG_DEFAULT, nullptr, &p)) || !p) continue;
        const std::wstring known = p;
        CoTaskMemFree(p);
        if (str::IEquals(known, path)) {
            *name = e.name;
            return true;
        }
    }
    return false;
}

// 内置兜底三项。发现层还没算完 / 算失败时用它 —— 让树立刻有内容。
std::vector<RootSpec> BuiltinFallbackRoots() {
    return {
        {RootSpec::Kind::Clsid, kClsidHome, L"主文件夹", 0},
        {RootSpec::Kind::Clsid, kClsidGallery, L"图库", 0},
        {RootSpec::Kind::Known, L"NetworkFolder", L"网络", 0},
    };
}

// 发现结果的进程级缓存。三态：
//   · 空 + !ready = 后台还在算（此刻 Defaults() 给兜底三项）
//   · 空 +  ready = 算过但没发现任何东西（照样走兜底）
//   · 非空        = 用这份
// 用 mutex 保护而不是"函数内 static 变量 + 一次性初始化"：
// 因为这份值会被**工作线程写、UI 线程读**，一次性初始化表达不了
// "先给兜底、算完再换"这个两阶段语义。
std::mutex g_rootsMu;
std::vector<RootSpec> g_discovered;
bool g_discoverReady = false;

// 预取线程句柄 + 是否启动过。只在 UI 线程读写（PrefetchDefaultsAsync /
// PrefetchShutdown 都由 UI 线程调），所以不需要锁。
std::thread g_prefetchThread;
bool g_prefetchStarted = false;

// 运行期重发现：与启动预取分开线程，避免"已经 ready 之后就不能再算"的
// 一次性语义。g_refreshRunning/g_refreshHook 与 g_rootsMu 共用锁，因为
// 它们和缓存/预取 hook 会被同一批调用点并发读写。
std::thread g_refreshThread;
bool g_refreshRunning = false;
std::function<void()> g_refreshHook;

// 【为什么不用静态对象析构兜底 join】试过，会**死锁**：
// 单测（tests/nav_root_test）的 main 里先 CoUninitialize()，之后才轮到静态析构；
// 那时工作线程还卡在 shell 调用里，join 永远等不到 —— 现象是打印完"全部通过"
// 后进程挂死（实测跑满 11 分钟被强杀）。所以改成：
//   · Defaults() **不**启动线程，只给缓存或兜底（纯读，可在任何线程上安全调）
//   · 线程由 App::OnCreate 显式启动，退出时由 App::OnDestroy 显式 join
// 这样"谁启动谁负责收"，不存在"进程退出时还有 joinable thread"的隐患。
// 唯一残留风险是调用方启动后不 join —— 由 PrefetchShutdown 的义务文档覆盖。

// 发现完成后的通知钩子（在工作线程上被调）。由 g_rootsMu 保护，
// 因为它可能与 Defaults() 的多次调用并发注册。
std::function<void()> g_readyHook;

// 把发现结果写进缓存（带锁）。空结果也算"已就绪" —— 否则每次 Defaults()
// 都会因为"没数据"而想去再算一遍。
void PublishDiscovered(std::vector<RootSpec> roots) {
    std::lock_guard<std::mutex> lock(g_rootsMu);
    g_discovered = std::move(roots);
    g_discoverReady = true;
}

std::vector<RootSpec> RootSpec::Defaults() {
    // explorer 左栏的根集是**随机器变化**的（装了新的网盘就多一项、重命名就换名字），
    // 硬编码一份 GUID 清单在别的机器上必然对不上（探针 63/69 实测：HKCU NameSpace
    // 下 17 个子键里，有 2 个是本项目注入的、explorer 并不显示）。
    // 所以默认走**运行时发现**：把 explorer 左栏的结构原样还原出来。
    //
    // 【为什么不再同步发现 —— 探针 72 实测】
    // 一次完整发现 = 461 ms（冷）/ 170 ms（热）：
    //   · 15 次 SHCreateItemFromParsingName ≈ 279/100 ms（每次都**重新绑定**
    //     第三方网盘 shell 扩展；DLL 已加载也无法避免这层 COM 往返）
    //   · Home 命名空间枚举 19 项 + 19 次 GetFileAttributes ≈ 178/68 ms
    //   · 注册表那两步合计只有 1 ms —— 贵的全在 shell 侧
    // 同步跑在 UI 线程上 = 启动头半秒窗口卡着不出来（消息循环不转）。
    //
    // 另外 Defaults() 会被反复调到：Create() 一次 + 每次 SetRoots(空) 一次，
    // 后者挂在 ApplyNavPane -> ApplyPaneChrome -> 每次焦点变化/切标签/设置落盘。
    // 实测日志 34 次启动累计 188 条"根清单"（≈ 5.5 次/启动）—— 4.5 次是纯浪费。
    //
    // 现在的口径（**纯读，不启动任何线程**）：
    //   · 缓存已就绪 -> 直接给缓存（<1 ms）
    //   · 未就绪     -> 给内置三项（0 ms）
    // 线程由 App::OnCreate 显式启动（PrefetchDefaultsAsync），App::OnDestroy join。
    // 【为什么 Defaults() 自己不启动线程 —— 实测踩过的死锁】它会被单测直接调用
    // （tests/nav_root_test 不带窗口、不跑消息循环）。若在这里起线程，进程退出时
    // 静态析构要 join 那个还卡在 shell 调用里的线程，而 main 已经 CoUninitialize ——
    // 现象是打印完"全部通过"后挂死（实测跑满 11 分钟被强杀）。
    // 让"谁启动谁负责收"就干净了：Defaults() 变成与线程无关的纯查询。
    {
        std::lock_guard<std::mutex> lock(g_rootsMu);
        if (g_discoverReady) {
            return g_discovered.empty() ? BuiltinFallbackRoots() : g_discovered;
        }
    }
    return BuiltinFallbackRoots();
}

bool PrefetchDefaultsAsync(std::function<void()> onReady) {
    // 【g_readyHook 的读写必须都在 g_rootsMu 内】
    // 写入方是 UI 线程，读取方是工作线程（在发现完成后取本地副本再调）。
    // 若写入不在锁内，与工作线程的带锁读构成**数据竞争**（std::function 不是
    // 线程安全的）。实测：第一版把这两处写在了锁外，注释却写着"由 g_rootsMu
    // 保护" —— 注释与实现不一致，按注释审查会漏掉。
    bool alreadyDone = false;
    {
        std::lock_guard<std::mutex> lock(g_rootsMu);
        if (g_discoverReady) {
            alreadyDone = true;
        } else if (g_prefetchStarted) {
            // 正在算：把新回调接上（调用方可能比第一次那个更晚注册）。
            if (onReady) g_readyHook = std::move(onReady);
            return false;
        } else {
            g_prefetchStarted = true;
            g_readyHook = std::move(onReady);
        }
    }
    // 【回调必须在锁外调】它可能间接调到 PrefetchTake（同样要拿这把锁），
    // 在锁内调就是不可重入死锁。
    if (alreadyDone) {
        if (onReady) onReady();
        return false;
    }

    // 【必须 join 旧线程再覆盖】g_prefetchThread 只启动一次，这里只是形式上的保险。
    if (g_prefetchThread.joinable()) g_prefetchThread.join();

    g_prefetchThread = std::thread([] {
        // COM 单元是线程私有的：不能借用 UI 线程已初始化的那个，
        // 否则 shell 扩展在本线程上拿不到有效 apartment（表现是解析全失败）。
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<RootSpec> found;
        if (SUCCEEDED(hr)) {
            found = DiscoverExplorerRoots();
            CoUninitialize();
        } else if (hr == RPC_E_CHANGED_MODE) {
            // 已经是别的单元模式（不该发生，但别因此丢掉发现）：照样试一次。
            found = DiscoverExplorerRoots();
        }
        PublishDiscovered(std::move(found));
        // 回调取本地副本再调：免得回调里又调 Prefetch* 时被同一把锁卡住。
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(g_rootsMu);
            hook = g_readyHook;
        }
        if (hook) hook();
    });
    return true;
}

bool PrefetchTake(std::vector<RootSpec>* roots) {
    if (!roots) return false;
    std::lock_guard<std::mutex> lock(g_rootsMu);
    if (!g_discoverReady) return false;
    *roots = g_discovered;
    return !g_discovered.empty();
}

void PrefetchShutdown(unsigned timeoutMs) {
    // 先停注册表监听：它的回调会启动发现线程，退出途中不能再生新的。
    StopRootChangeWatch();

    if (g_prefetchThread.joinable()) {
        // 【timeoutMs 是"等多久开始怀疑"，不是"等多久就放弃"】
        // std::thread 没有可中断的 join：C++ 标准不提供把线程从阻塞的 shell 调用里
        // 拽出来的手段。所以"超时后不 join 直接返回"是**做不到**的 —— 那会留下
        // 一个 joinable 的 std::thread，它的析构函数直接 std::terminate()。
        // 第一版写了"超时就跳过 join"，是**假超时**：循环确实会退出，但紧接着的
        // join() 照样无限等，参数看着有用其实没用，属于误导性实现。
        //
        // 现在诚实表达语义：先轮询等发现完成（这段能超时），然后再无条件 join。
        // 实测算下来发现只要 461 ms；真卡到 timeoutMs 说明 shell 扩展挂了，
        // 此时 join 会等到进程被系统收掉 —— 这是**已知且无法在应用层解决**的边界。
        if (timeoutMs > 0) {
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            for (;;) {
                {
                    std::lock_guard<std::mutex> lock(g_rootsMu);
                    if (g_discoverReady) break;
                }
                if (GetTickCount64() >= deadline) {
                    LogDbg(L"[navpane] 根集发现超过 %u ms 仍未完成，仍在等它收工"
                           L"（std::thread 无法中断，见 PrefetchShutdown 的说明）",
                           timeoutMs);
                    break;
                }
                Sleep(10);
            }
        }
        g_prefetchThread.join();
    }
    if (g_refreshThread.joinable()) g_refreshThread.join();
}

bool RefreshDefaultsAsync(std::function<void()> onReady) {
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(g_rootsMu);
        // 启动发现还在跑时不要开第二份：它算的就是同一份清单。
        if (!g_discoverReady && g_prefetchStarted) {
            if (onReady) g_readyHook = std::move(onReady);
            return false;
        }
        if (g_refreshRunning) {
            if (onReady) g_refreshHook = std::move(onReady);
            return false;
        }
        g_refreshRunning = true;
        g_refreshHook = std::move(onReady);
        start = true;
    }
    if (!start) return false;
    if (g_refreshThread.joinable()) g_refreshThread.join();

    g_refreshThread = std::thread([] {
        // 与启动预取同一个理由：shell 扩展需要自己的 COM 单元。
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = false;
        if (SUCCEEDED(hr)) {
            PublishDiscovered(DiscoverExplorerRoots());
            CoUninitialize();
            ok = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            PublishDiscovered(DiscoverExplorerRoots());
            ok = true;
        }
        if (!ok) LogDbg(L"[navpane] 根集重发现失败：CoInitializeEx 0x%08lX",
                        static_cast<unsigned long>(hr));
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(g_rootsMu);
            hook = g_refreshHook;
            g_refreshHook = nullptr;
            g_refreshRunning = false;   // 先放行，回调里可能再请求一次重发现
        }
        if (hook) hook();
    });
    return true;
}

bool RootSpec::Parse(const std::wstring& text, RootSpec* out) {
    if (!out) return false;
    const size_t colon = text.find(L':');
    if (colon == std::wstring::npos) return false;
    std::wstring kind = text.substr(0, colon);
    std::wstring value = text.substr(colon + 1);
    // 去掉两端空白（手改配置最容易带空格）
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) value.pop_back();
    if (value.empty()) return false;

    out->label.clear();
    if (str::IEquals(kind, L"clsid")) {
        GUID g{};
        if (!ParseGuid(value, &g)) return false;
        out->kind = Kind::Clsid;
        out->value = GuidText(g);
        return true;
    }
    if (str::IEquals(kind, L"known")) {
        // 名字拼错就当作无效配置（而不是静默变成"桌面"）
        if (!KnownByName(value)) return false;
        out->kind = Kind::Known;
        out->value = value;
        return true;
    }
    if (str::IEquals(kind, L"path")) {
        out->kind = Kind::Path;
        out->value = value;
        return true;
    }
    return false;
}

std::wstring RootSpec::Format(const RootSpec& spec) {
    switch (spec.kind) {
        case Kind::Clsid:
            return L"clsid:" + spec.value;
        case Kind::Known:
            return L"known:" + spec.value;
        case Kind::Path:
            return L"path:" + spec.value;
    }
    return L"";
}

std::vector<unsigned short> PidlBytes(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return {};
    const ULONG bytes = ILGetSize(pidl);
    if (!bytes || (bytes % sizeof(unsigned short)) != 0) return {};
    std::vector<unsigned short> out(bytes / sizeof(unsigned short));
    std::memcpy(out.data(), pidl, bytes);
    return out;
}

std::vector<unsigned short> PidlBytes(IShellItem* item) {
    if (!item) return {};
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetIDListFromObject(item, &pidl)) || !pidl) return {};
    std::vector<unsigned short> out = PidlBytes(pidl);
    ILFree(pidl);
    return out;
}

IShellItem* ItemFromPidlBytes(const std::vector<unsigned short>& bytes) {
    if (bytes.empty()) return nullptr;
    IShellItem* item = nullptr;
    SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(bytes.data()), IID_IShellItem,
                           reinterpret_cast<void**>(&item));
    return item;
}

IShellItem* ItemFromRootSpec(const RootSpec& spec) {
    IShellItem* item = nullptr;
    switch (spec.kind) {
        case RootSpec::Kind::Clsid: {
            // 【必须用 shell::: 前缀】实测（探针 34）：
            //   "shell:::{679F85CB-...}" -> S_OK（主页）
            //   "::{679F85CB-...}"       -> 0x80070057（失败）
            // 图库两种都行，主页只认 shell:::。统一用 shell:::{GUID}。
            const std::wstring spec2 = L"shell:::{" + spec.value + L"}";
            SHCreateItemFromParsingName(spec2.c_str(), nullptr, IID_IShellItem,
                                        reinterpret_cast<void**>(&item));
            break;
        }
        case RootSpec::Kind::Known: {
            const GUID* id = KnownByName(spec.value);
            if (id) {
                SHGetKnownFolderItem(*id, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                                     reinterpret_cast<void**>(&item));
            }
            break;
        }
        case RootSpec::Kind::Path:
            SHCreateItemFromParsingName(spec.value.c_str(), nullptr, IID_IShellItem,
                                        reinterpret_cast<void**>(&item));
            break;
    }
    return item;
}

// 见 nav_pane_root.h 的理由。规则只有两条，都要能被单测精确钉住：
//   1) 裸 `::{GUID}`（命名空间解析名）-> 补 `shell:`。主页只认这种写法。
//   2) 其余（文件系统路径、已带 shell: 的串）-> 原样返回，绝不重复加前缀。
//
// 【实现已下沉到 pathutil::ToParsingName】理由见 path_util.h 的说明：
// 同一个 bug 也出现在导航侧（会话恢复把裸 ::{GUID} 直接喂给 BrowseToObject，
// 右窗格 hr=0x80070057 —— 用户看到"右栏空白"）。两个入口各写一份判据，
// 迟早有一边漏改。这里保留名字只为对上游 API 稳定，规则本身只有一份。
std::wstring NormalizeParsingName(const std::wstring& raw) {
    return pathutil::ToParsingName(raw);
}

IShellItem* ItemFromPath(const std::wstring& path) {
    if (path.empty()) return nullptr;
    // 【必须先归一化】调用方给的可能是裸 ::{GUID}（导航层习惯这么存），
    // 裸形式对"主页"会 E_INVALIDARG。这是"补 shell: 前缀"规则的第二个入口，
    // 与 PathOfItem(反向) 用同一个 NormalizeParsingName，不许各写一份。
    const std::wstring spec = NormalizeParsingName(path);
    IShellItem* item = nullptr;
    SHCreateItemFromParsingName(spec.c_str(), nullptr, IID_IShellItem,
                                reinterpret_cast<void**>(&item));
    return item;
}

}  // namespace navpane