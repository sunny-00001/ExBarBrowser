// ai_api 的 JSON 序列化（从 ai_api.cpp 拆出来）。
//
// 【为什么拆】"执行一次命令"与"把结果写成 JSON"是**两件事**：
// 前者决定系统做什么，后者决定客户端看到什么。
// 混在一个文件里时，改协议（加字段、改形状）要在一个 660 行的文件里翻，
// 而改执行逻辑的人也得先读过那一大段序列化才敢动。
//
// 【边界】这里**只**做序列化：不判断权限、不碰状态、不产生副作用。
// 任何"顺手在这里加个判断"都会让"客户端看到的形状"与"系统实际做了什么"
// 悄悄分叉 —— 那正是最难查的一类 bug。
#include "ai_api.h"

#include "common.h"
#include "cmd_naming.h"
#include "json.h"

namespace ai {

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
    v.Set(L"sortKey", Str(s.sortKey));
    v.Set(L"groupKey", Str(s.groupKey));
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

    // ---- 标签组 / 书签 / 会话的可回读面 ----
    // 这几个字段是给自断言用的：断言的左值必须取自这里（check_ai_meta 第 18 项
    // 会核对"左值在 StateToJson 里"）。字段不存在 → 断言写不出来 → 覆盖率上不去。
    v.Set(L"tabCount", json::Value::MakeInt(s.tabCount));
    v.Set(L"activeTabTitle", Str(s.activeTabTitle));
    v.Set(L"activeTabIsLast", json::Value::MakeBool(s.activeTabIsLast));
    v.Set(L"bookmarkCount", json::Value::MakeInt(s.bookmarkCount));
    v.Set(L"bookmarkTotalCount", json::Value::MakeInt(s.bookmarkTotalCount));
    v.Set(L"sessionSaved", json::Value::MakeBool(s.sessionSaved));
    v.Set(L"namedSessionCount", json::Value::MakeInt(s.namedSessionCount));

    // ---- 标签改名 / shell 隐藏项设置的可回读面 ----
    v.Set(L"activeTabAlias", Str(s.activeTabAlias));
    v.Set(L"showHiddenFiles", json::Value::MakeBool(s.showHiddenFiles));
    v.Set(L"showSystemFiles", json::Value::MakeBool(s.showSystemFiles));
    v.Set(L"showExtensions", json::Value::MakeBool(s.showExtensions));
    v.Set(L"shellHiddenState", json::Value::MakeInt(s.shellHiddenState));
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

        // expectedEffect：这条命令的**预期效果**（自断言表达式）。
        //
        // 为什么必须给出去：调用方拿到 ok=true 时无法区分"真的做成了"和
        // "handler 自以为做成了"。把判据本身暴露出去，客户端就能自己对账，
        // 也能在失败时知道"预期是什么、实际是什么"（而不是只看到一个 false）。
        // 没有断言的命令不写这个字段 —— 空串会被误读成"预期效果为空"。
        if (!t.assertion.empty()) {
            o.Set(L"expectedEffect", Str(t.assertion));
        }
        // assertionExempt：这条命令**写不出**断言，理由是什么。
        //
        // 为什么必须给出去：expectedEffect 缺失时，调用方无从判断"是这条命令
        // 没有可验证的预期效果"还是"客户端版本旧、没实现这个字段"。给出理由
        // 后，调用方至少知道该自己补一次核对，而不是把 ok=true 当成"效果达成"。
        if (!t.assertionExempt.empty()) {
            o.Set(L"assertionExempt", Str(t.assertionExempt));
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
