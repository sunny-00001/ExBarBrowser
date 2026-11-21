// AI 操作接口实现（详见 ai_api.h 的设计说明）。
//
// 这一层刻意做得很薄：它不实现任何业务，只做四件事
//   1. 把命令表的 AI 元数据投影成工具列表；
//   2. 校验参数（fail-closed，不兜底）；
//   3. 把 JSON 参数扁平成 CommandBus 的单一字符串 arg；
//   4. 调 CommandBus 并以 actor="ai" 落审计。
//
// 不加业务逻辑是有意的：一旦这里出现"顺手做点什么"，它就会变成第二个
// 写实现，与工具栏/快捷键的行为分叉（架构规则 §4 原则 6）。
#include "ai_api.h"

#include "assert_check.h"
#include "ai_events.h"
#include "audit_log.h"
#include "str.h"

#include "cmd_naming.h"

#include "common.h"   // LogDbg
#include "config.h"
#include "text_file.h"

namespace ai {
namespace {

StateFn g_state;

// UTF-8 转换的唯一实现在 str.cpp（以前 ai_api / ai_server / commands / text_file
// 各有一份 MultiByteToWideChar 样板）。空 schema 的业务语义（"{}"）留在这里。
std::wstring Utf8ToWide(const char* s) {
    std::wstring w = str::FromUtf8(s);
    return w.empty() ? std::wstring(L"{}") : w;
}

// argsSchema 的 JSON Schema 校验。
//
// 为什么不再需要 kArgKeys 之类的映射表：命令 handler 现在直接收具名参数对象
// （Args），JSON 参数原样传下去即可。以前 handler 只收一个字符串，才不得不在
// 中间放一张"哪个键对应那个参数"的表 —— 那是缺陷的症状，不是解法。
//
// 这里只做"该拒绝的拒绝"，不做类型强转之外的加工：
//   · 缺必填项      → 拒绝
//   · 无参数命令收到参数 → 拒绝（静默忽略参数是最难查的错误）
//   · 枚举/范围越界 → 拒绝
bool ValidateSchemaImpl(const json::Value& schema, const json::Value& args,
                           std::wstring* err) {
    auto fail = [err](const std::wstring& m) {
        if (err) *err = m;
        return false;
    };

    const json::Value* props = schema.Find(L"properties");
    const json::Value* req = schema.Find(L"required");
    const bool hasProps = props && props->IsObject() && !props->obj.empty();

    // 无参数命令：只接受空对象。空 schema（{}）视为"无参数"。
    if (!hasProps) {
        if (args.IsNull()) return true;
        if (args.IsObject() && args.obj.empty()) return true;
        return fail(L"该命令不接受参数");
    }

    if (!args.IsObject()) return fail(L"args 必须是 JSON 对象");

    // 未声明的键：拒绝。放过它等于纵容调用方以为参数生效了。
    for (const auto& kv : args.obj) {
        if (!props->Find(kv.first.c_str()))
            return fail(L"未知参数：" + kv.first);
    }

    // 必填项
    if (req && req->IsArray()) {
        for (const auto& r : req->arr) {
            if (!r.IsString()) continue;
            const json::Value* v = args.Find(r.str.c_str());
            if (!v || v->IsNull()) return fail(L"缺少必填参数：" + r.str);
        }
    }

    // 逐项校验已给出的参数：类型 / 枚举 / 范围
    for (const auto& kv : args.obj) {
        const json::Value* spec = props->Find(kv.first.c_str());
        if (!spec) continue;
        const json::Value& val = kv.second;
        if (val.IsNull()) continue;

        const std::wstring want = spec->GetStr(L"type");
        if (want == L"integer") {
            if (val.type != json::Value::kNumber || val.num != static_cast<double>(static_cast<long long>(val.num)))
                return fail(L"参数需要整数：" + kv.first);
        } else if (want == L"number") {
            if (val.type != json::Value::kNumber)
                return fail(L"参数需要数字：" + kv.first);
        } else if (want == L"string") {
            if (val.type != json::Value::kString)
                return fail(L"参数需要字符串：" + kv.first);
        } else if (want == L"boolean") {
            if (val.type != json::Value::kBool)
                return fail(L"参数需要布尔：" + kv.first);
        } else if (want == L"array") {
            // 数组参数（如 FsDelete 的 targets）。不校验的话，
            // 传错类型会一路走到 handler 里变成"空数组 -> 命令执行失败"，
            // 报错信息完全指不到"你传的不是数组"这个真正原因。
            if (!val.IsArray()) return fail(L"参数需要数组：" + kv.first);
            // 逐项校验 items 声明的类型（通常是 string）
            if (const json::Value* items = spec->Find(L"items")) {
                const std::wstring itype = items->GetStr(L"type");
                if (!itype.empty()) {
                    for (const auto& e : val.arr) {
                        if (itype == L"string" && !e.IsString())
                            return fail(L"数组元素需要字符串：" + kv.first);
                        if (itype == L"integer" &&
                            (e.type != json::Value::kNumber ||
                             e.num != static_cast<double>(static_cast<long long>(e.num))))
                            return fail(L"数组元素需要整数：" + kv.first);
                        if (itype == L"number" && e.type != json::Value::kNumber)
                            return fail(L"数组元素需要数字：" + kv.first);
                        if (itype == L"boolean" && e.type != json::Value::kBool)
                            return fail(L"数组元素需要布尔：" + kv.first);
                    }
                }
            }
        }

        // enum（值可以是字符串或数字）
        if (const json::Value* en = spec->Find(L"enum")) {
            if (en->IsArray()) {
                bool hit = false;
                for (const auto& e : en->arr) {
                    if (e.type == val.type &&
                        ((e.type == json::Value::kNumber && e.num == val.num) ||
                         (e.type == json::Value::kString && e.str == val.str) ||
                         (e.type == json::Value::kBool && e.b == val.b)))
                        hit = true;
                }
                if (!hit) return fail(L"参数超出允许值：" + kv.first);
            }
        }

        // minimum / maximum（数字）
        if (val.type == json::Value::kNumber) {
            if (const json::Value* mn = spec->Find(L"minimum"))
                if (mn->type == json::Value::kNumber && val.num < mn->num)
                    return fail(L"参数小于最小值：" + kv.first);
            if (const json::Value* mx = spec->Find(L"maximum"))
                if (mx->type == json::Value::kNumber && val.num > mx->num)
                    return fail(L"参数大于最大值：" + kv.first);
        }
    }
    return true;
}

}  // namespace

// 公开入口：转发到匿名命名空间里的实现。
// 这样做（而不是把实现搬到外面）是为了保持内部实现不被外部直接依赖，
// 同时给单测/UI 预校验一个稳定的调用点。
bool ValidateAgainstSchema(const json::Value& schema, const json::Value& args,
                           std::wstring* err) {
    return ValidateSchemaImpl(schema, args, err);
}

std::vector<ToolSpec> ListTools() {
    std::vector<ToolSpec> out;
    for (const auto& d : CommandTable()) {
        if (!d.aiExposed || !d.implemented) continue;
        ToolSpec t;
        // 主名 = 点分名（规则 C4：第一段是 group 的小写域前缀）。
        // d.name 现已全部是点分名，所以 CanonicalName 在这里是恒等 —— 保留是因为它是
        // "给任意名字还规范名"的公共语义（数据驱动处会用到），不是为了兼容过渡期
        // （过渡期已结束，见 docs/NAMING.md §7）。
        t.name = cmdnaming::CanonicalName(d.name);
        // 旧名从别名表反查（不是 d.name）—— d.name 迁到点分名后，直接取 d.name
        // 会让 name 与 legacyName 变成同一个值，client 的"旧名对照"就失效了。
        t.legacyName = cmdnaming::LegacyOf(t.name);
        t.title = d.title;
        t.group = d.group;
        t.argsSchema = Utf8ToWide(d.argsSchema);
        t.outputSchema = d.outputSchema ? Utf8ToWide(d.outputSchema) : std::wstring();
        t.effects = d.effects;
        t.needsSelection = d.needsSelection;
        // 自断言必须在这里填上：CallImpl 靠 spec.assertion 判断"要不要先采一份
        // 执行前的状态"（$changed / $unchanged 需要它）。不填则 before 永远为空，
        // 那两种断言被判 malformed —— 断言语言的一半是死的。
        t.assertion = d.assertion ? Utf8ToWide(d.assertion) : std::wstring();
        t.assertionExempt = d.assertionExempt ? Utf8ToWide(d.assertionExempt) : std::wstring();
        out.push_back(std::move(t));
    }
    return out;
}

bool FindTool(const std::wstring& name, ToolSpec* out) {
    // 按调用方给的任意形式（点分名或旧名）解析成 CommandDef ——
    // FindCommand 两个方向都认，这里直接复用它，避免第二套匹配逻辑。
    const CommandDef* def = FindCommand(name);
    if (!def) return false;
    for (const auto& d : CommandTable()) {
        if (d.id != def->id) continue;
        if (!d.aiExposed || !d.implemented) return false;
        if (out) {
            out->name = cmdnaming::CanonicalName(d.name);
            out->legacyName = cmdnaming::LegacyOf(out->name);
            out->title = d.title;
            out->group = d.group;
            out->argsSchema = Utf8ToWide(d.argsSchema);
            out->outputSchema = d.outputSchema ? Utf8ToWide(d.outputSchema) : std::wstring();
            out->effects = d.effects;
            out->needsSelection = d.needsSelection;
            out->assertion = d.assertion ? Utf8ToWide(d.assertion) : std::wstring();
            out->assertionExempt =
                d.assertionExempt ? Utf8ToWide(d.assertionExempt) : std::wstring();
        }
        return true;
    }
    return false;
}

// 真正的执行体。Call() 在它外面统一发事件 —— 这样"未知命令/需批准/参数非法"
// 这些**没进 CommandBus** 的拒绝也会留下事件，客户端不用靠响应是否到达来猜。
CallResult CallImpl(const std::wstring& command, const json::Value& args, bool approve,
                     const std::wstring& traceId) {
    CallResult r;
    r.command = command;

    // 审计上下文（会话 / 追踪 / 审批）—— 语义见 audit_log.h 的 AiScope。
    // 出作用域自动恢复：命令会嵌套调用，中途 return 一次也不能让上下文留在
    // 错的状态上（下一条命令于是带上上一条的 sessionId）。
    auditlog::AiScope auditScope(traceId);

    // ---- 被拒绝的调用也要落审计 ----
    //
    // 下面每一条 early-return 都是"**没进执行**"的拒绝：未知命令 / 未暴露 /
    // 未实现 / 危险未允许 / 需批准 / schema 非法 / 参数不合法。
    // 这些过去一条审计都不写（实测：连续 4 次被拒 → audit.log 0 增长），
    // 于是"AI 动过什么"缺了一大段 —— 而被拒的调用恰恰最值得追
    // （反复撞同一个错 = 调用方用错了方法；反复撞 danger = 有人在试探边界）。
    //
    // 统一在这里登记一次，避免每个拒绝点各写一遍而漏掉某个分支。
    auto reject = [&](const std::wstring& reason, const std::wstring& errorCode) {
        CommandBus::Get().AuditRejected(command, reason, errorCode);
        return r;
    };

    ToolSpec spec;
    if (!FindTool(command, &spec)) {
        // 三种"调不到"必须分开：修法完全不同（新增命令 / 补 aiExposed / 写 handler）。
        // 文案保持原样（既有客户端与单测按它比对），分类放进 errorCode。
        const CommandDef* any = FindCommand(command);
        if (!any)
            r.errorCode = L"unknown_tool";
        else if (!any->implemented)
            r.errorCode = L"not_implemented";
        else
            r.errorCode = L"not_exposed";
        r.error = L"未知或未暴露给 AI 的命令";
        LogDbg(L"[ai] 拒绝未知命令：%s（%s）", command.c_str(), r.errorCode.c_str());
        return reject(r.errorCode, r.errorCode);
    }
    // 回给调用方的命令名统一成规范名：它可能传的是旧名（NavBack），
    // 但响应里给 nav.back，这样客户端只需认一种形式。
    r.command = spec.name;
    r.risk = RiskName(spec.effects);

    // Danger 必须同时满足两件事：人类在设置里允许（aiAllowDanger），
    // 且调用方显式带 approve=true（表示它已向人类确认过这一次操作）。
    // 少任何一个都拒绝 —— 权限不能靠调用方自觉。
    if (spec.effects.destructive) {
        if (!Config::Get().s().aiAllowDanger) {
            r.errorCode = L"danger_disabled";
            r.error = L"danger_disabled";
            LogDbg(L"[ai] 拒绝危险命令（设置未允许）：%s", command.c_str());
            auditlog::SetApproval(L"denied");   // 不是"没批"，是"根本不让批"
            return reject(r.errorCode, r.errorCode);
        }
        if (!approve) {
            r.errorCode = L"need_approval";
            r.error = L"need_approval";
            LogDbg(L"[ai] 需批准：%s", command.c_str());
            auditlog::SetApproval(L"required");  // 要批，但这次没批
            return reject(r.errorCode, r.errorCode);
        }
        // 带批准且设置允许 → granted。approvedBy 记 ai：批准是**调用方声明**的
        // （"我已向人类确认过"），不是本进程弹窗问出来的 —— 记成 human 是把事实说错。
        auditlog::SetApproval(L"granted", L"ai");
    }

    // 参数校验：按命令自己的 argsSchema 来，不硬编码任何命令知识。
    json::Value schema;
    std::wstring perr;
    if (!json::Parse(spec.argsSchema, schema, &perr) || !schema.IsObject()) {
        r.errorCode = L"schema_invalid";   // 命令表写错，是实现缺陷，不是调用方的错
        r.error = L"命令的 argsSchema 非法（实现缺陷）";
        LogDbg(L"[ai] %s 的 argsSchema 解析失败：%s", command.c_str(), perr.c_str());
        return reject(r.errorCode, r.errorCode);
    }
    std::wstring verr;
    if (!ValidateAgainstSchema(schema, args, &verr)) {
        r.errorCode = L"bad_args";         // 改请求即可，不该去改代码
        r.error = verr;
        LogDbg(L"[ai] 参数非法：%s（%s）", command.c_str(), verr.c_str());
        return reject(r.errorCode, r.errorCode);
    }

    // 唯一执行入口：参数对象原样交给 CommandBus，actor 固定 "ai"。
    // 关键变化：不再有"把对象压扁成一个字符串"这一步 —— 那是旧设计的产物，
    // 也是"多参数/可选参数/类型校验"全部做不到的根源。
    //
    // 执行**之前**先采一份状态快照（只在断言用到 $changed/$unchanged 时采）：
    // 那两种断言要回答的是"和刚才比变没变"，没有"刚才"就没法比。
    // 采一次要走一遍 shell 选中枚举，所以只在真的需要时才付这个代价。
    json::Value beforeState;
    bool haveBefore = false;
    if (!spec.assertion.empty()) {
        // spec.assertion 已经是宽字符串（ToolSpec::assertion 是 std::wstring），
        // 不能再走 FromUtf8 —— 那是给 CommandDef::assertion（const char*）用的。
        const std::wstring expr = spec.assertion;
        if (expr.find(L"$changed") != std::wstring::npos ||
            expr.find(L"$unchanged") != std::wstring::npos) {
            std::wstring jerr;
            const std::wstring beforeText = StateToJson(GetState());
            haveBefore = json::Parse(beforeText, beforeState, &jerr);
        }
    }

    json::Value out;
    r.ok = CommandBus::Get().InvokeByNameResult(command, Args::FromJson(args), &out, Actor::Ai());
    // 只有命令声明了 outputSchema 才把结果当成"有内容"，
    // 否则 hasResult=false（区别于"有结果但恰好是空对象"）。
    if (!spec.outputSchema.empty()) {
        r.result = std::move(out);
        r.hasResult = true;
    }
    // 失败原因：优先用命令总线给出的**前置不满足**原因（"已经是顶层"这类
    // 用户能懂的话），而不是笼统的"命令执行失败"。以前总线算出了 reason
    // 却只写日志，外部只能看到"执行失败"，等于让调用方去猜。
    if (!r.ok && r.error.empty()) {
        const std::wstring& blocked = CommandBus::Get().lastBlockReason();
        // 前置条件不满足（"已经是顶层"这类）与真正的执行失败要分开：
        // 前者调用方改目标就能过，后者是 bug。以前两者都是一句中文，靠人读。
        if (blocked.empty()) {
            r.errorCode = L"failed";
            r.error = L"命令执行失败";
        } else {
            // 被拦住不再一锅端成 "blocked"：四类阻断的**下一步完全不同**
            // （换目标 / 稍后重试 / 问人 / 换一条路），调用方要能据此决策。
            // 类别由命令调用点给出（CommandBus::NoteFailure 的 kind 参数），
            // 这里不匹配中文字符串 —— 文案一改就断，那是文案该干的事。
            const wchar_t* code = FailKindCode(CommandBus::Get().lastFailKind());
            r.errorCode = (code && *code) ? code : L"precondition_unmet";
            r.error = blocked;
        }
    }

    // ---- 自断言求值 ----
    //
    // 只在命令成功时求值：失败时状态未必变，断言失败只会盖住真正的原因
    // （"op 失败"比"op 失败且 sortMode 不对"更该被看到）。
    //
    // 状态取执行**之后**的快照 —— 断言要回答的是"做完之后符合预期吗"。
    // 而 $changed / $unchanged 还要**执行之前**的那一份：没有它就比不了。
    // 执行前的快照**只在断言用到时**才采集 —— 采一次要走一遍 shell 选中枚举，
    // 每条命令都多采一遍纯属浪费。
    if (r.ok) {
        if (const CommandDef* def = FindCommand(spec.name)) {
            if (def->assertion && *def->assertion) {
                const std::wstring expr = str::FromUtf8(def->assertion);
                json::Value state;
                std::wstring jerr;
                const std::wstring stateText = StateToJson(GetState());
                if (json::Parse(stateText, state, &jerr)) {
                    auto one = assert_check::Check(expr, args, state,
                                                  haveBefore ? &beforeState : nullptr);
                    if (one.malformed) {
                        // 断言写错是实现缺陷：进审计，但不改 ok ——
                        // 网络对面看到"命令成功但断言非法"，该去修命令表。
                        LogDbg(L"[ai] %s 的断言非法：%s（%s）", command.c_str(),
                               one.expr.c_str(), one.error.c_str());
                    } else if (!one.ok) {
                        LogDbg(L"[ai] %s 断言不符：%s 期望 %s 实际 %s", command.c_str(),
                               one.expr.c_str(), one.expect.c_str(), one.actual.c_str());
                    }
                    r.assertions = assert_check::ToJson({one});
                    r.hasAssertions = true;
                }
            }
        }
    }

    // 总判定：有断言时必须是"handler 成功 && 断言全过"；无断言时就是 ok。
    // 集中算一次，事件与响应用同一份，避免两处各算一遍而口径漂移。
    r.verdict = r.ok;
    if (r.hasAssertions) {
        for (const auto& a : r.assertions.arr)
            if (!a.GetBool(L"ok", true)) { r.verdict = false; break; }
    }
    // 命令说成了、效果没对：这是独立一类（实现错，或断言判据错），
    // 机器要能把它与"handler 直接失败"区分开，否则只能靠人读中文。
    if (r.ok && !r.verdict && r.errorCode.empty()) r.errorCode = L"effect_not_met";
    return r;
}

// 事件发布必须在**返回值**之前完成：订阅者拿到 command 事件后再去读 state，
// 才能看到这次操作之后的状态。若放到响应写出之后，竞态会让客户端读到旧状态。
CallResult Call(const std::wstring& command, const json::Value& args, bool approve,
                const std::wstring& traceId) {
    CallResult r = CallImpl(command, args, approve, traceId);
    json::Value data = json::Value::MakeObject();
    data.Set(L"command", json::Value::MakeStr(r.command));
    data.Set(L"actor", json::Value::MakeStr(Actor::Ai().source));
    data.Set(L"args", args);   // 事件里带参数：事后回看才知道"改成了什么"
    data.Set(L"ok", json::Value::MakeBool(r.ok));
    if (!r.error.empty()) data.Set(L"error", json::Value::MakeStr(r.error));
    if (!r.errorCode.empty()) data.Set(L"errorCode", json::Value::MakeStr(r.errorCode));
    if (r.hasAssertions) data.Set(L"assertions", r.assertions);
    data.Set(L"verdict", json::Value::MakeBool(r.verdict));
    events::Publish(L"command", data);
    return r;
}

void SetStateProvider(StateFn fn) { g_state = std::move(fn); }

StateSnapshot GetState() {
    if (g_state) return g_state();
    return StateSnapshot{};   // 未注册时给空快照，不编造数据
}

}  // namespace ai
