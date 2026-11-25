// 事件流单测：重点不是"能发出去"，而是"序号可续、可补历史、断线不静默丢"。
// 客户端重连靠 seq 做基准；一旦序号语义错了，事件流就退化成不可靠的通知。
#include <cstdio>
#include <string>

#include "ai_api.h"
#include "ai_events.h"
#include "common.h"
#include "config.h"
#include "json.h"
#include "text_file.h"

static int g_pass = 0, g_fail = 0;

static void Expect(bool cond, const char* what) {
    if (cond) ++g_pass;
    else { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

static json::Value Parse(const std::wstring& text) {
    json::Value v; std::wstring err;
    if (!json::Parse(text, v, &err)) std::printf("  FAIL  JSON 解析：%s\n", text.c_str());
    return v;
}

int main() {
    using namespace ai::events;
    std::printf("事件流\n");
    Clear();

    const uint64_t base = LastSeq();
    json::Value d = json::Value::MakeObject();
    d.Set(L"command", json::Value::MakeStr(L"view.sort"));
    d.Set(L"ok", json::Value::MakeBool(true));
    const uint64_t s1 = Publish(L"command", d);
    const uint64_t s2 = Publish(L"command", d);
    const uint64_t s3 = Publish(L"note", d);
    Expect(s1 == base + 1 && s2 == base + 2 && s3 == base + 3, "序号严格递增");
    Expect(LastSeq() == s3, "LastSeq 等于最后一条");

    json::Value history = Parse(Since(s1));
    Expect(history.GetBool(L"ok", true), "since 合法");
    Expect(history.Find(L"events") && history.Find(L"events")->arr.size() == 2,
           "since 只取指定序号之后（不含自身）");
    const auto& first = history.Find(L"events")->arr[0];
    Expect(first.GetStr(L"type") == L"command", "事件类型保留");
    Expect(first.Find(L"data") && first.Find(L"data")->GetStr(L"command") == L"view.sort",
           "事件载荷保留");
    Expect(history.Find(L"lastSeq") && static_cast<uint64_t>(history.Find(L"lastSeq")->num) == s3,
           "带回 lastSeq 供客户端续接");

    // 没有新事件时等待必须超时返回，不能忙等或假有一条。
    std::wstring line = L"stale";
    Expect(!WaitFor(s3, &line, 30), "无新事件时等待超时");
    Expect(line == L"stale", "超时不改写调用方的缓冲区");
    const uint64_t s4 = Publish(L"command", d);
    Expect(WaitFor(s3, &line, 500), "发布后等待立即返回");
    json::Value pushed = Parse(line);
    Expect(pushed.GetBool(L"event", false), "推送行带 event 标记，区别于请求响应");
    Expect(static_cast<uint64_t>(pushed.Find(L"seq")->num) == s4, "推送行带正确序号");
    Expect(pushed.GetStr(L"command") == L"view.sort", "载荷字段摊到顶层，客户端不必拆两层");

    // 断线太久：缓冲淘汰后必须报 gap，不能让客户端以为"没有新事件"。
    for (int i = 0; i < 600; ++i) Publish(L"flood", d);
    json::Value gapped = Parse(Since(0, 10));
    Expect(gapped.GetBool(L"gap", false), "历史被淘汰时明确报 gap");
    Expect(gapped.Find(L"events")->arr.size() <= 512, "缓冲有上限，不会无限增长");

    // 状态变化的检测与推送分离：PollStateChanges 自己判断有没有变。
    Clear();
    ai::SetStateProvider([] {
        ai::StateSnapshot s;
        s.activePath = L"C:\\one";
        return s;
    });
    ResetStateBaseline();
    const uint64_t b0 = LastSeq();
    PollStateChanges();
    Expect(LastSeq() == b0 + 1, "首次 Poll 发布一条初始状态");
    PollStateChanges();
    Expect(LastSeq() == b0 + 1, "状态没变时不再重复发布");
    ai::SetStateProvider([] {
        ai::StateSnapshot s;
        s.activePath = L"C:\\two";
        return s;
    });
    PollStateChanges();
    Expect(LastSeq() == b0 + 2, "状态变了才再发布");
    json::Value st = Parse(Since(b0 + 1));
    const json::Value* one = st.Find(L"events") && !st.Find(L"events")->arr.empty()
                                 ? &st.Find(L"events")->arr[0] : nullptr;
    Expect(one && one->GetStr(L"type") == L"state.changed", "事件类型是 state.changed");
    Expect(one && one->Find(L"data") && one->Find(L"data")->Find(L"state") &&
               one->Find(L"data")->Find(L"state")->GetStr(L"activePath") == L"C:\\two",
           "载荷里带的是新状态，不是旧快照");
    ai::SetStateProvider(nullptr);

    // ---- B5 / B12：W3C traceparent 与三级轨迹 ----
    // 解析必须严格：格式不对就当没有，绝不"尽力猜一个 traceId"——
    // 猜出来的 trace 会让两个无关请求在观测面上并成一条，比没有更糟。
    const std::wstring tp = L"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    TraceContext tc;
    Expect(ParseTraceparent(tp, &tc), "合法 traceparent 通过");
    Expect(tc.traceId == L"4bf92f3577b34da6a3ce929d0e0e4736", "trace-id 取 32 hex");
    Expect(tc.parentSpanId == L"00f067aa0ba902b7", "span-id 取 16 hex");
    Expect(tc.flags == 1, "trace flags 取对");
    Expect(!ParseTraceparent(L"ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", &tc),
           "ff 版本（W3C 保留）拒绝");
    Expect(!ParseTraceparent(L"00-00000000000000000000000000000000-00f067aa0ba902b7-01", &tc),
           "全零 trace-id 拒绝");
    Expect(!ParseTraceparent(L"00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01", &tc),
           "全零 span-id 拒绝");
    Expect(!ParseTraceparent(L"00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01", &tc),
           "非 hex 字符拒绝");
    Expect(!ParseTraceparent(L"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0111", &tc),
           "长度不对拒绝");

    // _meta 可能在 params 里（JSON-RPC），也可能在顶层（旧格式）。
    json::Value meta = json::Value::MakeObject();
    meta.Set(L"traceparent", json::Value::MakeStr(tp));
    json::Value params = json::Value::MakeObject();
    params.Set(L"_meta", meta);
    json::Value req = json::Value::MakeObject();
    req.Set(L"params", params);
    TraceContext fromReq;
    Expect(TraceContextFromRequest(req, &fromReq) && fromReq.traceId == tc.traceId,
           "params._meta.traceparent 能读到");
    json::Value reqTop = json::Value::MakeObject();
    reqTop.Set(L"_meta", meta);
    Expect(TraceContextFromRequest(reqTop, &fromReq), "顶层 _meta 也能读到");
    json::Value bad = json::Value::MakeObject();
    bad.Set(L"params", json::Value::MakeObject());
    Expect(!TraceContextFromRequest(bad, &fromReq), "没有 _meta 返回 false，不编造 trace");

    Clear();
    const uint64_t tb = LastSeq();
    {
        RequestScope scope(req);
        Expect(CurrentTraceIdOr(L"fallback") == tc.traceId, "请求上下文里的 traceId 覆盖 fallback");
        Publish(L"invoke_agent", d);
        Publish(L"execute_tool", d);
        {
            RequestScope nested(req);
            Publish(L"chat", d);
        }
        json::Value traced = Parse(Since(tb));
        const auto& evs = traced.Find(L"events")->arr;
        Expect(evs.size() == 3, "三条轨迹事件都发出");
        Expect(evs[0].GetStr(L"traceId") == tc.traceId, "事件带 W3C traceId");
        Expect(evs[0].GetStr(L"parentSpanId") == tc.parentSpanId, "首条事件挂上游 span");
        Expect(evs[1].GetStr(L"parentSpanId") == evs[0].GetStr(L"spanId"),
               "同轮内 span 串成链");
        Expect(evs[2].GetInt(L"turn", -1) == evs[0].GetInt(L"turn", -1),
               "嵌套 scope 不另开一轮");
        Expect(evs[0].GetInt(L"step", 0) == 1 && evs[2].GetInt(L"step", 0) == 3,
               "step 在本轮内递增");
        Expect(!evs[0].GetStr(L"ts").empty(), "事件带 ISO 时间戳");
    }
    Expect(CurrentTraceIdOr(L"fallback") == L"fallback", "离开请求边界后上下文恢复");

    // ---- B8：落盘 + 跨重启恢复 ----
    // 用独立 profile 根，免得测试把真实 logs/events.jsonl 当成自己的草稿纸。
    const std::wstring originalRoot = Config::Get().RootDir();
    wchar_t tmp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring tmpRoot =
        tf::Join(std::wstring(tmp), L"exbar-events-test_" + std::to_wstring(GetCurrentProcessId()));
    tf::EnsureDir(tmpRoot);
    tf::EnsureDir(tf::Join(tmpRoot, L"logs"));  // 临时验证：logs 子目录是否存在是唯一变量
    Config::Get().SetProfileDir(tmpRoot);
    const std::wstring path = EventsPath();
    DeleteFileW(tf::ToExtended(path).c_str());
    DeleteFileW(tf::ToExtended(path + L".1").c_str());

    Clear();
    const uint64_t db = LastSeq();
    {
        RequestScope scope(req);
        Publish(L"command", d);
    }
    Expect(tf::Exists(path), "事件写到 logs/events.jsonl");
    std::wstring disk;
    Expect(tf::ReadText(path, &disk) &&
               disk.find(L"\"traceId\"") != std::wstring::npos &&
               disk.find(L"\"step\"") != std::wstring::npos,
           "落盘行带 traceId / step（不是只写一行 type）");
    Clear();
    Expect(tf::Exists(path), "Clear 只清内存，不删磁盘历史");
    LoadFromDisk();
    Expect(LastSeq() == db + 1, "重启后序号从磁盘接回，不从头开始");
    json::Value revived = Parse(Since(db));
    const auto& rev = revived.Find(L"events")->arr;
    Expect(rev.size() == 1 && rev[0].GetStr(L"traceId") == tc.traceId,
           "重启后补回历史且三级轨迹不丢");
    Config::Get().SetProfileDir(originalRoot);

    std::printf("  通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
