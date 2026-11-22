// 事件流实现（接口说明见 ai_events.h）。
#include "ai_events.h"

#include "ai_api.h"      // GetState / StateToJson
#include "audit_log.h"   // NowIso：与 audit.jsonl 用同一个时间格式
#include "common.h"      // GetTickCount64 / GetCurrentProcessId
#include "config.h"
#include "file_hash.h"
#include "text_file.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace ai {
namespace events {
namespace {

constexpr size_t kMaxEvents = 512;
constexpr size_t kDefaultLimit = 256;
// 与 audit.jsonl 同量级：2 MB 轮转，每 200 条查一次大小。
constexpr unsigned long long kRotateBytes = 2ull << 20;
constexpr int kCheckEvery = 200;

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Event> g_events;
uint64_t g_lastSeq = 0;
std::atomic<int> g_turn{0};

// 请求上下文只属于当前线程：AI 请求在 UI 线程上执行，后台作业另有线程。
// 把它放在线程局部而不是全局，是为了避免两个线程的事件互相串 trace。
struct ThreadTrace {
    std::wstring traceId;
    std::wstring parentSpanId;   // traceparent 里的上游 span
    std::wstring lastSpanId;     // 本请求内上一条事件的 span（串成链）
    int turn = 0;
    int step = 0;
};
thread_local ThreadTrace t_trace;
thread_local bool t_armed = false;

// 没有请求上下文时的进程级 trace：作业进度、状态轮询等后台事件挂在这里。
std::wstring g_bgTraceId;
std::wstring g_bgLastSpan;
int g_bgStep = 0;

// 状态基线只在 UI 线程碰，不需要锁；但访问 GetState() 必须确保 provider 已注册。
std::wstring g_lastStateText;
bool g_hasStateBaseline = false;

json::Value Num(double n) { return json::Value::MakeNum(n); }

std::wstring Lower(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out)
        if (c >= L'A' && c <= L'F') c = static_cast<wchar_t>(c - L'A' + L'a');
    return out;
}

bool HexVal(wchar_t c, int* v) {
    if (c >= L'0' && c <= L'9') { *v = c - L'0'; return true; }
    if (c >= L'a' && c <= L'f') { *v = c - L'a' + 10; return true; }
    if (c >= L'A' && c <= L'F') { *v = c - L'A' + 10; return true; }
    return false;
}

// 64 hex 原始熵。trace-id 取前 32 位，span-id 取后 16 位。
// 这里要的是"不撞"，不是密码学不可预测 —— 事件 trace 不是访问凭证。
std::wstring NewHex64() {
    struct Seed {
        unsigned long long tick;
        unsigned long long counter;
        unsigned long long pid;
        unsigned long long addr;
    };
    static std::atomic<unsigned long long> counter{0};
    Seed s{};
    s.tick = GetTickCount64();
    s.counter = ++counter;
    s.pid = GetCurrentProcessId();
    s.addr = reinterpret_cast<unsigned long long>(&s);
    std::wstring hex;
    if (!filehash::HashBytes(&s, sizeof(s), filehash::Algo::Sha256, &hex) || hex.size() < 64)
        return std::wstring(64, L'0');
    return hex.substr(0, 64);
}

std::wstring NewSpanId() { return NewHex64().substr(32, 16); }

// 一条事件的磁盘/内存完整投影。EventLine 是"推送给订阅者"的扁平投影，
// 两者同源，避免落盘有 traceId 而推送没有这种半截状态。
json::Value ToValue(const Event& e) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"seq", Num(static_cast<double>(e.seq)));
    v.Set(L"ts", json::Value::MakeStr(e.ts));
    v.Set(L"type", json::Value::MakeStr(e.type));
    v.Set(L"traceId", json::Value::MakeStr(e.traceId));
    v.Set(L"spanId", json::Value::MakeStr(e.spanId));
    v.Set(L"parentSpanId", json::Value::MakeStr(e.parentSpanId));
    v.Set(L"turn", json::Value::MakeInt(e.turn));
    v.Set(L"step", json::Value::MakeInt(e.step));
    v.Set(L"data", e.data);
    return v;
}

std::wstring EventLine(const Event& e) {
    json::Value v = ToValue(e);
    v.Set(L"event", json::Value::MakeBool(true));
    // data 的字段摊到顶层，客户端不必再拆两层；trace 字段留在顶层同名录。
    if (e.data.IsObject())
        for (const auto& kv : e.data.obj) v.Set(kv.first, kv.second);
    else
        v.Set(L"data", e.data);
    return json::Stringify(v, 0);
}

Event FromValue(const json::Value& v) {
    Event e;
    if (const json::Value* s = v.Find(L"seq"); s && s->IsNumber())
        e.seq = static_cast<uint64_t>(s->num);
    e.ts = v.GetStr(L"ts");
    e.type = v.GetStr(L"type");
    e.traceId = v.GetStr(L"traceId");
    e.spanId = v.GetStr(L"spanId");
    e.parentSpanId = v.GetStr(L"parentSpanId");
    e.turn = v.GetInt(L"turn", 0);
    e.step = v.GetInt(L"step", 0);
    if (const json::Value* d = v.Find(L"data")) e.data = *d;
    return e;
}

bool LoadOne(const std::wstring& path, std::deque<Event>* out) {
    std::wstring text;
    if (!tf::ReadText(path, &text) || text.empty()) return false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        json::Value v;
        std::wstring err;
        if (!json::Parse(line, v, &err) || !v.IsObject()) continue;  // 坏行跳过，不整盘作废
        Event e = FromValue(v);
        if (!e.seq) continue;
        out->push_back(std::move(e));
    }
    return true;
}

uint64_t PublishImpl(const std::wstring& type, const json::Value& data,
                     const TraceContext* explicitCtx, int explicitTurn) {
    Event e;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        seq = e.seq = ++g_lastSeq;
        e.type = type;
        // Value 是值语义：这里复制一份快照，调用方随后改 data 不影响已发布事件。
        e.data = data;
        e.ts = auditlog::NowIso();

        const bool useExplicit = explicitCtx != nullptr;
        std::wstring parent;
        if (useExplicit) {
            e.traceId = explicitCtx->traceId.empty() ? g_bgTraceId : explicitCtx->traceId;
            e.parentSpanId = explicitCtx->parentSpanId;
            e.turn = explicitTurn;
            e.step = g_bgStep + 1;
        } else if (t_armed) {
            e.traceId = t_trace.traceId;
            parent = t_trace.lastSpanId.empty() ? t_trace.parentSpanId : t_trace.lastSpanId;
            e.parentSpanId = parent;
            e.turn = t_trace.turn;
            e.step = ++t_trace.step;
        } else {
            e.traceId = g_bgTraceId;
            parent = g_bgLastSpan;
            e.parentSpanId = parent;
            e.turn = 0;
            e.step = ++g_bgStep;
        }
        if (e.traceId.empty()) {
            e.traceId = NewHex64().substr(0, 32);
            g_bgTraceId = e.traceId;
        }
        e.spanId = NewSpanId();

        if (useExplicit) {
            g_bgStep = e.step;
            g_bgLastSpan = e.spanId;
        } else if (t_armed) {
            t_trace.lastSpanId = e.spanId;
        } else {
            g_bgLastSpan = e.spanId;
        }

        // 落盘必须在锁内、紧跟 seq：否则两个线程可能把 seq 大的先写进文件，
        // LoadFromDisk 之后内存与磁盘顺序就对不上了。
        const std::string line = tf::ToUtf8(json::Stringify(ToValue(e), 0)) + "\n";
        if (!tf::AppendLine(EventsPath(), line, kRotateBytes, kCheckEvery))
            LogDbg(L"[events] 落盘失败：seq=%llu", static_cast<unsigned long long>(e.seq));

        g_events.push_back(std::move(e));
        while (g_events.size() > kMaxEvents) g_events.pop_front();
    }
    g_cv.notify_all();
    return seq;
}

}  // namespace

bool ParseTraceparent(const std::wstring& text, TraceContext* out) {
    if (!out || text.size() != 55) return false;
    if (text[2] != L'-' || text[35] != L'-' || text[52] != L'-') return false;
    int hi = 0, lo = 0;
    if (!HexVal(text[0], &hi) || !HexVal(text[1], &lo)) return false;
    if (hi * 16 + lo == 0xff) return false;   // W3C 保留版本，非法
    bool traceZero = true, spanZero = true;
    for (size_t i = 3; i < 35; ++i) {
        int v = 0;
        if (!HexVal(text[i], &v)) return false;
        if (v) traceZero = false;
    }
    for (size_t i = 36; i < 52; ++i) {
        int v = 0;
        if (!HexVal(text[i], &v)) return false;
        if (v) spanZero = false;
    }
    if (traceZero || spanZero) return false;  // 全零 id 不是有效 trace
    int fhi = 0, flo = 0;
    if (!HexVal(text[53], &fhi) || !HexVal(text[54], &flo)) return false;

    TraceContext tmp;
    tmp.traceId = Lower(text.substr(3, 32));
    tmp.parentSpanId = Lower(text.substr(36, 16));
    tmp.flags = static_cast<unsigned char>(fhi * 16 + flo);
    *out = std::move(tmp);
    return true;
}

bool TraceContextFromRequest(const json::Value& request, TraceContext* out) {
    if (!out || !request.IsObject()) return false;
    const json::Value* meta = request.Find(L"_meta");
    if (const json::Value* p = request.Find(L"params")) {
        if (p->IsObject())
            if (const json::Value* m = p->Find(L"_meta")) meta = m;
    }
    if (!meta || !meta->IsObject()) return false;
    TraceContext tmp;
    if (!ParseTraceparent(meta->GetStr(L"traceparent"), &tmp)) return false;
    if (const json::Value* ci = meta->Find(L"clientInfo"))
        tmp.clientInfo = json::Stringify(*ci, 0);
    *out = std::move(tmp);
    return true;
}

RequestScope::RequestScope(const json::Value& request) {
    // HandleRequestLine → HandleRpcLine 是嵌套调用：内层不要再开一轮，
    // 否则一次请求会产生两个 turn，轨迹对不上。
    if (t_armed) return;
    TraceContext tc;
    const bool found = TraceContextFromRequest(request, &tc);
    t_trace.traceId = found ? tc.traceId : NewHex64().substr(0, 32);
    t_trace.parentSpanId = found ? tc.parentSpanId : L"";
    t_trace.lastSpanId.clear();
    t_trace.turn = ++g_turn;
    t_trace.step = 0;
    armed_ = true;
    t_armed = true;
}

RequestScope::~RequestScope() {
    if (!armed_) return;
    t_trace = ThreadTrace{};
    t_armed = false;
}

std::wstring CurrentTraceIdOr(const std::wstring& fallback) {
    if (t_armed && !t_trace.traceId.empty()) return t_trace.traceId;
    return fallback;
}

uint64_t Publish(const std::wstring& type, const json::Value& data) {
    return PublishImpl(type, data, nullptr, 0);
}

uint64_t PublishWith(const std::wstring& type, const json::Value& data,
                     const TraceContext& ctx, int turn) {
    return PublishImpl(type, data, &ctx, turn);
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
            arr.arr.push_back(ToValue(e));
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

std::wstring EventsPath() { return tf::Join(Config::Get().Dir(), L"events.jsonl"); }

void LoadFromDisk() {
    // 先读轮转出去的旧文件，再读当前文件；只留末尾 kMaxEvents 条，
    // 2 MB 的日志不会整份塞进内存。坏行跳过，不把整个历史当成不存在。
    std::deque<Event> loaded;
    LoadOne(EventsPath() + L".1", &loaded);
    LoadOne(EventsPath(), &loaded);
    while (loaded.size() > kMaxEvents) loaded.pop_front();

    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& e : loaded)
        if (e.seq > g_lastSeq) g_lastSeq = e.seq;
    g_events = std::move(loaded);
    if (g_bgTraceId.empty()) g_bgTraceId = NewHex64().substr(0, 32);
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
