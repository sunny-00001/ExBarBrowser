// 返回值自测（第 5 步判据：读操作能返回结构化结果）。
//
// 核心断言：**带 outputSchema 的命令必须真的产出结果；不带的不能凭空造结果。**
// 前者错了调用方按 schema 取字段会全落空，后者错了会让人以为"有数据"。
//
// handler 需要窗口，所以这里不测真实文件系统；测的是 CommandBus 的
// 派发通道本身 —— 注册、结果填充、HasResult、无结果时给空对象。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/result_test.cpp src/commands.cpp src/json.cpp src/text_file.cpp \
//     /tmp/config_stub.cpp -o /tmp/result_test.exe -lole32 -luuid -lshell32 -ladvapi32
#include <cstdio>
#include <string>

#include "common.h"
#include "commands.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("带返回值的命令能填结果\n");
    {
        CommandBus::Get().RegisterWithResult(CmdId::OpListDir,
            [](const Args&, json::Value* out) {
                out->Set(L"path", json::Value::MakeStr(L"D:\\demo"));
                out->Set(L"truncated", json::Value::MakeBool(false));
                json::Value arr = json::Value::MakeArray();
                json::Value e = json::Value::MakeObject();
                e.Set(L"name", json::Value::MakeStr(L"a.txt"));
                e.Set(L"isFolder", json::Value::MakeBool(false));
                arr.arr.push_back(e);
                out->Set(L"entries", arr);
                return true;
            });

        json::Value r;
        const bool ok = CommandBus::Get().InvokeResult(CmdId::OpListDir, Args::Empty(), &r,
                                                       Actor{ActorKind::Ai, L"test"});
        Check(ok, "调用成功");
        Check(r.GetStr(L"path") == L"D:\\demo", "path 被填入");
        Check(r.GetBool(L"truncated", true) == false, "truncated 被填入");
        const json::Value* entries = r.Find(L"entries");
        Check(entries && entries->IsArray() && entries->arr.size() == 1, "entries 有 1 项");
        if (entries && entries->arr.size() == 1) {
            Check(entries->arr[0].GetStr(L"name") == L"a.txt", "条目名字正确");
        }
    }

    std::printf("\nHasResult 能区分两类命令\n");
    {
        Check(CommandBus::Get().HasResult(CmdId::OpListDir), "OpListDir 有结果");
        Check(CommandBus::Get().HasResult(L"OpListDir"), "按名查也认");
        Check(!CommandBus::Get().HasResult(CmdId::NavBack), "NavBack 无结果（不编造）");
        Check(!CommandBus::Get().HasResult(L"NavBack"), "按名查也认");
    }

    std::printf("\n无返回值的命令：结果置空对象而非旧数据\n");
    {
        // NavRefresh 有 HasTabs 前置，所以要先给一个"有标签"的状态。
        // （这也顺带验证了第 3 步的前置条件真的在拦住调用。）
        SetStateProbe([] {
            CommandState s;
            s.tabCount = 1;
            return s;
        });
        CommandBus::Get().Register(CmdId::NavRefresh, [](const Args&) { return true; });
        json::Value r;
        r.Set(L"stale", json::Value::MakeStr(L"不该出现"));
        const bool ok = CommandBus::Get().InvokeResult(CmdId::NavRefresh, Args::Empty(), &r,
                                                       Actor{ActorKind::Ai, L"test"});
        Check(ok, "调用成功");
        Check(!r.Find(L"stale"), "旧字段被清掉（不残留上次结果）");
        Check(r.IsObject() && r.obj.empty(), "结果为空对象");
    }

    std::printf("\nresult 指针可以为空（只要成败）\n");
    {
        bool ok = CommandBus::Get().InvokeResult(CmdId::OpListDir, Args::Empty(), nullptr,
                                                 Actor{ActorKind::Ai, L"test"});
        Check(ok, "传 nullptr 不崩溃");
        ok = CommandBus::Get().Invoke(CmdId::OpListDir, Args::Empty(), Actor{ActorKind::Ai, L"test"});
        Check(ok, "Invoke 走同一 handler");
    }

    std::printf("\n前置不满足时也给空结果（不崩）\n");
    {
        // NavBack 有 HasHistoryBack 前置，未注册 probe 时必然拒绝
        SetStateProbe(nullptr);
        json::Value r;
        r.Set(L"stale", json::Value::MakeStr(L"x"));
        const bool ok = CommandBus::Get().InvokeResult(CmdId::NavBack, Args::Empty(), &r,
                                                       Actor{ActorKind::Ai, L"test"});
        Check(!ok, "前置不满足 -> 失败");
        Check(!r.Find(L"stale"), "旧字段被清掉");
    }

    std::printf("\n旧的 bool 入口不受影响（131 条兼容）\n");
    {
        SetStateProbe([] {
            CommandState s;
            s.tabCount = 1;
            s.historyBack = 1;
            return s;
        });
        CommandBus::Get().Register(CmdId::NavBack, [](const Args&) { return true; });
        Check(CommandBus::Get().Invoke(CmdId::NavBack, Args::Empty(), Actor{ActorKind::Ai, L"test"}),
              "普通 Invoke 正常");
        Check(CommandBus::Get().InvokeByName(L"NavBack", Args::Empty(), Actor{ActorKind::Ai, L"test"}),
              "InvokeByName 正常");
    }

    std::printf("\n命令表里声明 outputSchema 的条目结构完整\n");
    {
        int withOut = 0, bad = 0;
        for (const auto& d : CommandTable()) {
            if (!d.outputSchema) continue;
            ++withOut;
            json::Value v;
            std::wstring err;
            // outputSchema 是 const char*（UTF-8），转宽再解析
            std::wstring w;
            int n = MultiByteToWideChar(CP_UTF8, 0, d.outputSchema, -1, nullptr, 0);
            if (n > 1) {
                w.resize(static_cast<size_t>(n - 1));
                MultiByteToWideChar(CP_UTF8, 0, d.outputSchema, -1, &w[0], n);
            }
            if (!json::Parse(w, v, &err) || !v.IsObject()) ++bad;
            if (!d.implemented) ++bad;
        }
        Check(withOut >= 2, "至少有 2 条命令声明了 outputSchema");
        Check(bad == 0, "它们的 outputSchema 都是合法 JSON 对象且已实现");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
