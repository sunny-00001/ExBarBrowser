// 命令审计的**唯一事实源与唯一落盘处**。
//
// 【为什么单独一个文件】
// 审计曾经只有一行人类可读文本塞进 audit.log。要补 C6 要求的
// 参数哈希 / 耗时 / 是否审批，就只能往文本里继续塞 —— 塞到最后只能靠 grep
// 文本判结构，那和没有结构一样。R18 要求的其实是 audit.jsonl。
//
// 现在事实是**结构化的**（Record），渲染是它的投影：
//   · audit.jsonl —— 规范格式，一行一条 JSON，机器可判读（R18）
//   · audit.log   —— 旧文本格式，留着是为了不打断既有脚本
// 两者由**同一个 Record** 渲染，不存在"两个来源对不上"的可能 ——
// 真要两份各写一遍，那才会漂移。
//
// 【为什么参数哈希要单独一列】
// argsText 是给人看的（会被截断到 160 字符），argsHash 是给机器判
// "这是不是同一次调用"用的。哈希算的是**完整**参数 JSON ——
// 用截断后的文本算，两组不同的长参数会撞成同一个哈希，
// "同参重放"分析就废了。
//
// 【依赖方向】这个头**不依赖 commands.h** —— commands.h 里住着 Actor/ActorKind，
// 而 commands.h 又要用 Record。绕开的办法：Record 里存的是普通字符串，
// Actor → 字符串的转换放在调用方（commands.cpp）做。
#pragma once
#include <string>

namespace auditlog {

// 一次审计调用要落的**全部内容**（不含上下文派生出来的部分）。
//
// 【为什么单独一个 Call 而不是直接用 Record】
// Record 里有几项是**派生**的：时间戳、审批状态、sessionId、traceId、pid ——
// 它们不来自调用点，而来自当前上下文与当前进程。
// 把"调用点知道的"与"派生的"分开，调用点就只填自己知道的那几项，
// 派生的那几项由 MakeRecord 统一补 —— 少一个漏填的机会。
struct Call {
    std::wstring actorKind;   // "human" / "ai"
    std::wstring actorName;   // "ui"/"key"/"mouse"/"sysmenu"/"palette"/"seq"/"drop"/"ai"
    std::wstring cmd;         // 命令规范名
    std::wstring argsText;    // 人可读参数（已截断）
    std::wstring argsHash;    // 完整参数的 SHA-256 前 64 位（十六进制）
    bool ok = false;          // handler 本身成功？
    std::wstring verdict;     // ok / failed / blocked / rejected / not_implemented / effect_not_met
    std::wstring errorCode;   // 机器可判读的失败分类（可为空）
    // 人可读的失败原因（可为空）。
    //
    // 【为什么它不能塞在 errorCode 里】errorCode 是给机器分支用的
    // （precondition_unmet / lock_conflict / …），塞中文进去，机器就
    // 只能靠匹配文案分派 —— 文案一改就断。而人可读的原因本身是有价值的，
    // 它该有自己的位置，而不是和机器码抢一个字段。
    std::wstring reason;
    long long durationMs = 0; // 耗时（毫秒）
};

// 一条审计记录的全部字段。
//
// 【为什么字段这么多】每一个都对应一个"事后问不出来"的问题：
//   谁做的（actorKind/actorName）、做了什么（cmd/args）、
//   是不是同一次调用（argsHash）、成没成（ok/verdict/errorCode）、
//   花了多久（durationMs）、谁批的（approval/approvedBy）、
//   属于哪次会话（sessionId/traceId）、哪个进程（pid）。
// 少一个字段，就有一类问题永远只能靠猜。
struct Record {
    std::wstring ts;          // ISO-8601 本地时间（含毫秒）
    std::wstring actorKind;   // "human" / "ai"
    std::wstring actorName;   // "ui"/"key"/"mouse"/"sysmenu"/"palette"/"seq"/"drop"/"ai"
    std::wstring cmd;         // 命令规范名
    std::wstring argsText;    // 人可读参数（已截断）
    std::wstring argsHash;    // 完整参数的 SHA-256 前 64 位（十六进制）
    bool ok = false;          // handler 本身成功？
    std::wstring verdict;     // ok / failed / blocked / rejected / not_implemented / effect_not_met
    std::wstring errorCode;   // 机器可判读的失败分类（可为空）
    std::wstring reason;      // 人可读的失败原因（可为空）
    long long durationMs = 0; // 耗时（毫秒）
    std::wstring approval;    // not_required / granted / required / denied
    std::wstring approvedBy;  // 批准者（"ai" = 调用方声明已获人类确认；无则 "-"）
    std::wstring sessionId;   // AI 会话标识（无则 "-"）
    std::wstring traceId;     // 追踪 ID（无则 "-"）
    long long pid = 0;        // 进程号
};

// 由 Call 补出派生字段（时间戳 / 审批 / 会话 / 进程号），得到一条完整记录。
Record MakeRecord(const Call& c);

// 当前时间：ISO-8601 本地时间（含毫秒）。
//
// 【为什么时间戳由这里生成】审计的两份投影（JSONL 与文本）必须**同一时刻**，
// 否则两行之间会差几微秒，事后对不上。时间戳是记录的一部分，
// 由记录自己的模块生成，比让每个调用点各取一次更靠得住。
std::wstring NowIso();

// 参数哈希：完整参数 JSON 的 SHA-256，十六进制**前 64 位**。
//
// 为什么只取 64 位：64 位碰撞概率在本仓的调用量级下可以忽略，
// 而 64 字符的哈希会把每条审计行撑成两倍 —— 审计行是要被人 grep 的。
std::wstring HashArgs(const std::wstring& argsJson);

// 渲染成一行 JSON（**不含**换行）。
std::wstring ToJsonLine(const Record& r);

// 渲染成一行人类可读文本（与旧 audit.log 同格式；legacy 镜像用）。
std::wstring ToTextLine(const Record& r);

// 落盘：写 <dataDir>/audit.jsonl（UTF-8，按大小轮转）+ audit.log（legacy 镜像）。
//
// 写失败不抛异常、不影响命令本身 —— 审计写不进去不能让用户的操作失败。
// 但那本身是个事故，所以返回 false 让调用方能记日志。
bool Write(const Record& r);

// ---- 调用上下文（谁在调、这次调用属于哪个会话）----
//
// 【为什么用上下文而不是把参数一路传下去】
// 审计写在**拒绝**的路径上，而拒绝可能发生在还没有 CommandDef 的地方
// （未知命令根本没有 def），参数传不下去。
struct Context {
    std::wstring sessionId;   // 会话标识
    std::wstring traceId;     // 追踪 ID
    std::wstring approval;    // 本次调用的审批状态
    std::wstring approvedBy;  // 批准者
};

// 设置当前上下文，返回旧值（便于恢复）。
Context SetContext(const Context& c);
Context GetContext();

// 只改审批状态（AI 通道用）。其余上下文字段不动。
//
// 【为什么不塞进 SetContext】审批状态是**这次调用**的属性，
// 而 sessionId/traceId 是**这次请求**的属性 —— 两者生命周期不同，
// 混在一个结构体里就总有人会把上一个请求的审批状态带过来。
//
// 取值：
//   not_required —— 这条命令不是破坏性的
//   granted     —— 破坏性命令且调用方带 approve=true（已获人类确认）
//   required    —— 破坏性命令但这次没带批准（被拒）
//   denied      —— 设置里关掉了危险命令（被拒，且根本不让批）
void SetApproval(const std::wstring& approval, const std::wstring& approvedBy = L"");

// RAII 作用域：出作用域自动恢复旧上下文。
//
// 【为什么必须用 RAII 而不是"设了再改回来"】
// 命令会嵌套调用（面板追问后调另一条），中途 return 一次就会把上下文
// 留在错的状态上 —— 下一条命令于是带上上一条的 sessionId。
// 作用域守卫让"忘了恢复"这件事在结构上不可能发生。
class Scope {
public:
    explicit Scope(Context c) : prev_(SetContext(c)) {}
    ~Scope() { SetContext(prev_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Context prev_;
};

// 一次 AI 调用的审计作用域：会话固定 "ai"，追踪标识由调用方给。
//
// 【为什么用上下文而不是往下传参数】审批判定在 AI 通道（ai_api.cpp），
// 而审计落在 CommandBus 里 —— 往下传的话每个中间函数都多一个参数，
// 而且**拒绝**那条路根本没有 CommandDef 可传。上下文是唯一能同时覆盖
// 两条路的办法。
class AiScope {
public:
    explicit AiScope(const std::wstring& traceId);
    ~AiScope();

private:
    Context prev_;
};

}  // namespace auditlog
