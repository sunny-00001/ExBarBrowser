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
#include "ai_events.h"
#include "common.h"
#include "config.h"
#include "json.h"
#include "text_file.h"

#include <algorithm>
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
// 事件订阅者计数：只有 >0 时 UI 线程才值得每 100ms 生成状态快照。
std::atomic<int> g_subscribers{0};
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

// 管道名必须**进程唯一**：客户端读 ai-pipe.txt 拿全名，不猜名字。
// 后缀 = 窗口句柄（有窗口时）+ pid：
//   · 句柄标示"哪个窗口"，进程退出即失效，不会连到僵尸管道；
//   · pid 是必须的 —— 无窗口进程（单测 / headless e2e / 启动早期）原来全用
//     "default"，两个并发实例串到同一个管道，各自拿自己的令牌去连对方的管道，
//     表现为随机 bad_token（实测：先起一个占位进程，pipe_e2e 立刻 12 条失败）。
//
// 【多实例】句柄必须是**本进程自己**的主窗口：FindWindowW 会返回系统里第一个
// 同类窗口（可能是别的实例），那样两个实例的管道名只靠 pid 区分，日志里也会
// 指错窗口。这里按进程 id 枚举，找不到才退化成 default_<pid>。
BOOL CALLBACK FindOwnMainProc(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t cls[64] = L"";
    if (GetClassNameW(h, cls, _countof(cls)) &&
        wcscmp(cls, id::kMainClass) == 0) {
        *reinterpret_cast<HWND*>(lp) = h;
        return FALSE;
    }
    return TRUE;
}

HWND OwnMainWindow() {
    HWND h = nullptr;
    EnumWindows(FindOwnMainProc, reinterpret_cast<LPARAM>(&h));
    return h;
}

std::wstring PipeName() {
    const unsigned long pid = GetCurrentProcessId();
    wchar_t buf[80];
    if (HWND h = OwnMainWindow())
        swprintf(buf, 80, L"%llx_%lu",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)), pid);
    else
        swprintf(buf, 80, L"default_%lu", pid);
    return std::wstring(kPipePrefix) + buf;
}

std::wstring PipePath() { return tf::Join(Config::Get().Dir(), L"ai-pipe.txt"); }

// 每实例一份发现文件（ai-token.<pid>.txt / ai-pipe.<pid>.txt）。
// 多实例共享同一个 profile 时，规范文件 ai-token.txt 只能指向"最后启动的实例"；
// 每实例文件让客户端可以枚举出全部正在监听的窗口，而不是只能看到最后一个。
std::wstring InstanceDiscoveryPath(const wchar_t* stem) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%ls.%lu.txt", stem, static_cast<unsigned long>(GetCurrentProcessId()));
    return tf::Join(Config::Get().Dir(), buf);
}

// 删除内容仍属于自己的发现文件；内容已被别的实例覆盖就不动。
// 否则"关闭本实例"会把另一个实例的令牌删掉，客户端随即全部 bad_token。
void DeleteDiscoveryIfOwned(const std::wstring& path, const std::wstring& expected) {
    std::wstring cur;
    if (!tf::ReadText(path, &cur)) return;
    while (!cur.empty() && (cur.back() == L'\n' || cur.back() == L'\r')) cur.pop_back();
    if (cur != expected) return;
    DeleteFileW(tf::ToExtended(path).c_str());
}

void WriteDiscoveryFiles(const std::wstring& token, const std::wstring& pipe) {
    // 客户端从这些文件拿到"连哪里、用什么令牌"。
    // 权限：文件在便携数据目录下，天然按用户隔离；这不是加密，只是护栏。
    tf::EnsureDir(Config::Get().Dir());
    if (!tf::WriteText(TokenPath(), token))
        LogDbg(L"[ai] 令牌写入失败：%s", TokenPath().c_str());
    if (!tf::WriteText(PipePath(), pipe))
        LogDbg(L"[ai] 管道名写入失败");
    // 每实例副本：多实例并存时客户端可以逐个发现，不互相覆盖。
    if (!tf::WriteText(InstanceDiscoveryPath(L"ai-token"), token))
        LogDbg(L"[ai] 实例令牌写入失败");
    if (!tf::WriteText(InstanceDiscoveryPath(L"ai-pipe"), pipe))
        LogDbg(L"[ai] 实例管道名写入失败");
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

// 订阅请求识别与游标提取。放在管道线程用，只解析不执行任何命令。
bool IsSubscribeRequest(const std::string& line) {
    json::Value req;
    std::wstring err;
    return json::Parse(str::FromUtf8(line), req, &err) &&
           req.GetStr(L"method") == L"events/subscribe";
}

uint64_t AfterOfRequest(const std::string& line) {
    json::Value req;
    std::wstring err;
    if (!json::Parse(str::FromUtf8(line), req, &err)) return 0;
    uint64_t after = static_cast<uint64_t>(req.GetInt(L"after", 0));
    if (const json::Value* p = req.Find(L"params"))
        after = static_cast<uint64_t>(p->GetInt(L"after", static_cast<int>(after)));
    return after;
}

uint64_t SeqOfLine(const std::wstring& line) {
    json::Value v;
    std::wstring err;
    if (!json::Parse(line, v, &err)) return 0;
    const json::Value* s = v.Find(L"seq");
    return s && s->IsNumber() ? static_cast<uint64_t>(s->num) : 0;
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
        bool subscribed = false;
        uint64_t after = 0;

        auto send = [&](const std::string& bytes) {
            DWORD w = 0;
            return WriteFile(g_pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr) &&
                   w == bytes.size();
        };

        // 处理一行请求；返回 false = 连接断了，别再往下读。
        auto processLine = [&](const std::string& line) {
            std::string resp;
            if (line.size() > kMaxLineBytes) {
                resp = "{\"ok\":false,\"error\":\"line_too_large\"}\n";
            } else {
                std::wstring out = RunOnUiThread(LineToWide(line));
                resp = out.empty() ? std::string("{\"ok\":false,\"error\":\"timeout\"}\n")
                                   : WideToLine(out) + "\n";
            }
            if (!send(resp)) return false;
            // 订阅确认已经写回去，才切模式：确认之前产生的事件排在后面推，
            // 客户端看到的顺序永远是「订阅成功 → 事件」。
            // 计数用于 UI 线程判断"还有没有人要状态快照"。
            if (!subscribed && IsSubscribeRequest(line)) {
                subscribed = true;
                ++g_subscribers;
                after = AfterOfRequest(line);
            }
            return true;
        };

        // 把新读到的字节切成行。订阅/非订阅两条路共用，避免限长规则写两份。
        auto feed = [&](const char* buf, DWORD read) {
            if (acc.size() + read > kMaxLineBytes) {
                LogDbg(L"[ai] 单行超过 %zu 字节（已累积 %zu），断开该客户端",
                       kMaxLineBytes, acc.size());
                return false;
            }
            acc.append(buf, read);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (!processLine(line)) return false;
            }
            return true;
        };

        char buf[4096];
        DWORD read = 0;
        while (g_running) {
            if (subscribed) {
                // 推事件：阻塞等新事件（最多 20ms），有则立刻醒来 ——
                // 不再用 Sleep(20) 轮询，空闲订阅者不占 CPU。
                // 同一个 20ms 窗口里也顺便看客户端有没有新请求（订阅期间仍可发命令）。
                bool failed = false;
                std::wstring line;
                for (bool first = true; events::WaitFor(after, &line, first ? 20 : 0); first = false) {
                    const uint64_t seq = SeqOfLine(line);
                    if (!send(WideToLine(line) + "\n")) { failed = true; break; }
                    if (seq > after) after = seq;
                    line.clear();
                }
                if (failed) break;
                DWORD avail = 0;
                // 【客户端断开必须在这里被发现】PeekNamedPipe 对已断开的管道返回
                // FALSE。以前这个返回值被忽略，订阅者一断开，服务端就在订阅分支
                // 里永远空转：既不重建监听实例，也不释放唯一那个管道实例，
                // 之后所有客户端都拿到 ERROR_PIPE_BUSY（实测 err=231），
                // 直到重启程序才恢复 —— 一个订阅动作就能把通道彻底堵死。
                if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr)) break;
                if (avail > 0) {
                    char chunk[4096];
                    const DWORD n = std::min<DWORD>(avail, sizeof(chunk));
                    if (!ReadFile(g_pipe, chunk, n, &read, nullptr) || read == 0) break;
                    if (!feed(chunk, read)) break;
                }
                continue;
            }

            if (!ReadFile(g_pipe, buf, sizeof(buf), &read, nullptr) || read == 0) break;
            if (!feed(buf, read)) break;
        }
        if (subscribed) {
            subscribed = false;
            --g_subscribers;
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
    // 每次启动都是一条新的事件流：旧进程的历史不应被新客户端当成刚刚发生的。
    events::Clear();
    events::ResetStateBaseline();
    WriteDiscoveryFiles(g_token, g_pipe_name);
    g_running = true;
    g_thread = std::thread(ServeLoop);
    LogDbg(L"[ai] 通道已启动：%s（令牌见 %s）", g_pipe_name.c_str(), TokenPath().c_str());
    return true;
}

bool IsRunning() { return g_running.load(); }

bool HasSubscribers() { return g_subscribers.load() > 0; }

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
    g_subscribers = 0;

    // 发现文件清理：
    //   · 每实例副本 ai-token.<pid>.txt / ai-pipe.<pid>.txt 无条件删；
    //   · 规范文件 ai-token.txt / ai-pipe.txt 只删"内容仍是自己"的 ——
    //     多实例共存时别的实例可能已经覆盖了它们，删掉等于把对方的令牌也删了。
    DeleteFileW(tf::ToExtended(InstanceDiscoveryPath(L"ai-token")).c_str());
    DeleteFileW(tf::ToExtended(InstanceDiscoveryPath(L"ai-pipe")).c_str());
    DeleteDiscoveryIfOwned(TokenPath(), g_token);
    DeleteDiscoveryIfOwned(PipePath(), g_pipe_name);
}

// UI 线程调用：处理挂起的请求。返回处理了几个（0 = 没活干）。
int PumpRequests() {
    // 状态变化由 UI 线程定时检测、由管道推给订阅者；客户端因此不需要轮询 state。
    // 没有订阅者时跳过：生成整份状态快照（含选中路径/标签列表）没有接收方，
    // 而 AI 通道现在默认开启，这份开销不该在没人看的时候每 100ms 付一次。
    if (HasSubscribers()) events::PollStateChanges();
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

    // ---- 能力面：让客户端能区分"没这条命令 / 有但没开放 / 有但没实现" ----
    // 放在 tools/list 旁边而不是塞进 state：它是编译期事实，与运行时状态无关。
    if (method == L"capabilities" || method == L"exbar/capabilities") {
        json::Value out;
        std::wstring jerr;
        if (!json::Parse(CapabilitiesToJson(), out, &jerr)) out = json::Value::MakeObject();
        out.Set(L"ok", json::Value::MakeBool(true));
        return json::Stringify(out, 0);
    }

    // ---- 事件流：补历史 + 订阅。两者都过同一个令牌校验（见上面的 CheckToken）----
    if (method == L"events/since") {
        const uint64_t after = static_cast<uint64_t>(req.GetInt(L"after", 0));
        const size_t limit = static_cast<size_t>(req.GetInt(L"limit", 0));
        json::Value out;
        std::wstring jerr;
        if (!json::Parse(events::Since(after, limit), out, &jerr)) out = json::Value::MakeObject();
        out.Set(L"ok", json::Value::MakeBool(true));
        return json::Stringify(out, 0);
    }

    if (method == L"events/subscribe") {
        // 真正的"保持连接并推送"在 ServeLoop 里做；这里只回一个确认。
        json::Value v = json::Value::MakeObject();
        v.Set(L"ok", json::Value::MakeBool(true));
        v.Set(L"subscribed", json::Value::MakeBool(true));
        v.Set(L"lastSeq", json::Value::MakeNum(static_cast<double>(events::LastSeq())));
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

    // ---- 事件流（JSON-RPC 版）：与本地简格式共用同一份事件缓冲 ----
    if (method == L"events/since") {
        json::Value out;
        std::wstring jerr;
        const uint64_t after = static_cast<uint64_t>(p.GetInt(L"after", 0));
        const size_t limit = static_cast<size_t>(p.GetInt(L"limit", 0));
        if (!json::Parse(events::Since(after, limit), out, &jerr)) out = json::Value::MakeObject();
        return json::Stringify(RpcResult(id, out), 0);
    }
    if (method == L"events/subscribe") {
        json::Value result = json::Value::MakeObject();
        result.Set(L"subscribed", json::Value::MakeBool(true));
        result.Set(L"lastSeq", json::Value::MakeNum(static_cast<double>(events::LastSeq())));
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

        // 失败优先：以前 hasResult 为真时会把（空的）结果对象当正文输出，
        // 于是调用方只看到 "{}"，真正的原因 r.error（"目标已存在……"）被丢掉 ——
        // 声明了 outputSchema 的命令统统踩这一条。失败时原因必须在文本块里。
        if (!r.ok) {
            text.Set(L"text", json::Value::MakeStr(L"失败：" + r.error));
            if (r.hasResult) result.Set(L"structuredContent", r.result);
        } else if (r.hasResult) {
            // 有结构化结果时，文本块里放序列化 JSON（兼容只认文本的客户端）。
            text.Set(L"text", json::Value::MakeStr(json::Stringify(r.result, 0)));
            result.Set(L"structuredContent", r.result);
        } else {
            text.Set(L"text", json::Value::MakeStr(L"ok: " + r.command));
        }
        content.arr.push_back(text);
        // content 在断言块之后再 Set：json::Value::Set 是 move 语义，
        // 先 Set 会把它搬空，后面 push_back 的失败说明就丢了。

        // ---- 自断言 ----
        //
        // 断言失败时 isError 置 true。这是刻意的：
        //   命令 handler 返回 true 只说明"我这个函数返回了成功"，而断言说的是
        //   "效果对吗"。两者不一致时，从调用方的立场看这次操作**没有达成目的** ——
        //   如果仍报 isError=false，AI 会基于"已成功"继续下一步，
        //   错误被推到很远以后才暴露，而那时现场已经丢了。
        //
        // 同时在 text 里给出可读原因，并在 assertions 字段里给结构化明细：
        // 只说失败没用，调用方需要知道"期望什么、实际什么"才能决策。
        bool assertFailed = false;
        if (r.hasAssertions) {
            result.Set(L"assertions", r.assertions);
            for (const auto& a : r.assertions.arr) {
                const json::Value* okv = a.Find(L"ok");
                if (okv && !okv->b) {
                    assertFailed = true;
                    const std::wstring expr = a.GetStr(L"expr");
                    const std::wstring expect = a.GetStr(L"expect");
                    const std::wstring actual = a.GetStr(L"actual");
                    LogDbg(L"[ai] %s 断言不符：%s 期望 %s 实际 %s", r.command.c_str(),
                           expr.c_str(), expect.c_str(), actual.c_str());
                    // 失败原因写进文本块：只认 content 的客户端也能看到。
                    json::Value warn = json::Value::MakeObject();
                    warn.Set(L"type", json::Value::MakeStr(L"text"));
                    warn.Set(L"text", json::Value::MakeStr(
                        L"断言不符：" + expr + L"（期望 " + expect + L"，实际 " + actual + L"）"));
                    content.arr.push_back(warn);
                }
            }
            if (assertFailed) {
                result.Set(L"isError", json::Value::MakeBool(true));
            }
        }
        result.Set(L"content", content);
        // 机器码与 isError 并存：MCP 客户端能读 isError，但读不出"哪一类失败"。
        // 客户端要决定"改参数 / 补命令 / 要审批"，靠这个字段而不是解析中文。
        if (!r.errorCode.empty()) result.Set(L"errorCode", json::Value::MakeStr(r.errorCode));
        if (!result.Find(L"isError")) {
            result.Set(L"isError", json::Value::MakeBool(!r.ok));
        }
        return json::Stringify(RpcResult(id, result), 0);
    }

    // ---- exbar/capabilities：命令总数/已实现/已暴露 + 两类缺口清单 ----
    // 与 exbar/state 同一命名约定（厂商前缀，不占 MCP 标准方法名）。
    if (method == L"exbar/capabilities") {
        json::Value result;
        std::wstring perr;
        if (!json::Parse(CapabilitiesToJson(), result, &perr)) result = json::Value::MakeObject();
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
