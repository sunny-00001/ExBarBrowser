// AI 服务子进程管理实现。设计与生命周期说明见 aichat_service.h。
//
// 【include 顺序】winsock2.h 必须在 windows.h 之前 —— 反过来会把
// windows.h 里的旧版 winsock.h 带进来，两者符号冲突。
// 头文件 aichat_service.h 自带 <windows.h>，所以这里先把 winsock2 拉进来。
#include <winsock2.h>
#include <ws2tcpip.h>

#include "aichat_service.h"

#include "config.h"      // Config::Dir / LogsDir：把新分层目录告诉 Node 服务
#include "log.h"
#include "text_file.h"   // tf::ToExtended（长路径前缀）

#include <bcrypt.h>
#include <string>

namespace aisvc {
namespace {

PROCESS_INFORMATION g_pi{};
HANDLE g_job = nullptr;
unsigned g_port = 0;
bool g_started = false;
std::wstring g_reason;
std::wstring g_httpToken;
std::wstring g_scriptPath;
std::wstring g_exeDir;
std::wstring g_pipeName;
std::wstring g_token;

// 本地 HTTP token：进程内生成，经环境传给 node，经 URL 传给 WebView。
std::wstring MakeHttpToken() {
    unsigned char bytes[16] = {};
    if (BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        static const wchar_t* kHex = L"0123456789abcdef";
        std::wstring out;
        out.reserve(32);
        for (unsigned char b : bytes) {
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 0x0F]);
        }
        return out;
    }
    // BCrypt 不可用时用实例熵兜底；token 只防浏览器跨源请求，不用于身份鉴权。
    return L"fb" + std::to_wstring(GetCurrentProcessId())
           + std::to_wstring(GetTickCount64());
}

// 找一个空闲端口：让系统 bind 0，读回实际端口，立刻关掉。
// 竞态窗口（关掉到服务再 bind 之间）存在但极短；服务那边 EADDRINUSE 会明确报错，
// 比固定 8792 让第二个实例直接启动失败要好。
unsigned PickFreePort() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { WSACleanup(); return 0; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;   // 让系统选

    unsigned port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        sockaddr_in got{};
        int len = sizeof(got);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&got), &len) == 0) {
            port = ntohs(got.sin_port);
        }
    }
    closesocket(s);
    WSACleanup();
    return port;
}

// 从注册表/常见位置找 node.exe。找不到就用 PATH 上的 "node"。
std::wstring FindNode() {
    // 1) 显式覆盖（测试/便携部署用）
    wchar_t buf[MAX_PATH * 2] = L"";
    if (GetEnvironmentVariableW(L"EXBAR_NODE", buf, _countof(buf)) > 0 && buf[0]) {
        return buf;
    }
    // 2) 项目自带（aichat/ 下的 node 目录，若有）
    // 3) 交给 CreateProcess 按 PATH 解析 —— 传 L"node.exe" 即可。
    return L"node.exe";
}

std::wstring QuoteArg(const std::wstring& s) {
    // CreateProcess 的命令行解析规则：含空格时整体加引号，内部 " 前加反斜杠。
    // 我们的参数（路径/管道名）都不含引号，简单加引号足够；但保留转义以免将来出错。
    if (s.find(L' ') == std::wstring::npos && s.find(L'\t') == std::wstring::npos) return s;
    std::wstring out = L"\"";

    size_t backslashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

bool MakeKillOnCloseJob() {
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (!g_job) return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    return SetInformationJobObject(g_job, JobObjectExtendedLimitInformation,
                                   &info, sizeof(info)) != 0;
}

// 主动通知 node 服务退出：发一个带本地 token 的 POST /api/shutdown。
// 之前没有这条通路，node 只能靠"父进程 PID 消失"的 2s 轮询判断该退，
// 而父进程此刻正卡在下面的 WaitForSingleObject 里等它 —— 两边互等白耗 1.5s。
// fire-and-forget：只 send 不读响应，回环 connect 通常 <1ms；失败就走强杀兜底。
void NotifyShutdown() {
    if (!g_started || !g_port || g_httpToken.empty()) return;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        // 回环连接，给个短发送超时防止极端情况下挂住关闭流程。
        DWORD timeoutMs = 200;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<unsigned short>(g_port));
        if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            // token 是 hex ASCII，逐字转窄即可。
            std::string token;
            token.reserve(g_httpToken.size());
            for (wchar_t c : g_httpToken) token.push_back(static_cast<char>(c & 0x7F));
            const std::string req =
                std::string("POST /api/shutdown HTTP/1.1\r\n")
                + "Host: 127.0.0.1:" + std::to_string(g_port) + "\r\n"
                + "X-ExBar-Token: " + token + "\r\n"
                + "Content-Length: 0\r\n"
                + "Connection: close\r\n"
                + "\r\n";
            send(s, req.data(), static_cast<int>(req.size()), 0);
        }
        closesocket(s);
    }
    WSACleanup();
}

}  // namespace

unsigned PickFreePortForTest() { return PickFreePort(); }

bool Start(const std::wstring& scriptPath, const std::wstring& exeDir,
           const std::wstring& pipeName, const std::wstring& token,
           unsigned* outPort, std::wstring* reason) {
    // 进程已经退出时不能继续返回旧端口：那会让面板导航到死端口。
    if (g_started && Current().running) {
        if (outPort) *outPort = g_port;
        return true;
    }
    if (g_started) Stop();
    g_reason.clear();

    if (scriptPath.empty()) {
        g_reason = L"服务脚本路径为空";
        if (reason) *reason = g_reason;
        return false;
    }
    {
        // 走 tf::ToExtended：GetFileAttributesW 与其它文件 API 一样受 MAX_PATH 限制，
        // 而用户可能把程序装在很深的目录里。扩展前缀让长路径也能判存在。
        const DWORD attr = GetFileAttributesW(tf::ToExtended(scriptPath).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            g_reason = L"找不到服务脚本：" + scriptPath;
            if (reason) *reason = g_reason;
            return false;
        }
    }

    const unsigned port = PickFreePort();
    if (!port) {
        g_reason = L"无法分配空闲端口";
        if (reason) *reason = g_reason;
        return false;
    }

    const std::wstring node = FindNode();
    const std::wstring cmd = QuoteArg(node) + L" " + QuoteArg(scriptPath);

    // 环境变量：把"该连哪个窗口"直接告诉服务（见 aichat_service.h 的说明）。
    // 用 CREATE_UNICODE_ENVIRONMENT + 手工拼块，因为要改子进程的环境而非本进程的。
    wchar_t portStr[16] = L"";
    swprintf(portStr, 16, L"%u", port);

    // 只设我们需要的几项，其余继承。做法：复制本进程环境块再追加，
    // 避免 SetEnvironmentVariableW 改动本进程（会影响其它子系统）。
    std::wstring envBlock;
    {
        LPWCH cur = GetEnvironmentStringsW();
        if (cur) {
            const wchar_t* p = cur;
            while (*p) {
                const size_t n = wcslen(p);
                envBlock.append(p, n);
                envBlock.push_back(L'\0');
                p += n + 1;
            }
            FreeEnvironmentStringsW(cur);
        }
    }
    auto addEnv = [&envBlock](const std::wstring& kv) {
        envBlock.append(kv);
        envBlock.push_back(L'\0');
    };
    g_httpToken = MakeHttpToken();
    addEnv(L"EXBAR_PIPE=" + pipeName);
    addEnv(L"EXBAR_TOKEN=" + token);
    addEnv(L"EXBAR_HTTP_TOKEN=" + g_httpToken);
    addEnv(L"AGENTCHAT_PORT=" + std::wstring(portStr));
    // 数据目录分层（见 config.h）：Node 服务据此找到 data\aichat.json /
    // models.json / mcp.json 和 logs\sessions\。显式传入而非让 Node 猜 exe 目录，
    // 这样 profile 隔离对 C++ 与 Node 两侧是同一套根。
    addEnv(L"EXBAR_DATA_DIR=" + Config::Get().Dir());
    addEnv(L"EXBAR_LOGS_DIR=" + Config::Get().LogsDir());
    addEnv(L"EXBAR_RUNTIME_DIR=" + Config::Get().RuntimeDir());
    // 让服务在父进程消失时自己退出（Job Object 之外的第三重保险）。
    addEnv(L"EXBAR_PARENT_PID=" + std::to_wstring(GetCurrentProcessId()));
    envBlock.push_back(L'\0');   // 双重结尾

    g_scriptPath = scriptPath;
    g_exeDir = exeDir;
    g_pipeName = pipeName;
    g_token = token;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    // 不要让子进程弹控制台窗口：它是后台服务，日志走文件。
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring cmdMutable = cmd;   // CreateProcessW 需要可写缓冲
    const BOOL ok = CreateProcessW(
        nullptr, cmdMutable.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        envBlock.data(),
        exeDir.empty() ? nullptr : exeDir.c_str(),
        &si, &g_pi);

    if (!ok) {
        g_reason = L"启动 node 失败（err=" + std::to_wstring(GetLastError())
                   + L"）。请确认已安装 Node.js 并在 PATH 中。";
        if (reason) *reason = g_reason;
        return false;
    }

    // 先塞进 Job Object 再放行 —— 反过来的话，在"已启动但还没入 job"的窗口期
    // 本进程若崩掉，子进程会变孤儿。
    if (MakeKillOnCloseJob()) {
        AssignProcessToJobObject(g_job, g_pi.hProcess);
    } else {
        LogDbg(L"[aisvc] Job Object 创建失败（err=%lu）；仅靠显式 Stop 收尾",
               GetLastError());
    }

    ResumeThread(g_pi.hThread);
    CloseHandle(g_pi.hThread);
    g_pi.hThread = nullptr;

    g_port = port;
    g_started = true;
    if (outPort) *outPort = port;

    LogDbg(L"[aisvc] node 已启动 pid=%lu port=%u script=%s",
           g_pi.dwProcessId, port, scriptPath.c_str());
    return true;
}

void Stop() {
    if (!g_started) return;

    // 先通知服务主动退出（关监听端口 + process.exit），让它几乎立即收尾。
    // 这样既保留"优雅关闭 socket、避免 EADDRINUSE"的初衷，又不必再等原来
    // 那 1.5 秒 —— 之前服务只认父进程 PID 消失，而父进程正卡在这里等它，
    // 形成互等，必然吃满超时。通知失败时下面的强杀路径仍是兜底。
    const ULONGLONG tStop = GetTickCount64();
    NotifyShutdown();
    LogDbg(L"[aisvc] notify sent +%llu ms", GetTickCount64() - tStop);

    // 给了通知后只需短暂等待服务自己退出；超时才强杀。
    // 强杀后仍等它真正结束（保留原来的 EADDRINUSE 兜底）。
    if (g_pi.hProcess) {
        if (WaitForSingleObject(g_pi.hProcess, 600) == WAIT_TIMEOUT) {
            TerminateProcess(g_pi.hProcess, 0);
            WaitForSingleObject(g_pi.hProcess, 1000);
        }
        LogDbg(L"[aisvc] process reaped +%llu ms", GetTickCount64() - tStop);
        CloseHandle(g_pi.hProcess);
        g_pi.hProcess = nullptr;
    }
    if (g_job) {
        // 关 job 会连带杀掉还活着的成员（KILL_ON_JOB_CLOSE）。
        CloseHandle(g_job);
        g_job = nullptr;
    }
    g_started = false;
    g_port = 0;
    g_httpToken.clear();
    g_scriptPath.clear();
    g_exeDir.clear();
    g_pipeName.clear();
    g_token.clear();
    LogDbg(L"[aisvc] node 服务已停止");
}

bool Restart(unsigned* outPort, std::wstring* reason) {
    if (g_scriptPath.empty()) {
        if (reason) *reason = L"服务尚未启动过，无法重启";
        return false;
    }
    const std::wstring script = g_scriptPath;
    const std::wstring dir = g_exeDir;
    const std::wstring pipe = g_pipeName;
    const std::wstring token = g_token;
    Stop();
    return Start(script, dir, pipe, token, outPort, reason);
}

Status Current() {
    Status st;
    st.pid = g_pi.dwProcessId;
    st.port = g_port;
    st.reason = g_reason;
    if (g_started && g_pi.hProcess) {
        DWORD code = 0;
        if (GetExitCodeProcess(g_pi.hProcess, &code) && code == STILL_ACTIVE) {
            st.running = true;
        } else {
            st.reason = L"服务进程已退出（code=" + std::to_wstring(code) + L"）";
        }
    } else if (g_started) {
        st.running = true;
    }
    return st;
}

bool Enabled() { return Current().running; }

const std::wstring& HttpToken() { return g_httpToken; }

bool WaitReady(int timeoutMs) {
    if (!g_started || !g_port) return false;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    const int stepMs = 120;
    bool ready = false;
    for (int waited = 0; waited < timeoutMs; waited += stepMs) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(static_cast<unsigned short>(g_port));
            if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                ready = true;
            }
            closesocket(s);
        }
        if (ready) break;
        Sleep(stepMs);
    }
    WSACleanup();
    return ready;
}

}  // namespace aisvc