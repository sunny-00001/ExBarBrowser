// 端到端通道自测：真的建管道、真的连、真的走一轮 MCP 请求。
//
// 为什么需要这一层（单测不够）：
//   · HandleRequestLine 是纯函数，测不到"管道读写 / 线程切换 / 超时"这些真实路径；
//   · 客户端写法（读哪两个发现文件、怎么带令牌）只有跑一遍才知道对不对。
//
// 做法：本进程内 StartServer()，然后**自己当客户端**连上去发请求。
// 因为命令 handler 需要窗口，这里只验证"协议层通、形状对"，
// 不验证"命令真的改了 UI"（那要靠人工冒烟）。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/pipe_e2e_test.cpp src/ai_server.cpp src/ai_api.cpp src/commands.cpp \
//     src/cmd_naming.cpp src/json.cpp src/text_file.cpp /tmp/config_stub.cpp \
//     -o /tmp/pipe_e2e.exe -lole32 -luuid -lshell32 -ladvapi32 -luser32
#include "common.h"

#include <cstdio>
#include <string>
#include <thread>

#include "ai_server.h"
#include "config.h"
#include "json.h"
#include "text_file.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

std::wstring ReadLine(HANDLE h) {
    std::string buf;
    char c = 0;
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(h, &c, 1, &got, nullptr) || got == 0) return std::wstring();
        if (c == '\n') break;
        if (c != '\r') buf.push_back(c);
        if (buf.size() > 1u << 20) return std::wstring();   // 防跑飞
    }
    if (buf.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), &out[0], n);
    return out;
}

bool WriteLine(HANDLE h, const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return false;
    std::string buf(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &buf[0], n, nullptr,
                        nullptr);
    buf.push_back('\n');
    DWORD wrote = 0;
    return WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &wrote, nullptr) != 0;
}

json::Value ParseJson(const std::wstring& t) {
    json::Value v;
    std::wstring e;
    json::Parse(t, v, &e);
    return v;
}

}  // namespace

int main() {
    std::printf("启动通道\n");
    // 本测试不跑 UI 循环，所以用后台线程充当"UI 线程"：不断 PumpRequests。
    // 真实程序里这是主窗口的定时器做的，这里只是让请求有人处理。
    if (!ai::StartServer(nullptr)) {
        std::printf("  FAIL  启动失败：%s\n", tf::ToUtf8(L"（见日志）").c_str());
        return 1;
    }
    std::thread pumper([] {
        for (int i = 0; i < 400 && ai::IsRunning(); ++i) {
            ai::PumpRequests();
            Sleep(10);
        }
    });
    Check(true, "StartServer 成功");

    // ---- 客户端：按"外部程序该怎么做"来读发现文件 ----
    std::wstring token, pipe;
    Check(tf::ReadText(ai::TokenPath(), &token), "读到令牌文件");
    Check(tf::ReadText(tf::Join(Config::Get().Dir(), L"ai-pipe.txt"), &pipe), "读到管道名文件");
    Check(!token.empty(), "令牌非空");
    Check(!pipe.empty(), "管道名非空");
    // 管道名必须带本进程 pid：无窗口的 headless 实例原来共用 "..._default"，
    // 并发时测试连到别人的管道、拿自己的令牌去握手 -> 随机 bad_token。
    // 这条断言锁住契约，防止以后"简化"掉 pid 又复现那个查了很久的间歇红。
    Check(pipe.find(std::to_wstring(GetCurrentProcessId())) != std::wstring::npos,
          "管道名包含本进程 pid（并发实例不串线）");
    std::printf("        pipe=%ls token=%ls...\n", pipe.c_str(),
                token.substr(0, 8).c_str());

    // ---- 连接 ----
    std::wstring shortName = pipe;
    const std::wstring prefix = L"\\\\.\\pipe\\";
    if (shortName.rfind(prefix, 0) == 0) shortName = shortName.substr(prefix.size());

    HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           0, nullptr);
    Check(h != INVALID_HANDLE_VALUE, "连上管道");
    if (h == INVALID_HANDLE_VALUE) {
        ai::StopServer();
        pumper.join();
        return 1;
    }

    // ---- 1. initialize 握手 ----
    {
        std::wstring req = L"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                           L"\"params\":{\"__token\":\"" + token + L"\"}}";
        Check(WriteLine(h, req), "发送 initialize");
        json::Value r = ParseJson(ReadLine(h));
        Check(r.GetStr(L"jsonrpc") == L"2.0", "响应 jsonrpc=2.0");
        const json::Value* result = r.Find(L"result");
        Check(result != nullptr, "initialize 有 result");
        if (result) {
            Check(result->GetStr(L"protocolVersion").rfind(L"20", 0) == 0,
                  "protocolVersion 是日期形式");
            const json::Value* si = result->Find(L"serverInfo");
            Check(si && !si->GetStr(L"name").empty(), "serverInfo.name 非空");
            const json::Value* caps = result->Find(L"capabilities");
            Check(caps && caps->Find(L"tools"), "声明了 tools 能力");
        }
    }

    // ---- 2. tools/list ----
    {
        std::wstring req = L"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\","
                           L"\"params\":{\"__token\":\"" + token + L"\"}}";
        Check(WriteLine(h, req), "发送 tools/list");
        json::Value r = ParseJson(ReadLine(h));
        const json::Value* result = r.Find(L"result");
        Check(result != nullptr, "tools/list 有 result");
        const json::Value* tools = result ? result->Find(L"tools") : nullptr;
        Check(tools && tools->IsArray() && !tools->arr.empty(), "tools 是非空数组");

        if (tools && !tools->arr.empty()) {
            const json::Value& t0 = tools->arr[0];
            // MCP 必填三项
            Check(!t0.GetStr(L"name").empty(), "工具有 name");
            Check(!t0.GetStr(L"description").empty(), "工具有 description");
            const json::Value* is = t0.Find(L"inputSchema");
            Check(is && is->IsObject(), "inputSchema 是对象（不是字符串）");

            // 名字应为点分规范名
            Check(t0.GetStr(L"name").find(L'.') != std::wstring::npos,
                  "name 是点分规范名");
            Check(!t0.GetStr(L"legacyName").empty(), "同时给了 legacyName");

            // annotations 四要素
            const json::Value* ann = t0.Find(L"annotations");
            Check(ann && ann->Find(L"readOnlyHint") && ann->Find(L"destructiveHint") &&
                      ann->Find(L"idempotentHint") && ann->Find(L"openWorldHint"),
                  "annotations 含四个标准 hint");
        }
        std::printf("        工具数：%zu\n", tools ? tools->arr.size() : 0u);
    }

    // ---- 3. tools/call：调一个只读命令 ----
    {
        std::wstring req = L"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                           L"\"params\":{\"name\":\"view.mode\",\"arguments\":{\"preset\":3},"
                           L"\"__token\":\"" + token + L"\"}}";
        Check(WriteLine(h, req), "发送 tools/call(view.mode)");
        json::Value r = ParseJson(ReadLine(h));
        const json::Value* result = r.Find(L"result");
        Check(result != nullptr, "tools/call 有 result（不是 JSON-RPC error）");
        if (result) {
            const json::Value* content = result->Find(L"content");
            Check(content && content->IsArray() && !content->arr.empty(),
                  "result.content 是非空数组");
            Check(result->Find(L"isError") != nullptr, "result 有 isError 字段");
        }
    }

    // ---- 4. 错误参数：应为 isError:true 而非 JSON-RPC error ----
    {
        // 缺必填参数 preset
        std::wstring req = L"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
                           L"\"params\":{\"name\":\"view.mode\",\"arguments\":{},"
                           L"\"__token\":\"" + token + L"\"}}";
        Check(WriteLine(h, req), "发送缺参数的 tools/call");
        json::Value r = ParseJson(ReadLine(h));
        Check(r.Find(L"error") == nullptr, "工具执行失败不走 JSON-RPC error");
        const json::Value* result = r.Find(L"result");
        Check(result && result->GetBool(L"isError", false), "result.isError = true");
    }

    // ---- 5. 未知工具 ----
    {
        std::wstring req = L"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
                           L"\"params\":{\"name\":\"no.such.tool\",\"arguments\":{},"
                           L"\"__token\":\"" + token + L"\"}}";
        Check(WriteLine(h, req), "发送未知工具");
        json::Value r = ParseJson(ReadLine(h));
        const json::Value* result = r.Find(L"result");
        Check(result && result->GetBool(L"isError", false), "未知工具也是 isError:true");
    }

    // ---- 6. 错误令牌 ----
    {
        std::wstring req = L"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/list\","
                           L"\"params\":{\"__token\":\"wrong\"}}";
        Check(WriteLine(h, req), "发送错误令牌");
        json::Value r = ParseJson(ReadLine(h));
        Check(r.Find(L"error") != nullptr, "错误令牌 -> JSON-RPC error");
        Check(r.Find(L"result") == nullptr, "且没有 result（不泄露工具清单）");
    }

    CloseHandle(h);
    ai::StopServer();
    pumper.join();

    std::printf("\n停止后清理发现文件\n");
    Check(GetFileAttributesW(ai::TokenPath().c_str()) == INVALID_FILE_ATTRIBUTES,
          "令牌文件已删除");
    Check(GetFileAttributesW(tf::Join(Config::Get().Dir(), L"ai-pipe.txt").c_str()) ==
              INVALID_FILE_ATTRIBUTES,
          "管道名文件已删除");

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
