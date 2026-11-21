// 事件流实现（接口说明见 ai_events.h）。
#include "ai_events.h"

#include "ai_api.h"
#include "common.h"   // GetTickCount64 通过 windows.h

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace ai {
namespace events {
namespace {

constexpr size_t kMaxEvents = 512;
constexpr size_t kDefaultLimit = 256;

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Event> g_events;
uint64_t g_lastSeq = 0;

// 状态基线只在 UI 线程碰，不需要锁；但访问 GetState() 必须确保 provider 已注册。
std::wstring g_lastStateText;
bool g_hasStateBaseline = false;

json::Value Num(double n) { return json::Value::MakeNum(n); }

// 把 data 的字段摊到一条事件消息顶层，客户端不必再拆一层。
std::wstring EventLine(const Event& e) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"event", json::Value::MakeBool(true));
    v.Set(L"seq", Num(static_cast<double>(e.seq)));
    v.Set(L"type", json::Value::MakeStr(e.type));
    if (e.data.IsObject())
        for (const auto& kv : e.data.obj) v.Set(kv.first, kv.second);
    else
        v.Set(L"data", e.data);
    return json::Stringify(v, 0);
}

}  // namespace

uint64_t Publish(const std::wstring& type, const json::Value& data) {
    Event e;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        seq = e.seq = ++g_lastSeq;
        e.type = type;
        // Value 是值语义：这里复制一份快照，调用方随后改 data 不影响已发布事件。
        e.data = data;
        g_events.push_back(std::move(e));
        while (g_events.size() > kMaxEvents) g_events.pop_front();
    }
    g_cv.notify_all();
    return seq;
}

uint64_t LastSeq() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_lastSeq;
}

std::wstring Since(uint64_t afterSeq, size_t limit) {
    if (limit == 0 || limit > kMaxEvents) limit = kDefaultLimit;
    json::Value arr = json::Value::MakeArray();
    uint64_t first = 0, last = 0;
    bool gap = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        last = g_lastSeq;
        size_t taken = 0;
        for (const auto& e : g_events) {
            if (e.seq <= afterSeq) continue;
            if (!first) first = e.seq;
            arr.arr.push_back(json::Value::MakeObject());
            json::Value& one = arr.arr.back();
            one.Set(L"seq", Num(static_cast<double>(e.seq)));
            one.Set(L"type", json::Value::MakeStr(e.type));
            one.Set(L"data", e.data);
            if (++taken >= limit) break;
        }
        if (!g_events.empty()) gap = g_events.front().seq > afterSeq + 1;
    }
    json::Value v = json::Value::MakeObject();
    v.Set(L"events", arr);
    v.Set(L"lastSeq", Num(static_cast<double>(last)));
    if (gap) {
        // 断线太久、缓冲已经淘汰：明确告诉客户端"中间漏了"，别让它以为没有新事件。
        v.Set(L"gap", json::Value::MakeBool(true));
        v.Set(L"firstAvailable", Num(static_cast<double>(first)));
    }
    return json::Stringify(v, 0);
}

bool WaitFor(uint64_t afterSeq, std::wstring* line, int timeoutMs) {
    std::unique_lock<std::mutex> lk(g_mu);
    auto ready = [&] {
        for (const auto& e : g_events)
            if (e.seq > afterSeq) return true;
        return false;
    };
    if (timeoutMs < 0)
        g_cv.wait(lk, ready);
    else if (!g_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), ready))
        return false;
    for (const auto& e : g_events) {
        if (e.seq <= afterSeq) continue;
        if (line) *line = EventLine(e);
        return true;
    }
    return false;
}

void Clear() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_events.clear();
}

void PollStateChanges() {
    const std::wstring now = StateToJson(GetState());
    if (g_hasStateBaseline && now == g_lastStateText) return;
    g_hasStateBaseline = true;
    g_lastStateText = now;

    json::Value state = json::Value::MakeObject();
    std::wstring perr;
    if (!json::Parse(now, state, &perr)) {
        // 快照自己序列化出来的东西解析不了，是实现缺陷；不把它当事件发出去。
        LogDbg(L"[events] 状态快照解析失败：%s", perr.c_str());
        return;
    }
    json::Value data = json::Value::MakeObject();
    data.Set(L"state", state);
    Publish(L"state.changed", data);
}

void ResetStateBaseline() {
    g_hasStateBaseline = false;
    g_lastStateText.clear();
}

}  // namespace events
}  // namespace ai
