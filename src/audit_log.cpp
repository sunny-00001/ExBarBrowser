// 命令审计的落盘实现（唯一）。
//
// 事实源是 audit_log.h 里的 Record；渲染（JSONL / 旧文本）是它的投影。
// 两种投影都由**同一个** Record 生成，所以不存在"两个来源对不上"的可能。
#include "common.h"

#include "audit_log.h"

#include "config.h"
#include "file_hash.h"
#include "json.h"
#include "text_file.h"

namespace auditlog {
namespace {

// 审计文件的位置与轮转阈值。
//
// 【为什么两个文件、两套阈值】
// audit.jsonl 是规范格式：机器读，按大小轮转。
// audit.log 是旧文本镜像：既有脚本在读，同样按大小轮转。
// 两者阈值都来自同一个设置 auditRotateKB（默认 2048=2MB）—— 但因为是同一条
// Record 渲染，内容不会对不上。轮转大小不再是写死的裸数字，合规场景可调大。
// 检查频率（每多少条查一次文件大小）是内部调优常量，不暴露给用户。
constexpr int kJsonlCheckEvery = 200;
constexpr int kTextCheckEvery = 200;

// 参数文本的显示上限来自设置 auditArgsTextLimit（默认 160，与旧 DescribeArgs 一致）。

// 当前上下文。thread_local：AI 通道的命令全在 UI 线程上跑，
// 但万一将来 JSON-RPC 路径挪到别的线程，也不会互相污染。
thread_local Context g_ctx;

std::wstring OrDash(const std::wstring& s) { return s.empty() ? L"-" : s; }

}  // namespace

std::wstring NowIso() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02dT%02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

Record MakeRecord(const Call& c) {
    Record r;
    r.ts = NowIso();
    r.actorKind = c.actorKind;
    r.actorName = c.actorName;
    r.cmd = c.cmd;
    r.argsText = c.argsText;
    r.argsHash = c.argsHash;
    r.ok = c.ok;
    r.verdict = c.verdict;
    r.errorCode = c.errorCode;
    r.reason = c.reason;
    r.durationMs = c.durationMs;
    // 派生字段：审批状态与会话来自当前上下文，进程号来自当前进程。
    // 由这里统一补，调用点就不会漏填其中之一。
    const Context ctx = GetContext();
    r.approval = ctx.approval.empty() ? L"not_required" : ctx.approval;
    r.approvedBy = ctx.approvedBy;
    r.sessionId = ctx.sessionId;
    r.traceId = ctx.traceId;
    r.pid = static_cast<long long>(GetCurrentProcessId());
    return r;
}

std::wstring HashArgs(const std::wstring& argsJson) {
    if (argsJson.empty()) return L"";
    std::string utf8 = tf::ToUtf8(argsJson);
    std::wstring hex;
    if (!filehash::HashBytes(utf8.data(), utf8.size(), filehash::Algo::Sha256, &hex)) return L"";
    // 只取前 64 位（16 个十六进制字符）：64 位碰撞概率在本仓调用量级下可忽略，
    // 而完整 64 字符会把每行撑成两倍 —— 审计行是要被人 grep 的。
    return hex.size() > 16 ? hex.substr(0, 16) : hex;
}

std::wstring ToJsonLine(const Record& r) {
    // 用 json::Value 渲染而不是手拼字符串：转义（引号、反斜杠、控制字符、
    // 换行）交给唯一的实现 —— 手拼一遍 JSON 转义，迟早漏一个字符。
    json::Value o = json::Value::MakeObject();
    o.Set(L"ts", json::Value::MakeStr(r.ts));
    o.Set(L"actorKind", json::Value::MakeStr(OrDash(r.actorKind)));
    o.Set(L"actorName", json::Value::MakeStr(OrDash(r.actorName)));
    o.Set(L"cmd", json::Value::MakeStr(OrDash(r.cmd)));
    o.Set(L"args", json::Value::MakeStr(r.argsText));
    o.Set(L"argsHash", json::Value::MakeStr(OrDash(r.argsHash)));
    o.Set(L"ok", json::Value::MakeBool(r.ok));
    o.Set(L"verdict", json::Value::MakeStr(OrDash(r.verdict)));
    o.Set(L"errorCode", json::Value::MakeStr(r.errorCode));
    o.Set(L"reason", json::Value::MakeStr(OrDash(r.reason)));
    o.Set(L"durationMs", json::Value::MakeNum(static_cast<double>(r.durationMs)));
    o.Set(L"approval", json::Value::MakeStr(OrDash(r.approval)));
    o.Set(L"approvedBy", json::Value::MakeStr(OrDash(r.approvedBy)));
    o.Set(L"sessionId", json::Value::MakeStr(OrDash(r.sessionId)));
    o.Set(L"traceId", json::Value::MakeStr(OrDash(r.traceId)));
    o.Set(L"pid", json::Value::MakeNum(static_cast<double>(r.pid)));
    return json::Stringify(o, 0);
}

std::wstring ToTextLine(const Record& r) {
    // 文本镜像是**旧格式的兼容投影**，不是新格式 —— 新格式是 audit.jsonl。
    //
    // 【为什么必须逐字复刻旧形状】既有脚本按这些记号筛：
    //   · `ok=0` / `ok=1` 出现在**行尾**（`grep 'ok=0$'` 这类锚定要成立）；
    //   · `(blocked: …)` / `(failed: …)` 括号里装的是**人可读的原因**；
    //   · `rejected=<机器码>` 是被拒调用专用记号（旧版 AI 通道把机器码传给了它）。
    // 我第一版把三者统一成 `ok=0 (verdict: 原因)` 放在行尾 —— 结果
    // `rejected=` 这个记号彻底消失、`ok=` 不再是行尾，两个既有筛法同时失效。
    // "新格式更整齐"换"旧脚本看不见拒绝"，是净亏。
    //
    // 所以：字段顺序照旧（arg= … [记号] ok=N），机器码留在 rejected= 里，
    // 人可读原因留在 (blocked:/failed:) 里；两者的完整结构化版本在 JSONL。
    // 旧版占位符是 "?"（actor.source 为空时），不是 "-" ——
    // "-" 是 JSONL 那一侧的约定（OrDash）。两侧各自保持自己的旧约定。
    const std::wstring who = r.actorName.empty() ? L"?" : r.actorName;
    const std::wstring cmd = r.cmd.empty() ? L"?" : r.cmd;

    // arg= 后面那一段；mid 是 arg= 与 ok= 之间的额外记号（只有被拒调用有）。
    std::wstring desc;
    std::wstring mid;
    if (r.verdict == L"rejected") {
        // 旧版被拒行是 `arg=- rejected=<码> ok=0`：
        // 参数恒为 "-"（拒绝发生在参数被接受之前），机器码单独占一个记号。
        desc = L"-";
        const std::wstring code = r.errorCode.empty() ? r.reason : r.errorCode;
        mid = L" rejected=" + (code.empty() ? std::wstring(L"-") : code);
    } else if (r.verdict == L"blocked") {
        // 旧版：`DescribeArgs(args) + " (blocked: " + reason + ")"` —— 恒定拼接，
        // 参数为空时留下一个前导空格（`arg= (blocked: …)`）。照抄，不"修好看"。
        desc = r.argsText + L" (blocked: " + r.reason + L")";
    } else if (r.verdict == L"failed" && !r.reason.empty()) {
        // 旧版 ArgsDescForAudit：参数为空时**只**留括号（无前导空格）。
        desc = r.argsText.empty() ? (L"(failed: " + r.reason + L")")
                                  : (r.argsText + L" (failed: " + r.reason + L")");
    } else {
        desc = r.argsText.empty() ? L"-" : r.argsText;
    }

    return r.ts + L" " + who + L" " + cmd + L" arg=" + desc + mid + L" ok=" +
           (r.ok ? L"1" : L"0");
}

bool Write(const Record& r) {
    const std::wstring dir = Config::Get().LogsDir();
    if (dir.empty()) return false;
    tf::EnsureDir(dir);

    // 规范格式：audit.jsonl（R18 要求的文件名）。
    const std::wstring jsonl = tf::Join(dir, L"audit.jsonl");
    const std::string line = tf::ToUtf8(ToJsonLine(r)) + "\n";
    // 轮转大小来自设置 auditRotateKB（默认 2048=2MB）：合规场景可调大以延长回溯。
    // 检查频率是内部调优常量（见文件头），不暴露。
    const unsigned long long rotateBytes =
        static_cast<unsigned long long>(Config::Get().s().auditRotateKB) * 1024ull;
    const bool okJson = tf::AppendLine(jsonl, line, rotateBytes, kJsonlCheckEvery);

    // 旧文本镜像：同一份 Record 的另一种投影。
    const std::wstring text = tf::Join(dir, L"audit.log");
    const std::string line2 = tf::ToUtf8(ToTextLine(r)) + "\r\n";
    const bool okText = tf::AppendLine(text, line2, rotateBytes, kTextCheckEvery);

    if (!okJson || !okText) {
        // 审计写不进去是事故，但不能让用户的操作失败 —— 记一条日志就够了。
        LogDbg(L"[audit] 落盘失败：jsonl=%d text=%d（%ls）", okJson ? 1 : 0, okText ? 1 : 0,
               r.cmd.c_str());
    }
    return okJson && okText;
}

Context SetContext(const Context& c) {
    Context prev = g_ctx;
    g_ctx = c;
    return prev;
}

Context GetContext() { return g_ctx; }

void SetApproval(const std::wstring& approval, const std::wstring& approvedBy) {
    g_ctx.approval = approval;
    g_ctx.approvedBy = approvedBy;
}

AiScope::AiScope(const std::wstring& traceId) {
    Context c;
    c.sessionId = L"ai";
    c.traceId = traceId;
    // approval 留空：MakeRecord 会把空值解释成 not_required。
    // 破坏性命令走到 AI 通道时再显式改成 granted / required / denied。
    prev_ = SetContext(c);
}

AiScope::~AiScope() { SetContext(prev_); }

}  // namespace auditlog
