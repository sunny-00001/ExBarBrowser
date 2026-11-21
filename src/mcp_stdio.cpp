// MCP over stdio 桥接实现。接口说明见 mcp_stdio.h。
//
// 【职责边界】这个模块**只搬字节**，不解释协议：
//   · 不解析方法名、不认识任何命令；
//   · 不做鉴权决策（令牌从 ai_server 取，判定在 ai_server 里）；
//   · 不生成错误响应（错误一律由 ai_server 生成，格式与管道口完全一致）。
// 唯一一处"加工"是注入令牌 —— 因为 MCP 协议里没有令牌这回事，
// 而管道通道要求带令牌；不注入的话，MCP 客户端每一条请求都会被判 bad_token。
#include "mcp_stdio.h"

#include "ai_server.h"
#include "json.h"
#include "log.h"
#include "str.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

namespace mcpstdio {
namespace {

// 等管道实例出现的最长时间。
// 为什么需要等：管道的**实例**是服务线程启动后才建的（ServeLoop 里
// CreateNamedPipeW），StartServer 返回时名字才登记、实例可能还没建好。
// 这时立刻 CreateFileW 会得到 ERROR_FILE_NOT_FOUND。
constexpr int kConnectRetryMs = 2000;
constexpr int kConnectRetryStepMs = 20;

std::atomic<bool> g_running{false};
std::thread g_thread;
HWND g_main = nullptr;

// 桥接线程的两个同步点：
//   g_ready    —— 线程已经记下自己的线程 ID（Stop 要用它去打断阻塞读）
//   g_finished —— 线程已经收尾（Stop 等它退出）
HANDLE g_ready = nullptr;
HANDLE g_finished = nullptr;
DWORD g_thread_id = 0;

// 带缓冲的行读取器：管道与 stdin 都是"读一块、按行切"，所以两边共用它。
struct LineReader {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string acc;

    // 读一整行（不含换行）。返回 false = 对端关闭或读失败。
    bool readLine(std::string* out) {
        for (;;) {
            const size_t nl = acc.find('\n');
            if (nl != std::string::npos) {
                *out = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!out->empty() && out->back() == '\r') out->pop_back();
                return true;
            }
            char buf[4096];
            DWORD n = 0;
            if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) return false;
            acc.append(buf, n);
        }
    }
};

// 写一行（补上换行）。返回 false = 写失败（对端已断）。
bool WriteLine(HANDLE h, const std::string& line) {
    std::string out = line;
    out += '\n';
    DWORD w = 0;
    return WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &w, nullptr) &&
           w == out.size();
}

// 这一行是不是"某个请求的响应"。
// 判据：有 id 字段、且没有 method 字段 ——
//   · 响应 = {"jsonrpc":"2.0","id":1,"result":{...}}
//   · 服务端主动推的事件 = {"event":true,"seq":12,...}（无 id、有 event）
//   · 服务端主动推的通知 = {"jsonrpc":"2.0","method":"notifications/xxx"}
bool IsResponse(const json::Value& v) {
    if (!v.IsObject()) return false;
    return v.Find(L"id") != nullptr && v.Find(L"method") == nullptr;
}

// 注入令牌，同时回答"这条请求有没有 id"。
//
// 令牌放在 params.__token —— 与 ai_server.cpp 的 RpcToken 口径一致。
// 顶层 token 也认，但放 params 里更干净：不会与客户端自定义的顶层字段打架。
//
// 【为什么不能就地改】json::Value 的 Find 返回 const 指针，改不了嵌套对象；
// 所以这里**重建**一层：顶层字段原样搬过来（params 除外），params 里补 __token。
std::wstring InjectToken(const std::string& line, const std::wstring& token, bool* hasId) {
    json::Value v;
    std::wstring err;
    if (hasId) *hasId = false;
    if (!json::Parse(str::FromUtf8(line), v, &err) || !v.IsObject())
        return str::FromUtf8(line);   // 解析不了就原样送过去，让服务端报"请求不合法"

    if (hasId) *hasId = v.Find(L"id") != nullptr;

    // params：原来是对象就沿用其字段，否则建一个新的空对象。
    json::Value params = json::Value::MakeObject();
    if (const json::Value* p = v.Find(L"params"); p && p->IsObject())
        for (const auto& kv : p->obj) params.Set(kv.first, kv.second);
    params.Set(L"__token", json::Value::MakeStr(token));

    json::Value out = json::Value::MakeObject();
    for (const auto& kv : v.obj) {
        if (kv.first == L"params") continue;
        if (kv.first == L"token") continue;   // 客户端自己塞的 token 不认，认我们注入的
        out.Set(kv.first, kv.second);
    }
    out.Set(L"params", params);
    return json::Stringify(out, 0);
}

// 连管道。管道实例可能还没建好，所以重试一小会儿。
//
// 【注意】ai::CurrentPipeName() 返回的已经是**完整路径**
// （identity.h 的 kPipePrefix = "\\\\.\\pipe\\ExBarBrowser_ai_"），
// 这里不要再自己拼前缀 —— 拼了就成了
// "\\.\pipe\\.\pipe\ExBarBrowser_ai_…"，必然 ERROR_FILE_NOT_FOUND。
HANDLE ConnectPipe(const std::wstring& pipeName) {
    for (int waited = 0; waited <= kConnectRetryMs; waited += kConnectRetryStepMs) {
        HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        const DWORD err = GetLastError();
        // ERROR_FILE_NOT_FOUND = 名字还没登记；ERROR_PIPE_BUSY = 实例被别人占了。
        // 两种都值得再等一下；别的错误（如权限）重试也没用，直接放弃。
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
        Sleep(kConnectRetryStepMs);
    }
    return INVALID_HANDLE_VALUE;
}

// 打断桥接线程的阻塞 I/O。
//
// 【为什么需要】线程阻塞在 ReadFile(stdin) 上时，光设标志没用 ——
// 线程得先醒过来才看得到标志。而 CancelIoEx 对**同步**读无效
// （它只管 overlapped 操作），所以要用 CancelSynchronousIo：
// 它能取消**另一个线程**挂在同步 I/O 上的阻塞操作。
// 权限要求：目标线程句柄要带 THREAD_TERMINATE 访问权。
//
// 【为什么不能直接 CloseHandle(stdin)】句柄还在别的线程阻塞读时关掉它，
// 句柄值可能被系统回收给别的对象，后果不可预期。
void CancelBridgeIo() {
    if (!g_thread_id) return;
    HANDLE th = OpenThread(THREAD_TERMINATE, FALSE, g_thread_id);
    if (!th) return;
    CancelSynchronousIo(th);
    CloseHandle(th);
}

// 桥接线程主体：stdin ⇄ 管道。
//
// 【为什么拆成两层】外层负责"线程生命周期信号"（g_ready / g_finished），
// 内层只管搬数据。这样不管内层从哪个分支 return，收尾信号都不会漏。
void BridgeLoop();
void BridgeBody();

// 桥接线程主体：stdin ⇄ 管道。
//
// 【为什么拆成两层】外层负责"线程生命周期信号"（g_ready / g_finished），
// 内层只管搬数据。这样不管内层从哪个分支 return，收尾信号都不会漏。
void BridgeLoop() {
    // 先记线程 ID 并放行 g_ready：Stop() 要靠它打断阻塞读，
    // 必须在 Start() 返回之前就绪，否则 Stop 可能打空。
    g_thread_id = GetCurrentThreadId();
    if (g_ready) SetEvent(g_ready);

    BridgeBody();

    g_running = false;
    if (g_finished) SetEvent(g_finished);
}

// 真正的搬运循环。返回即表示桥接结束。
void BridgeBody() {
    const std::wstring token = ai::CurrentToken();
    const std::wstring pipeName = ai::CurrentPipeName();
    if (token.empty() || pipeName.empty()) {
        LogDbg(L"[mcp] stdio 桥接未启动：AI 通道没起来（令牌或管道名为空）");
        return;
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hIn == nullptr || hIn == INVALID_HANDLE_VALUE ||
        hOut == nullptr || hOut == INVALID_HANDLE_VALUE) {
        LogDbg(L"[mcp] stdio 桥接未启动：标准输入/输出不可用"
               L"（--mcp-stdio 必须由 MCP 客户端启动，它会接管 stdin/stdout）");
        return;
    }

    HANDLE hPipe = ConnectPipe(pipeName);
    if (hPipe == INVALID_HANDLE_VALUE) {
        LogDbg(L"[mcp] stdio 桥接未启动：连不上管道 %ls（err=%lu）",
               pipeName.c_str(), GetLastError());
        return;
    }

    LogDbg(L"[mcp] stdio 桥接已就绪（管道 %ls）", pipeName.c_str());

    LineReader in, pipeIn;
    in.h = hIn;
    pipeIn.h = hPipe;

    std::string line;
    while (g_running && in.readLine(&line)) {
        if (line.empty()) continue;

        bool hasId = false;
        const std::wstring req = InjectToken(line, token, &hasId);
        if (!WriteLine(hPipe, str::ToUtf8(req))) break;

        // 等自己的响应。中间可能夹着服务端主动推的事件/通知 ——
        // 那些要**转给客户端**（MCP 允许服务端推通知），不能吞掉。
        bool got = false;
        std::string resp;
        while (pipeIn.readLine(&resp)) {
            json::Value v;
            std::wstring err;
            if (json::Parse(str::FromUtf8(resp), v, &err) && IsResponse(v)) {
                got = true;
                break;
            }
            // 不是响应 → 服务端推送，转发后继续等。
            if (!WriteLine(hOut, resp)) { g_running = false; break; }
        }
        if (!got) break;

        // 通知（无 id）按 JSON-RPC 规范**不回复**；响应才写回 stdout。
        if (hasId && !WriteLine(hOut, resp)) break;
    }

    CloseHandle(hPipe);
    LogDbg(L"[mcp] stdio 桥接结束");

    // stdin EOF = 客户端断开。按 MCP 客户端的预期，此时进程应当退出。
    // 用 WM_CLOSE 而不是 ExitProcess：走正常的窗口关闭流程（存会话、停管道服务、
    // 释放 Shell 视图），不留半个进程在后台。
    // IsWindow 兜底：窗口可能已经被用户关掉了，这时候 HWND 已被回收，
    // 往里 PostMessage 可能打到别的窗口上。
    if (g_main && IsWindow(g_main)) PostMessageW(g_main, WM_CLOSE, 0, 0);
}

}  // namespace

bool Requested() {
    // 缓存结果：命令行一辈子不变，而且 ConfirmExit 之类的地方会反复问。
    // 函数内 static 的初始化是线程安全的（C++11 起）。
    static const bool kRequested = []() {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return false;
        bool found = false;
        for (int i = 1; i < argc; ++i) {
            if (str::IEquals(argv[i], L"--mcp-stdio")) {
                found = true;
                break;
            }
        }
        LocalFree(argv);
        return found;
    }();
    return kRequested;
}

bool Start(HWND mainWnd) {
    if (!Requested()) return false;
    if (g_running.load()) return true;
    g_main = mainWnd;
    g_running = true;

    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_ready || !g_finished) {
        LogDbg(L"[mcp] stdio 桥接未启动：创建同步事件失败");
        if (g_ready) { CloseHandle(g_ready); g_ready = nullptr; }
        if (g_finished) { CloseHandle(g_finished); g_finished = nullptr; }
        g_running = false;
        return false;
    }

    g_thread = std::thread(BridgeLoop);
    // 等线程记下自己的线程 ID 再返回：Stop() 依赖它去打断阻塞读，
    // 不等的话可能打空（线程还没跑到那行就被 Stop 了）。
    WaitForSingleObject(g_ready, 2000);
    return true;
}

void Stop() {
    // 可重入：OnDestroy 与 App::Run 收尾都会调。
    if (!g_running.load() && !g_thread.joinable()) return;

    g_running = false;

    // 线程可能正阻塞在 stdin 的 ReadFile 上，光设标志唤不醒它。
    // CancelBridgeIo 取消它挂在同步 I/O 上的阻塞操作。
    //
    // 【为什么循环取消】存在一个窄窗口：线程刚判断完 g_running（还是 true）、
    // 还没把 ReadFile 挂上去，这时一次取消就打空。反复取消直到线程退出，
    // 才能保证不漏。
    for (int i = 0; i < 200; ++i) {
        if (g_finished && WaitForSingleObject(g_finished, 0) == WAIT_OBJECT_0) break;
        CancelBridgeIo();
        Sleep(10);
    }

    if (g_finished && WaitForSingleObject(g_finished, 2000) == WAIT_OBJECT_0) {
        if (g_thread.joinable()) g_thread.join();
    } else if (g_thread.joinable()) {
        // 线程死活不肯走：不能 join（会把 UI 线程卡死在这里）。
        // 只能 detach —— detach 之后 std::thread 的析构不会再 terminate。
        // 这是兜底，正常路径走不到（上面 2 秒的取消循环足以唤醒它）。
        LogDbg(L"[mcp] stdio 桥接线程未在超时内退出，改为 detach");
        g_thread.detach();
    }

    if (g_ready) { CloseHandle(g_ready); g_ready = nullptr; }
    if (g_finished) { CloseHandle(g_finished); g_finished = nullptr; }
    g_thread_id = 0;
}

}  // namespace mcpstdio
