// ExBarBrowser 入口：DPI、COM、路径转发、命令行解析。
//
// 形态（360 文件夹路线）：
//   · 每个窗口是本进程自己的顶层窗口，explorer 完全不参与；
//   · 视图用 IExplorerBrowser 在本进程内托管真实 Shell 视图；
//   · 默认启动：已有窗口就把路径转发过去开新标签并激活它；没有就新开进程。
//     显式多开（--new-window / EXBARBROWSER_MULTI）则不查找、不转发，直接新起一个。
//   · 【没有单实例锁】不存在"已在运行 → 拒绝启动"的分支；多实例是允许的常态。
#include <cstddef>   // offsetof
#include <cstring>   // memcpy

#include "app.h"
#include "config.h"
#include "icon_resolve.h"
#include "text_file.h"
#include "str.h"

namespace {

void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto ctx = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(
        GetProcAddress(u32, "SetProcessDpiAwarenessContext"));
    if (ctx && ctx(reinterpret_cast<HANDLE>(-4) /* PER_MONITOR_AWARE_V2 */)) return;

    if (HMODULE sh = LoadLibraryW(L"shcore.dll")) {
        auto awareness = reinterpret_cast<HRESULT(WINAPI*)(int)>(
            GetProcAddress(sh, "SetProcessDpiAwareness"));
        if (awareness && SUCCEEDED(awareness(2 /* PER_MONITOR_DPI_AWARE */))) return;
    }
    SetProcessDPIAware();
}

std::wstring StripQuotes(std::wstring s) {
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') return s.substr(1, s.size() - 2);
    return s;
}

// 是否强制开启 AI 通道：--ai 命令行开关，或 EXBARBROWSER_AI 环境变量（"0"/空=否）。
// 这是进程内启动开关，只改内存标志、不写 settings.json —— 测试实例关闭不会污染用户设置。
bool AiForceRequested() {
    wchar_t buf[8] = L"";
    const DWORD n = GetEnvironmentVariableW(id::kAiForceEnv, buf, _countof(buf));
    if (n > 0 && n < _countof(buf) && buf[0] != L'\0' && buf[0] != L'0') return true;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (str::IEquals(argv[i], id::kAiForceArg)) { found = true; break; }
        }
        LocalFree(argv);
    }
    return found;
}

// 显式要求新实例：--new-window 命令行（"在新窗口打开"命令用它启动子进程）。
// 不读环境变量 —— MULTI 的语义是"换独立 profile"，由调用方单独处理。
bool NewWindowRequested() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (str::IEquals(argv[i], id::kNewWindowArg)) { found = true; break; }
        }
        LocalFree(argv);
    }
    return found;
}

// 相对路径（`ExBarBrowser.exe sub`）直接交给 shell 解析必然失败：
// 先按当前目录补全成绝对路径。命名空间（::{} / shell:）原样保留。
std::wstring NormalizePath(const std::wstring& p) {
    if (p.empty() || p.rfind(L"::", 0) == 0 || p.rfind(L"shell:", 0) == 0) return p;
    wchar_t buf[32768] = L"";
    DWORD n = GetFullPathNameW(p.c_str(), _countof(buf), buf, nullptr);
    if (n == 0 || n >= _countof(buf)) return p;
    return std::wstring(buf);
}

std::wstring InitialPath() {
    std::wstring path;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a.empty()) continue;
            if (str::IEquals(a.c_str(), L"/select") && i + 1 < argc) {
                const std::wstring full = NormalizePath(StripQuotes(argv[++i]));
                // 盘根/UNC 根没有父级：直接打开它自己（以前会退化成"打开主目录"）
                path = ParentOf(full);
                if (path.empty()) path = full;
                break;
            }
            if (a.rfind(L"/select,", 0) == 0) {
                const std::wstring full = NormalizePath(StripQuotes(a.substr(8)));
                path = ParentOf(full);
                if (path.empty()) path = full;
                break;
            }
            if (a[0] == L'/' || a[0] == L'-') continue;  // 其他开关忽略（含 --ai）
            path = NormalizePath(StripQuotes(a));
            break;
        }
        LocalFree(argv);
    }

    // 没给路径时返回空：由 App 决定"恢复上次会话"还是"打开主目录"
    return path;
}

// 查找已有主窗口。timeoutMs 只用于覆盖"首实例还在建窗口"的启动竞态；
// 它**不是**单实例判定的代价 —— 找不到就立即新开进程，绝不在这里等候或阻断。
HWND FindMainWindow(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (HWND h = FindWindowW(kMainClass, nullptr)) return h;
        if (GetTickCount64() >= deadline) return nullptr;
        Sleep(25);
    }
}

// 已有实例：把路径转发过去，由它开新标签（360 行为）。
// 必须用 SendMessageTimeout 并校验处理结果：目标卡在模态框时 SendMessage 会无限挂起，
// 而"无条件返回 true"会让调用方误以为已转发而退出（用户点了打开却什么都没发生）。
bool ForwardToExisting(const std::wstring& path, DWORD findTimeoutMs) {
    HWND target = FindMainWindow(findTimeoutMs);
    if (!target) return false;

    // 【按实际长度发送，不用固定 1024 截断】
    //
    // 固定数组 + lstrcpynW 会把深路径（>1024 字符）与前缀较长的命名空间路径
    // 静默截断，被截断的路径在接收端解析失败 —— 表现为"从命令行/资源管理器
    // 打开深目录，主窗口没反应"，且没有任何报错。
    // 接收端（app_wnd.cpp 的 WM_COPYDATA）本来就是按 cbData 与 NUL 长度读的，
    // 所以这里只需把 payload 扩成"头 + 实际字符数"的连续缓冲。
    const size_t pathChars = path.size() + 1;   // 含 NUL
    std::vector<BYTE> buf(offsetof(CopyDataPayload, path) + pathChars * sizeof(wchar_t));
    auto* payload = reinterpret_cast<CopyDataPayload*>(buf.data());
    payload->magic = kCopyDataMagic;
    memcpy(payload->path, path.c_str(), pathChars * sizeof(wchar_t));

    COPYDATASTRUCT cd{};
    cd.dwData = kCopyDataMagic;
    cd.cbData = static_cast<DWORD>(buf.size());
    cd.lpData = payload;

    DWORD_PTR result = 0;
    LRESULT sent = SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd),
                                       SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
    // 目标处理成功时 WM_COPYDATA 返回 TRUE（app.cpp 的处理器就是这样返回的）
    if (sent == 0 || result != TRUE) return false;

    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
    return true;
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    EnableDpiAwareness();

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    ApplyProcessDarkMode(SystemIsDark());  // 必须在任何窗口创建之前
    // 必须是 OLE 初始化（内部就是 CoInitializeEx(STA) + 剪贴板/拖放的消息过滤）：
    //  · 文件复制/剪切走 OleSetClipboard/OleFlushClipboard；
    //  · IExplorerBrowser 的 shell 视图要 RegisterDragDrop，拖放才有反应。
    // 只调 CoInitializeEx 时这两条链路会静默失败（复制没反应、拖不进文件）。
    if (FAILED(OleInitialize(nullptr))) return 1;

    const std::wstring path = InitialPath();

    // 显式多开（--new-window）或独立 profile（EXBARBROWSER_MULTI）：不查找、不转发。
    const bool multi = GetEnvironmentVariableW(id::kMultiInstanceEnv, nullptr, 0) > 0;
    const bool forceNew = multi || NewWindowRequested();

    // 测试隔离（优先级4）：显式 profile 优先；只给 MULTI=1 时落到固定子目录。
    // 必须在任何 Config::Get().s()/Load() 之前 —— Config 只在第一次访问时读盘，
    // 晚设目录会把用户的 settings.json 读进测试实例，再写回去就是污染。
    wchar_t profileBuf[MAX_PATH * 2] = L"";
    const DWORD profileLen = GetEnvironmentVariableW(id::kProfileEnv, profileBuf,
                                                     static_cast<DWORD>(_countof(profileBuf)));
    if (profileLen > 0 && profileLen < _countof(profileBuf)) {
        Config::Get().SetProfileDir(profileBuf);
    } else if (multi) {
        Config::Get().SetProfileDir(tf::Join(tf::ExeDir(), L"data\\profile-multi"));
    }

    // 强制开启 AI 通道的启动开关（--ai / EXBARBROWSER_AI）：只置进程内标志，
    // 不写 settings.json。必须在 App::Run（读 AiChannelEnabled）之前设好。
    Config::Get().SetAiForceEnabled(AiForceRequested());

    if (!forceNew) {
        // 默认行为：已开着的窗口就激活它（顺带把路径作为新标签打开）；
        // 没有窗口则继续往下走，正常新开 —— 这里等不到窗口不会报错，更不会阻断。
        //
        // 400ms 只用来吃掉"双击第二次时首实例还没建好窗口"的窄窗口期；
        // 超时就当没有实例，直接新起一个（多实例是允许的）。
        if (ForwardToExisting(path, 400)) {
            ico::ClearCache();
            OleUninitialize();
            return 0;
        }
    }

    int rc = App::Get().Run(hInst, path);
    ico::ClearCache();
    OleUninitialize();
    return rc;
}
