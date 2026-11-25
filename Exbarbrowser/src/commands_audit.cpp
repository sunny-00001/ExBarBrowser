// CommandBus 的审计落盘（拆出来，让 commands.cpp 专注命令表本身）。
//
// 【为什么拆】命令表已经 800 多行，而审计是**另一个职责**：
// 它关心"谁做了什么、成了吗、花了多久"，不关心"这条命令干什么"。
// 两者混在一个文件里时，读命令表的人要一路翻到文件末尾才发现还有一套
// 审计逻辑 —— 而那套逻辑改起来最需要独立看（它是"系统说过什么"的账）。
//
// 【为什么放在 CommandBus 而不是别处】命令有两个入口（人 / 机器），
// 审计要覆盖**两个**入口 —— 只在 AI 通道记，人做的破坏性操作就成了盲区。
// Dispatch 是两个入口唯一的汇合点，审计挂在它下面才不漏。
#include "commands.h"

#include "audit_log.h"
#include "common.h"
#include "config.h"   // auditArgsTextLimit：参数明文保留多少字符（可调）
#include "json.h"

namespace {

// 审计里记的参数摘要。用紧凑 JSON，过长时截断 —— 审计是给人查的，
// 不该因为一条超长路径把日志行撑爆。
//
// 【哈希用的不是这份】参数哈希算在**完整**参数上（见 Audit 里的注释），
// 截断过的文本会撞哈希。
std::wstring DescribeArgs(const Args& args) {
    if (args.IsEmpty()) return std::wstring();
    std::wstring s = json::Stringify(args.raw(), 0);
    // 上限来自设置 auditArgsTextLimit（默认 160）：隐私与可查性的权衡，管理员可调。
    // 哈希仍在完整 JSON 上算（见 Audit），所以调大/调小不影响判重。
    const size_t limit = static_cast<size_t>(Config::Get().s().auditArgsTextLimit);
    if (s.size() > limit) s = s.substr(0, limit > 3 ? limit - 3 : 0) + L"...";
    return s;
}

}  // namespace

void CommandBus::Audit(const CommandDef& def, const Args& args, Actor actor, bool ok,
                       const std::wstring& verdict, const std::wstring& errorCode,
                       const std::wstring& reason, long long durationMs) {
    // 【为什么参数传 Args 而不是传格式化好的字符串】参数要落两样东西：
    // 给人看的摘要（截断）与给机器算的哈希（**完整** JSON）。传字符串进来
    // 就只剩一样 —— 而用截断后的文本算哈希，两组不同的长参数会撞成同一个
    // 哈希，"同参重放"分析就废了。
    //
    // verdict 是**结构化**的判定（ok / failed / blocked / not_implemented），
    // errorCode 是失败时的机器可判读分类。旧实现把两者揉进一个字符串
    // （"(blocked: 已经是顶层)"），机器只能靠匹配文案分派。
    auditlog::Call c;
    c.actorKind = (actor.kind == ActorKind::Ai) ? L"ai" : L"human";
    c.actorName = actor.source ? actor.source : L"?";
    c.cmd = def.name;
    c.argsText = DescribeArgs(args);
    c.argsHash = auditlog::HashArgs(json::Stringify(args.raw(), 0));
    c.ok = ok;
    c.verdict = verdict;
    c.errorCode = errorCode;
    c.reason = reason;
    c.durationMs = durationMs;
    auditlog::Write(auditlog::MakeRecord(c));
}

void CommandBus::AuditRejected(const std::wstring& commandName, const std::wstring& reason,
                                const std::wstring& errorCode) {
    // 与 Audit 同一套格式与同一个文件 —— 拒绝与执行是同一条时间线，
    // 分两个文件的话，事后回溯"这个命令到底被谁动过"要拼两个文件。
    //
    // actor 固定记 "ai"：这条入口只为 AI 通道的拒绝而开（见 commands.h 的说明）。
    // verdict=rejected 与正常调用的 ok=0 区分开：
    // ok=0 表示"执行了但没成"，rejected 表示"**根本没进执行**"。
    auditlog::Call c;
    c.actorKind = L"ai";
    c.actorName = L"ai";
    c.cmd = commandName.empty() ? L"?" : commandName;
    c.verdict = L"rejected";
    c.errorCode = errorCode;
    c.reason = reason;
    auditlog::Write(auditlog::MakeRecord(c));
}
