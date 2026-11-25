// AI 对外通道：传输层与生命周期（管道、发现文件、监听线程、UI 队列、启停）。
// 协议分派（简格式 + JSON-RPC 2.0 / MCP）在 ai_server_rpc.cpp；
// 令牌只有本文件里的 g_token 一份，RPC 侧经 ai::CurrentToken() 读取。
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
#include "ai_queue.h"
#include "common.h"
#include "config.h"
#include "json.h"
#include "text_file.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>   // rand_s
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
//
// 队列本体在 ai_queue.{h,cpp}：那里的不变式（请求对象共享所有权、
// 超时后 UI 线程仍可安全处理完）是这块最容易写错的地方，值得单独单测。
aiqueue::Queue g_queue;

// UI 线程处理一个请求的超时（毫秒）。超时后客户端收到 error=timeout，但 UI 线程
// 仍会把那个请求处理完 —— 命令已经在跑了。取值来自配置项 aiTimeoutMs（可热改）。
int UiTimeoutMs() {
    const int v = Config::Get().s().aiTimeoutMs;
    return v > 0 ? v : Settings{}.aiTimeoutMs;
}

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
    if (GetClassNameW(h, cls, _countof(cls)) && wcscmp(cls, id::kMainClass) == 0) {
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
        swprintf(buf, 80, L"%llx_%lu", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)), pid);
    else
        swprintf(buf, 80, L"default_%lu", pid);
    return std::wstring(kPipePrefix) + buf;
}

std::wstring PipePath() { return tf::Join(Config::Get().RuntimeDir(), L"ai-pipe.txt"); }

// 每实例一份发现文件（ai-token.<pid>.txt / ai-pipe.<pid>.txt）。
// 多实例共享同一个 profile 时，规范文件 ai-token.txt 只能指向"最后启动的实例"；
// 每实例文件让客户端可以枚举出全部正在监听的窗口，而不是只能看到最后一个。
std::wstring InstanceDiscoveryPath(const wchar_t* stem) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%ls.%lu.txt", stem, static_cast<unsigned long>(GetCurrentProcessId()));
    return tf::Join(Config::Get().RuntimeDir(), buf);
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
    tf::EnsureDir(Config::Get().RuntimeDir());
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
//
// 【为什么超时不是"取消"】超时之后 UI 线程**仍会**把那个请求处理完
// （命令已经在跑了，中途掐掉会留下半截状态）。所以请求对象必须由
// 提交方与 UI 线程**共享所有权** —— 提交方放手不等于对象没了。
// 详见 ai_queue.h。
std::wstring RunOnUiThread(const std::wstring& request) {
    std::wstring out;
    if (!g_queue.Submit(request, UiTimeoutMs(), &out)) return std::wstring();
    return out;
}

// 订阅请求识别与游标提取。放在管道线程用，只解析不执行任何命令。
bool IsSubscribeRequest(const std::string& line) {
    json::Value req; std::wstring err;
    return json::Parse(str::FromUtf8(line), req, &err) && req.GetStr(L"method") == L"events/subscribe";
}

uint64_t AfterOfRequest(const std::string& line) {
    json::Value req; std::wstring err;
    if (!json::Parse(str::FromUtf8(line), req, &err)) return 0;
    uint64_t after = static_cast<uint64_t>(req.GetInt(L"after", 0));
    if (const json::Value* p = req.Find(L"params"))
        after = static_cast<uint64_t>(p->GetInt(L"after", static_cast<int>(after)));
    return after;
}

// 超时响应：**按请求的格式**给。
//
// 【为什么不能一律回旧格式】{"ok":false,"error":"timeout"} 对 JSON-RPC 客户端
// 是个**非法响应**：没有 id，客户端无法把它认成"我那条请求的回复" ——
// 要么当成乱码丢掉，要么当成服务端推送，然后永远等一个不会来的结果（挂死）。
// MCP stdio 桥接更是直接栽在这：它靠"有 id、无 method"判响应，
// 旧格式的超时响应两条都不满足 → 被当成推送转发给客户端，客户端那头就一直挂着。
//
// 【JSON-RPC 的 code 用 -32000】那是 JSON-RPC 留给实现自定义的区间；
// 语义是"服务端没能在时限内处理完"，不是"请求不合法"（那是 -32600）。
// errorCode 一并带上：项目口径是 errorCode 才是给机器分支的稳定标识。
std::string TimeoutResponse(const std::string& line) {
    json::Value req;
    std::wstring err;
    if (json::Parse(str::FromUtf8(line), req, &err) && req.IsObject() &&
        req.Find(L"jsonrpc") != nullptr) {
        const json::Value* id = req.Find(L"id");
        json::Value v = json::Value::MakeObject();
        v.Set(L"jsonrpc", json::Value::MakeStr(L"2.0"));
        // id 缺失时按 JSON-RPC 规范回 null（"请求的 id 不知道"）。
        v.Set(L"id", id ? *id : json::Value());
        json::Value e = json::Value::MakeObject();
        e.Set(L"code", json::Value::MakeInt(-32000));
        e.Set(L"message", json::Value::MakeStr(L"请求超时：UI 线程未在时限内处理完"));
        e.Set(L"errorCode", json::Value::MakeStr(L"timeout"));
        v.Set(L"error", e);
        return WideToLine(json::Stringify(v, 0)) + "\n";
    }
    return "{\"ok\":false,\"error\":\"timeout\"}\n";
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
                resp = out.empty() ? TimeoutResponse(line) : WideToLine(out) + "\n";
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

std::wstring TokenPath() { return tf::Join(Config::Get().RuntimeDir(), L"ai-token.txt"); }

std::wstring CurrentPipeName() { return g_pipe_name; }

std::wstring CurrentToken() { return g_token; }

bool StartServer(void (*postToUi)(const std::wstring& requestLine)) {
    (void)postToUi;   // 队列 + 条件变量足够，不需要 PostMessage
    if (g_running) return true;
    g_token = MakeToken();
    g_pipe_name = PipeName();
    // 恢复上一进程留下的最近事件与序号（B8）：磁盘历史不能被新进程当成
    // "刚刚发生的"，但也不能因为重启就消失 —— 自迭代要在它上面做反思。
    events::LoadFromDisk();
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
        g_thread.join();
    }
    g_pipe = INVALID_HANDLE_VALUE;
    g_subscribers = 0;
    // 排队里还没被 UI 线程取走的请求：没人会再处理它们了，直接清掉；
    // 正在等结果的监听线程也一并唤醒（不必干等到超时）。
    g_queue.Interrupt();

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
    int total = 0;
    for (;;) {
        // 队列在 ai_queue：请求对象由提交方与 UI 线程共享所有权，
        // 所以提交方超时走人后，这里处理它依然安全。
        const int n = g_queue.Pump([](const std::wstring& req) {
            return HandleRequestLine(req);
        });
        if (n == 0) break;
        total += n;
    }
    // 客户端在时限内没等到结果的请求有几个 —— 这就是"界面卡了多久"的账。
    // 只在**账变了**时报：定时器每 100ms 跑一次，每次都报会刷爆日志。
    static size_t last_abandoned = 0;
    const size_t abandoned = g_queue.Abandoned();
    if (abandoned > last_abandoned) {
        LogDbg(L"[ai] 有 %zu 个请求在客户端超时之后才处理完（命令跑得比等待时限长）",
               abandoned);
        last_abandoned = abandoned;
    }
    return total;
}

}  // namespace ai
