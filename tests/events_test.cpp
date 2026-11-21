// 事件流单测：重点不是"能发出去"，而是"序号可续、可补历史、断线不静默丢"。
// 客户端重连靠 seq 做基准；一旦序号语义错了，事件流就退化成不可靠的通知。
#include <cstdio>
#include <string>

#include "ai_api.h"
#include "ai_events.h"
#include "json.h"

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

    std::printf("  通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
