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
#include "str.h"

#include "cmd_naming.h"

#include "common.h"   // LogDbg
#include "config.h"
#include "text_file.h"

namespace ai {
namespace {

StateFn g_state;

const wchar_t* RiskName(const Effects& e) {
    if (e.destructive) return L"danger";
    return e.readOnly ? L"read" : L"write";
}

const wchar_t* ScopeName(Scope s) {
    switch (s) {
    case Scope::CurrentTab: return L"currentTab";
    case Scope::App:        return L"app";
    case Scope::System:     return L"system";
    }
    return L"currentTab";
}

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
        }
        return true;
    }
    return false;
}

// 真正的执行体。Call() 在它外面统一发事件 —— 这样"未知命令/需批准/参数非法"
// 这些**没进 CommandBus** 的拒绝也会留下事件，客户端不用靠响应是否到达来猜。
CallResult CallImpl(const std::wstring& command, const json::Value& args, bool approve) {
    CallResult r;
    r.command = command;

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
        return r;
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
            return r;
        }
        if (!approve) {
            r.errorCode = L"need_approval";
            r.error = L"need_approval";
            LogDbg(L"[ai] 需批准：%s", command.c_str());
            return r;
        }
    }

    // 参数校验：按命令自己的 argsSchema 来，不硬编码任何命令知识。
    json::Value schema;
    std::wstring perr;
    if (!json::Parse(spec.argsSchema, schema, &perr) || !schema.IsObject()) {
        r.errorCode = L"schema_invalid";   // 命令表写错，是实现缺陷，不是调用方的错
        r.error = L"命令的 argsSchema 非法（实现缺陷）";
        LogDbg(L"[ai] %s 的 argsSchema 解析失败：%s", command.c_str(), perr.c_str());
        return r;
    }
    std::wstring verr;
    if (!ValidateAgainstSchema(schema, args, &verr)) {
        r.errorCode = L"bad_args";         // 改请求即可，不该去改代码
        r.error = verr;
        LogDbg(L"[ai] 参数非法：%s（%s）", command.c_str(), verr.c_str());
        return r;
    }

    // 唯一执行入口：参数对象原样交给 CommandBus，actor 固定 "ai"。
    // 关键变化：不再有"把对象压扁成一个字符串"这一步 —— 那是旧设计的产物，
    // 也是"多参数/可选参数/类型校验"全部做不到的根源。
    json::Value out;
    r.ok = CommandBus::Get().InvokeByNameResult(command, Args::FromJson(args), &out, L"ai");
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
            r.errorCode = L"blocked";
            r.error = blocked;
        }
    }

    // ---- 自断言求值 ----
    //
    // 只在命令成功时求值：失败时状态未必变，断言失败只会盖住真正的原因
    // （"op 失败"比"op 失败且 sortMode 不对"更该被看到）。
    //
    // 状态取执行**之后**的快照 —— 断言要回答的是"做完之后符合预期吗"。
    if (r.ok) {
        if (const CommandDef* def = FindCommand(spec.name)) {
            if (def->assertion && *def->assertion) {
                const std::wstring expr = str::FromUtf8(def->assertion);
                json::Value state;
                const std::wstring stateText = StateToJson(GetState());
                std::wstring jerr;
                if (json::Parse(stateText, state, &jerr)) {
                    auto one = assert_check::Check(expr, args, state);
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
CallResult Call(const std::wstring& command, const json::Value& args, bool approve) {
    CallResult r = CallImpl(command, args, approve);
    json::Value data = json::Value::MakeObject();
    data.Set(L"command", json::Value::MakeStr(r.command));
    data.Set(L"actor", json::Value::MakeStr(L"ai"));
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

// ---------------------------------------------------------------- JSON 输出

namespace {

json::Value Str(const std::wstring& s) { return json::Value::MakeStr(s); }

}  // namespace

std::wstring ResultToJson(const CallResult& r) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"ok", json::Value::MakeBool(r.ok));
    v.Set(L"command", Str(r.command));
    v.Set(L"risk", Str(r.risk));
    if (!r.error.empty()) v.Set(L"error", Str(r.error));
    // 机器码与中文原因并存：老客户端读 error 不变，新客户端只读 errorCode。
    if (!r.errorCode.empty()) v.Set(L"errorCode", Str(r.errorCode));

    // 结构化结果（P6）。MCP 的形状是 structuredContent + 一份序列化文本
    // （兼容只认文本的客户端）。这里同时给两份。
    if (r.hasResult) {
        v.Set(L"structuredContent", r.result);
        v.Set(L"content", Str(json::Stringify(r.result, 0)));
    }
    // 自断言：与 structuredContent 同级给出，便于客户端只看一处就知道
    // "命令说成了" 与 "效果确实对" 是否一致。
    if (r.hasAssertions) v.Set(L"assertions", r.assertions);
    // 总判定：调用方（含 ebbctl / 意图层）只读这一个字段就能回答"成没成"。
    v.Set(L"verdict", json::Value::MakeBool(r.verdict));
    return json::Stringify(v, 0);
}

std::wstring StateToJson(const StateSnapshot& s) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"activePath", Str(s.activePath));
    v.Set(L"activeTab", json::Value::MakeInt(s.activeTab));

    json::Value tabs = json::Value::MakeArray();
    for (const auto& p : s.tabPaths) tabs.arr.push_back(Str(p));
    v.Set(L"tabs", tabs);

    // itemCount / viewMode 用 number。unsigned 转 int 在这里安全（项目数不会溢出）。
    v.Set(L"itemCount", json::Value::MakeInt(static_cast<int>(s.itemCount)));
    v.Set(L"viewMode", json::Value::MakeInt(s.viewMode));

    // ---- 可写状态的回读（见 StateSnapshot 的注释：能改就必须能读）----
    // 这几个字段各自对应一条命令，只有写没有读时调用方无法确认改没改成。
    v.Set(L"sortMode", json::Value::MakeInt(s.sortMode));
    v.Set(L"groupBy", json::Value::MakeInt(s.groupBy));
    v.Set(L"bookmarkBarVisible", json::Value::MakeBool(s.bookmarkBarVisible));
    v.Set(L"navPaneVisible", json::Value::MakeBool(s.navPaneVisible));
    v.Set(L"previewPaneVisible", json::Value::MakeBool(s.previewPaneVisible));

    // ---- 搜索态（search.find 的回读面）----
    v.Set(L"searching", json::Value::MakeBool(s.searching));
    v.Set(L"searchScope", Str(s.searchScope));
    v.Set(L"searchQuery", Str(s.searchQuery));

    // ---- 选中与焦点（观测面）：AI 回答"用户选中了什么/焦点在哪"的唯一来源 ----
    v.Set(L"selectionCount", json::Value::MakeInt(static_cast<int>(s.selectionCount)));
    json::Value sel = json::Value::MakeArray();
    json::Value folders = json::Value::MakeArray();
    for (size_t i = 0; i < s.selectionPaths.size(); ++i) {
        sel.arr.push_back(Str(s.selectionPaths[i]));
        bool isFolder = i < s.selectionFolders.size() && s.selectionFolders[i];
        folders.arr.push_back(json::Value::MakeBool(isFolder));
    }
    v.Set(L"selection", sel);
    v.Set(L"selectionFolders", folders);
    v.Set(L"selectionTruncated", json::Value::MakeBool(s.selectionTruncated));
    v.Set(L"focus", Str(s.focus));
    return json::Stringify(v, 0);
}

namespace {

// 给工具生成 description（MCP 里模型据此判断何时该用/不该用）。
//
// 刻意从结构化字段拼出来而不是手写自由文本：手写的描述会与 effects 脱节
// （改了 destructive 却忘了改描述），而这两者对不上时模型会做错判断。
// 这里保证"描述里说的"与"annotations 里标的"永远一致。
std::wstring Describe(const ToolSpec& t) {
    std::wstring d = t.title;

    // 这是做什么的：用 group 给一句领域说明，比"执行 X"更有信息量。
    if (t.group == L"Tab")            d += L"。标签页操作";
    else if (t.group == L"Go")        d += L"。跳转到已知位置";
    else if (t.group == L"Nav")       d += L"。目录导航";
    else if (t.group == L"Path")      d += L"。把路径写入剪贴板";
    else if (t.group == L"File")      d += L"。文件操作";
    else if (t.group == L"View")      d += L"。调整视图显示";
    else if (t.group == L"Window")    d += L"。窗口级操作";
    else if (t.group == L"External")  d += L"。启动外部程序";
    else if (t.group == L"Bookmark")  d += L"。书签栏操作";
    else if (t.group == L"Session")   d += L"。会话管理";
    else if (t.group == L"Tool")      d += L"。查询工具";
    else if (t.group == L"Search")    d += L"。搜索";

    // 副作用警告：这是模型最需要知道的部分。
    if (t.effects.destructive) d += L"。**可能不可逆**，调用前应确认";
    else if (t.effects.readOnly) d += L"。只读，不改变任何状态";
    if (t.effects.openWorld) d += L"。会与进程外交互";
    if (t.needsSelection) d += L"。作用于当前选中项";

    return d;
}

}  // namespace

std::wstring ToolsToJson() {    json::Value root = json::Value::MakeObject();
    json::Value arr = json::Value::MakeArray();
    for (const auto& t : ListTools()) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"name", Str(t.name));
        // 旧名作为兼容字段：既有脚本/审计日志用它对照，新客户端只用 name。
        o.Set(L"legacyName", Str(t.legacyName));
        o.Set(L"title", Str(t.title));
        o.Set(L"group", Str(t.group));
        o.Set(L"needsSelection", json::Value::MakeBool(t.needsSelection));

        // description 是 MCP 里模型"判断何时该用"的主要依据，不能省。
        // 从已有的结构化信息生成，而不是另写一份自由文本 —— 后者会与效果标注
        // 脱节（改了 destructive 却忘了改描述）。
        o.Set(L"description", Str(Describe(t)));

        // 效果按 MCP 的 annotations 惯例给出四个正交布尔 + 作用域。
        // 单给一个 risk 字符串会让调用方无法区分"幂等的写"与"不可逆的写"，
        // 从而只能一律弹确认，或者一律放行 —— 两者都不对。
        json::Value ann = json::Value::MakeObject();
        ann.Set(L"readOnlyHint", json::Value::MakeBool(t.effects.readOnly));
        ann.Set(L"destructiveHint", json::Value::MakeBool(t.effects.destructive));
        ann.Set(L"idempotentHint", json::Value::MakeBool(t.effects.idempotent));
        ann.Set(L"openWorldHint", json::Value::MakeBool(t.effects.openWorld));
        ann.Set(L"undoable", json::Value::MakeBool(t.effects.undoable));
        ann.Set(L"scope", Str(ScopeName(t.effects.scope)));
        o.Set(L"annotations", ann);

        // 兼容字段：粗粒度三档，老的调用方还能用。
        o.Set(L"risk", Str(RiskName(t.effects)));

        // inputSchema 要原样回给 MCP 客户端，所以这里必须解析回对象而不是塞字符串
        // （MCP 规定 inputSchema 是 JSON 对象；字符串会让客户端 schema 校验直接失败）。
        json::Value schema;
        std::wstring perr;
        if (json::Parse(t.argsSchema, schema, &perr) && schema.IsObject()) {
            o.Set(L"inputSchema", schema);
        } else {
            LogDbg(L"[ai] 命令 %s 的 argsSchema 不是合法 JSON：%s", t.name.c_str(), perr.c_str());
            o.Set(L"inputSchema", json::Value::MakeObject());
        }

        // outputSchema 同理；没有就整个字段不写（MCP：可选）。
        if (!t.outputSchema.empty()) {
            json::Value os;
            if (json::Parse(t.outputSchema, os, &perr) && os.IsObject())
                o.Set(L"outputSchema", os);
            else
                LogDbg(L"[ai] 命令 %s 的 outputSchema 不是合法 JSON：%s", t.name.c_str(), perr.c_str());
        }
        arr.arr.push_back(std::move(o));
    }
    root.Set(L"tools", arr);
    return json::Stringify(root, 0);
}

std::wstring CapabilitiesToJson() {
    // 直接从命令表数，不另存一份清单 —— 清单会与表脱节，而这张表是唯一真源。
    int total = 0, implemented = 0, exposed = 0;
    json::Value notImpl = json::Value::MakeArray();
    json::Value notExposed = json::Value::MakeArray();
    for (const auto& d : CommandTable()) {
        ++total;
        const std::wstring name = cmdnaming::CanonicalName(d.name);
        if (!d.implemented) {
            // 未实现也算"缺口"，但修法是写 handler，不是改 aiExposed。
            notImpl.arr.push_back(Str(name));
            continue;
        }
        ++implemented;
        if (!d.aiExposed) {
            // 已实现但没开放：修法是补元数据，不是写新命令。
            notExposed.arr.push_back(Str(name));
            continue;
        }
        ++exposed;
    }
    json::Value v = json::Value::MakeObject();
    v.Set(L"total", json::Value::MakeInt(total));
    v.Set(L"implemented", json::Value::MakeInt(implemented));
    v.Set(L"aiExposed", json::Value::MakeInt(exposed));
    v.Set(L"notImplemented", notImpl);
    v.Set(L"notExposed", notExposed);
    return json::Stringify(v, 0);
}

}  // namespace ai
