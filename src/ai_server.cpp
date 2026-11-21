// AI 对外通道实现（协议说明见 ai_server.h）。
//
// 线程模型（关键，别改坏）：
//   监听线程  —— 只做"读一行 → 排队 → 等结果 → 写回"。
//   UI 线程   —— 从队列取请求 → HandleRequestLine() → 把结果放回。
// 为什么必须这样分：CommandBus 的 handler 全是 UI/Shell 操作（导航、SetViewMode、
// 弹对话框），在监听线程直接调会崩或死锁；反过来在 UI 线程读管道会卡住消息循环。
// 架构规则 §4 原则 8「通道分层，UI 不阻塞」讲的就是这条。
#include "ai_server.h"

#include "str.h"

#include "ai_api.h"
#include "common.h"
#include "config.h"
#include "json.h"
#include "text_file.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>   // rand_s
#include <deque>
#include <mutex>
#include <thread>

namespace ai {
namespace {

constexpr const wchar_t* kPipePrefix = id::kPipePrefix;
constexpr size_t kMaxLineBytes = 64 * 1024;   // 一行上限，防被灌爆

std::atomic<bool> g_running{false};
std::thread g_thread;
HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::wstring g_token;
std::wstring g_pipe_name;

// ---- UI 线程与监听线程之间的请求队列 ----
struct Job {
    std::wstring request;
    std::wstring response;
    bool done = false;
};
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Job*> g_queue;      // 待处理（UI 线程消费）
Job* g_inflight = nullptr;     // 正在处理的那一个（UI 线程处理完置 done）

// UTF-8 转换唯一实现在 str.cpp
std::wstring LineToWide(const std::string& s) { return str::FromUtf8(s); }
std::string WideToLine(const std::wstring& s) { return str::ToUtf8(s); }

// 生成随机令牌（每进程一次）。
// 用 RtlGenRandom（advapi32 的 SystemFunction036）：rand_s 需要 _CRT_RAND_S 且在
// 不同 CRT 下可用性不一致；rand() 种子可预测，不适合做令牌。
std::wstring MakeToken() {
    unsigned char raw[16] = {};
    // advapi32 的 RtlGenRandom 在 <ntsecapi.h> 里声明为 SystemFunction036。
    // 用 LoadLibrary 动态取，避免为一个函数加链接依赖。
    using Fn = BOOLEAN(WINAPI*)(PVOID, ULONG);
    static Fn gen = [] () -> Fn {
        if (HMODULE m = LoadLibraryW(L"advapi32.dll"))
            return reinterpret_cast<Fn>(GetProcAddress(m, "SystemFunction036"));
        return nullptr;
    }();
    if (!gen || !gen(raw, sizeof(raw))) {
        // 极端兜底：用时间 + 计数器凑一个（仍然比纯时间好）
        static ULONGLONG c = 0;
        ULONGLONG t = GetTickCount64() + (++c);
        memcpy(raw, &t, sizeof(t));
    }
    wchar_t buf[40];
    swprintf(buf, 40, L"%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
             raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
    return buf;
}

// 管道名后缀用主窗口句柄：客户端不需要知道 pid，只需要连到"正在跑的那个窗口"。
// 进程退出 → 句柄失效 → 旧名自然连不上，不会连到僵尸管道。
std::wstring PipeName() {
    wchar_t buf[32] = L"default";
    if (HWND h = FindWindowW(id::kMainClass, nullptr))
        swprintf(buf, 32, L"%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
    return std::wstring(kPipePrefix) + buf;
}

void WriteDiscoveryFiles(const std::wstring& token, const std::wstring& pipe) {
    // 客户端从这两个文件拿到"连哪里、用什么令牌"。
    // 权限：目录在 %APPDATA%\ExBar 下，按用户隔离；这不是加密，只是护栏。
    tf::EnsureDir(Config::Get().Dir());
    if (!tf::WriteText(TokenPath(), token))
        LogDbg(L"[ai] 令牌写入失败：%s", TokenPath().c_str());
    if (!tf::WriteText(tf::Join(Config::Get().Dir(), L"ai-pipe.txt"), pipe))
        LogDbg(L"[ai] 管道名写入失败");
}

// 把请求交给 UI 线程并同步等结果。超时返回空（调用方回错误）。
std::wstring RunOnUiThread(const std::wstring& request) {
    Job job;
    job.request = request;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.push_back(&job);
    }
    g_cv.notify_one();

    std::unique_lock<std::mutex> lk(g_mu);
    // 等 UI 线程处理完。带超时：UI 线程若在模态对话框里，不能让客户端无限挂。
    if (!g_cv.wait_for(lk, std::chrono::seconds(10), [&] { return job.done; })) {
        // 超时：把它从各自的容器里摘掉，避免 UI 线程之后写悬空指针
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
            if (*it == &job) { g_queue.erase(it); break; }
        }
        if (g_inflight == &job) g_inflight = nullptr;
        return std::wstring();
    }
    return job.response;
}

void ServeLoop() {
    while (g_running) {
        g_pipe = CreateNamedPipeW(g_pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                  1, kMaxLineBytes, kMaxLineBytes, 0, nullptr);
        if (g_pipe == INVALID_HANDLE_VALUE) {
            LogDbg(L"[ai] 管道创建失败 err=%lu", GetLastError());
            return;
        }
        BOOL ok = ConnectNamedPipe(g_pipe, nullptr) ? TRUE
                                                    : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!ok || !g_running) {
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
            continue;
        }

        std::string acc;
        char buf[4096];
        DWORD read = 0;
        while (g_running && ReadFile(g_pipe, buf, sizeof(buf), &read, nullptr) && read > 0) {
            // 【必须在这里限长，不能只在见到 '\n' 之后判】
            // 原实现是 acc.append 无条件累积，上限检查放在 `line.size() > kMaxLineBytes`
            // 那一支 —— 而那一支只有在读到一个换行之后才会执行。客户端只要一直不发 '\n'
            // （或发一个 1GB 的巨型行），acc 就无界增长，本机任意进程无需令牌即可
            // 把浏览器内存耗光。这里逐次判断，超限即断连。
            if (acc.size() + read > kMaxLineBytes) {
                LogDbg(L"[ai] 单行超过 %zu 字节（已累积 %zu），断开该客户端",
                       kMaxLineBytes, acc.size() + read);
                break;
            }
            acc.append(buf, read);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                std::string resp;
                if (line.size() > kMaxLineBytes) {
                    resp = "{\"ok\":false,\"error\":\"line_too_large\"}\n";
                } else {
                    std::wstring out = RunOnUiThread(LineToWide(line));
                    resp = out.empty() ? std::string("{\"ok\":false,\"error\":\"timeout\"}\n")
                                       : WideToLine(out) + "\n";
                }
                DWORD w = 0;
                WriteFile(g_pipe, resp.data(), static_cast<DWORD>(resp.size()), &w, nullptr);
            }
        }
        FlushFileBuffers(g_pipe);
        DisconnectNamedPipe(g_pipe);
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

}  // namespace

std::wstring TokenPath() { return tf::Join(Config::Get().Dir(), L"ai-token.txt"); }

bool StartServer(void (*postToUi)(const std::wstring& requestLine)) {
    (void)postToUi;   // 队列 + 条件变量足够，不需要 PostMessage
    if (g_running) return true;
    g_token = MakeToken();
    g_pipe_name = PipeName();
    WriteDiscoveryFiles(g_token, g_pipe_name);
    g_running = true;
    g_thread = std::thread(ServeLoop);
    LogDbg(L"[ai] 通道已启动：%s（令牌见 %s）", g_pipe_name.c_str(), TokenPath().c_str());
    return true;
}

bool IsRunning() { return g_running.load(); }

void StopServer() {
    if (!g_running) return;
    g_running = false;
    // 打断阻塞中的 ConnectNamedPipe / ReadFile
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(g_pipe, nullptr);
        DisconnectNamedPipe(g_pipe);
    }
    if (g_thread.joinable()) {
        // ConnectNamedPipe 无客户端时会一直阻塞：自连一次唤醒它
        HANDLE h = CreateFileW(g_pipe_name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            // 连上后立刻断开，让 ConnectNamedPipe 返回 TRUE，循环醒来发现 !g_running 退出
            DisconnectNamedPipe(h);
            CloseHandle(h);
        }
        g_cv.notify_all();
        g_thread.join();
    }
    g_pipe = INVALID_HANDLE_VALUE;

    // 令牌只对"本次运行的管道"有效，停服后必须删掉发现文件：
    // 否则 aiEnabled 关掉后旧令牌仍躺在磁盘上，看起来像通道还开着。
    DeleteFileW(tf::ToExtended(TokenPath()).c_str());
    DeleteFileW(tf::ToExtended(tf::Join(Config::Get().Dir(), L"ai-pipe.txt")).c_str());
}

// UI 线程调用：处理挂起的请求。返回处理了几个（0 = 没活干）。
int PumpRequests() {
    int n = 0;
    for (;;) {
        Job* job = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_queue.empty()) break;
            job = g_queue.front();
            g_queue.pop_front();
            g_inflight = job;
        }
        std::wstring resp = HandleRequestLine(job->request);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            job->response = std::move(resp);
            job->done = true;
            if (g_inflight == job) g_inflight = nullptr;
        }
        g_cv.notify_all();
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------- 请求处理

namespace {

bool CheckToken(const json::Value& req) {
    if (g_token.empty()) return false;
    std::wstring got = req.GetStr(L"token");
    if (got.size() != g_token.size()) return false;
    // 定长逐字符比较，不提前返回（本地时序侧信道意义有限，成本几乎为零）
    unsigned diff = 0;
    for (size_t i = 0; i < got.size(); ++i) diff |= static_cast<unsigned>(got[i] ^ g_token[i]);
    return diff == 0;
}

json::Value Err(const wchar_t* msg) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"ok", json::Value::MakeBool(false));
    v.Set(L"error", json::Value::MakeStr(msg));
    return v;
}

// ---------------------------------------------------------------- JSON-RPC 2.0
//
// 为什么加这一层：MCP 的传输层就是 JSON-RPC 2.0。客户端（Claude Desktop、
// Cursor 等）说的是 {"jsonrpc":"2.0","id":1,"method":"tools/list"}，
// 我们原先的 {"token":"..","method":"tools/list"} 它们看不懂 —— 于是每个客户端
// 都得自己写一层转换，那就失掉了"直接可用"的意义。
//
// 保留旧格式的理由：已经写好的本地脚本按旧格式发的；两者靠**有无 jsonrpc 字段**
// 区分，互不干扰。
//
// 令牌怎么带：MCP 没有这个概念，所以约定放在 params 里 __token 字段
// （也接受顶层 token 以兼容旧格式）。不是加密，只是"别的进程不能随手乱调"。

// JSON-RPC 2.0 标准错误码全集。kParseError / kInternalError 目前没有调用点，
// 但**保留**而不是删掉：这张表是对外协议的一部分（客户端可能按 code 分派），
// 缺两个会让下一个人以为"本实现不支持这两个码"而另发明一个。
// 用 [[maybe_unused]] 明确"有意保留"，而不是靠注释躲过警告。
[[maybe_unused]] constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
[[maybe_unused]] constexpr int kInternalError = -32603;

json::Value RpcError(const json::Value& id, int code, const wchar_t* message) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"jsonrpc", json::Value::MakeStr(L"2.0"));
    v.Set(L"id", id);
    json::Value err = json::Value::MakeObject();
    err.Set(L"code", json::Value::MakeInt(code));
    err.Set(L"message", json::Value::MakeStr(message));
    v.Set(L"error", err);
    return v;
}

json::Value RpcResult(const json::Value& id, const json::Value& result) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"jsonrpc", json::Value::MakeStr(L"2.0"));
    v.Set(L"id", id);
    v.Set(L"result", result);
    return v;
}

}  // namespace

std::wstring HandleRequestLine(const std::wstring& line) {
    json::Value req;
    std::wstring perr;
    if (!json::Parse(line, req, &perr) || !req.IsObject())
        return json::Stringify(Err(L"invalid_json"), 0);

    // ---- 协议分派：有 jsonrpc 字段走 JSON-RPC 2.0（MCP 兼容），否则走旧格式 ----
    if (req.Find(L"jsonrpc")) return HandleRpcLine(req);

    if (!CheckToken(req)) return json::Stringify(Err(L"bad_token"), 0);

    const std::wstring method = req.GetStr(L"method");

    if (method == L"tools/list") {
        json::Value v = json::Value::MakeObject();
        v.Set(L"ok", json::Value::MakeBool(true));
        json::Value tools;
        if (json::Parse(ToolsToJson(), tools, &perr)) {
            const json::Value* arr = tools.Find(L"tools");
            v.Set(L"tools", arr ? *arr : json::Value::MakeArray());
        } else {
            v.Set(L"tools", json::Value::MakeArray());
        }
        return json::Stringify(v, 0);
    }

    if (method == L"state") {
        json::Value v = json::Value::MakeObject();
        v.Set(L"ok", json::Value::MakeBool(true));
        json::Value st;
        if (json::Parse(StateToJson(GetState()), st, &perr)) {
            for (const auto& kv : st.obj) v.Set(kv.first, kv.second);
        }
        return json::Stringify(v, 0);
    }

    if (method == L"tools/call") {
        const std::wstring name = req.GetStr(L"name");
        if (name.empty()) return json::Stringify(Err(L"missing_name"), 0);
        const json::Value* args = req.Find(L"args");
        const json::Value empty = json::Value::MakeObject();
        const bool approve = req.GetBool(L"approve", false);
        CallResult r = Call(name, args ? *args : empty, approve);
        return ResultToJson(r);
    }

    return json::Stringify(Err(L"unknown_method"), 0);
}

// ---------------------------------------------------------------- JSON-RPC 2.0

namespace {

// 从 JSON-RPC 请求里取令牌：MCP 没有令牌概念，所以约定放 params.__token；
// 也接受顶层 token（有些客户端会把自定义字段放外面）。
std::wstring RpcToken(const json::Value& req) {
    if (const json::Value* params = req.Find(L"params")) {
        if (params->IsObject()) {
            std::wstring t = params->GetStr(L"__token");
            if (!t.empty()) return t;
        }
    }
    return req.GetStr(L"token");
}

bool RpcHasToken(const json::Value& req) {
    const std::wstring got = RpcToken(req);
    if (got.empty() || g_token.empty()) return false;
    if (got.size() != g_token.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < got.size(); ++i) diff |= static_cast<unsigned>(got[i] ^ g_token[i]);
    return diff == 0;
}

}  // namespace

std::wstring HandleRpcLine(const json::Value& req) {
    // id 可以是数字或字符串；缺省视为通知（notification），但仍要回一个
    // 带 null id 的响应 —— 我们不走长连接的状态机，一问一答更好调试。
    json::Value id;   // 默认 kNull
    if (const json::Value* v = req.Find(L"id")) id = *v;

    // 1. 版本
    if (req.GetStr(L"jsonrpc") != L"2.0")
        return json::Stringify(RpcError(id, kInvalidRequest, L"需要 jsonrpc: \"2.0\""), 0);

    // 2. 令牌。放在方法分派之前 —— 未授权时不泄露"有哪些方法/工具"。
    if (!RpcHasToken(req)) {
        json::Value e = RpcError(id, kInvalidRequest, L"bad_token");
        return json::Stringify(e, 0);
    }

    const std::wstring method = req.GetStr(L"method");
    if (method.empty()) return json::Stringify(RpcError(id, kInvalidRequest, L"缺少 method"), 0);

    const json::Value* params = req.Find(L"params");
    const json::Value emptyParams = json::Value::MakeObject();
    const json::Value& p = (params && params->IsObject()) ? *params : emptyParams;

    // ---- MCP 的 initialize：客户端握手时一定会问，不实现会被判定为不兼容 ----
    if (method == L"initialize") {
        json::Value result = json::Value::MakeObject();
        // 协议版本回一个我们支持的（MCP 用日期命名）。客户端会据此决定降级行为。
        result.Set(L"protocolVersion", json::Value::MakeStr(L"2024-11-05"));
        json::Value info = json::Value::MakeObject();
        info.Set(L"name", json::Value::MakeStr(id::kAiServiceName));
        info.Set(L"version", json::Value::MakeStr(L"1.0"));
        result.Set(L"serverInfo", info);
        // 只声明 tools 能力。声明了却做不到比不声明更糟。
        json::Value caps = json::Value::MakeObject();
        json::Value toolsCap = json::Value::MakeObject();
        // listChanged=false：命令表是编译期固定的，不会在运行时增删。
        toolsCap.Set(L"listChanged", json::Value::MakeBool(false));
        caps.Set(L"tools", toolsCap);
        result.Set(L"capabilities", caps);
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- tools/list ----
    if (method == L"tools/list") {
        json::Value tools;
        std::wstring perr;
        json::Value result = json::Value::MakeObject();
        if (json::Parse(ToolsToJson(), tools, &perr)) {
            const json::Value* arr = tools.Find(L"tools");
            result.Set(L"tools", arr ? *arr : json::Value::MakeArray());
        } else {
            result.Set(L"tools", json::Value::MakeArray());
        }
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- tools/call ----
    if (method == L"tools/call") {
        const std::wstring name = p.GetStr(L"name");
        if (name.empty())
            return json::Stringify(RpcError(id, kInvalidParams, L"缺少 params.name"), 0);

        const json::Value* arguments = p.Find(L"arguments");
        const json::Value emptyArgs = json::Value::MakeObject();
        const bool approve = p.GetBool(L"approve", false);

        CallResult r = Call(name, arguments ? *arguments : emptyArgs, approve);

        // MCP 的 Tool Result 形状：content 数组 + isError。
        // 失败也走 result（不是 JSON-RPC error）—— 那是"工具执行失败"，
        // 与"请求本身不合法"是两回事，混在一起客户端没法区分该重试还是该改请求。
        json::Value result = json::Value::MakeObject();
        json::Value content = json::Value::MakeArray();
        json::Value text = json::Value::MakeObject();
        text.Set(L"type", json::Value::MakeStr(L"text"));

        if (r.hasResult) {
            // 有结构化结果时，文本块里放序列化 JSON（兼容只认文本的客户端）。
            text.Set(L"text", json::Value::MakeStr(json::Stringify(r.result, 0)));
            result.Set(L"structuredContent", r.result);
        } else if (r.ok) {
            text.Set(L"text", json::Value::MakeStr(L"ok: " + r.command));
        } else {
            text.Set(L"text", json::Value::MakeStr(L"失败：" + r.error));
        }
        content.arr.push_back(text);
        result.Set(L"content", content);
        result.Set(L"isError", json::Value::MakeBool(!r.ok));
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- exbar/state：非 MCP 标准方法，用厂商前缀避免与将来标准冲突 ----
    if (method == L"exbar/state") {
        json::Value st;
        std::wstring perr;
        json::Value result = json::Value::MakeObject();
        if (json::Parse(StateToJson(GetState()), st, &perr)) result = st;
        return json::Stringify(RpcResult(id, result), 0);
    }

    return json::Stringify(RpcError(id, kMethodNotFound, L"未知方法"), 0);
}

}  // namespace ai
