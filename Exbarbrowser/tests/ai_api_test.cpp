// AI 接口自测：工具投影 / 参数校验 / 危险命令门 / 未暴露命令拒绝。
//
// 这些断言覆盖的是"AI 能做什么"的边界 —— 比单个命令是否正确更重要：
// 一旦这里放宽，AI 就能执行本不该暴露的操作，且不会有任何报错提示。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/ai_api_test.cpp src/ai_api.cpp src/commands.cpp src/json.cpp \
//     src/config.cpp src/text_file.cpp \
//     -o /tmp/ai_api_test.exe -lole32 -luuid -lshell32 -ladvapi32
#include <cstdio>
#include <string>
#include <vector>

#include "ai_api.h"
#include "commands.h"
#include "str.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool HasTool(const std::vector<ai::ToolSpec>& tools, const wchar_t* name) {
    for (const auto& t : tools)
        if (_wcsicmp(t.name.c_str(), name) == 0) return true;
    return false;
}

// 参数校验的唯一入口是 ai::ValidateAgainstSchema（不是早已删除的 EncodeArgs）。
//
// 历史：第 1 步把 handler 的入参从"单个字符串"改成 Args 对象后，编码层
// （ai::EncodeArgs）整个删掉了 —— 参数不再需要压成一个字符串。这个测试却一直
// 还在调它，于是**从未编译通过**（文档里"34 项断言"其实是 0 项实际执行）。
// 现在改成直接校验 schema，断言的含义不变：合法参数通过、缺必填/多余/类型错都拒绝。
bool ArgsOk(const wchar_t* cmd, const wchar_t* jsonText) {
    const CommandDef* d = FindCommand(cmd);
    if (!d) return false;
    json::Value schema, args;
    std::wstring err;
    // 命令表里的 schema 是 UTF-8 的 const char*；用与生产同一份转换实现
    // （str::FromUtf8），不要在测试里另写一个 MultiByteToWideChar 样板。
    if (!json::Parse(str::FromUtf8(d->argsSchema), schema, &err)) return false;
    if (!json::Parse(jsonText, args, &err)) return false;
    return ai::ValidateAgainstSchema(schema, args, &err);
}

}  // namespace

int main() {
    // ---- 1. 工具清单的基本性质 ----
    std::printf("工具清单\n");
    auto tools = ai::ListTools();
    Check(!tools.empty(), "至少暴露一个工具");

    bool allHaveSchema = true, allImplemented = true;
    for (const auto& t : tools) {
        if (t.argsSchema.empty()) allHaveSchema = false;
        const CommandDef* d = FindCommand(t.name);
        if (!d || !d->implemented) allImplemented = false;
    }
    Check(allHaveSchema, "每个工具都有 argsSchema（R18）");
    Check(allImplemented, "每个工具都对应已实现的命令");

    // 未实现命令绝不能出现在工具列表里
    const CommandDef* pick = FindCommand(L"ToggleCheckboxes");
    Check(pick && !pick->implemented, "ToggleCheckboxes 仍是未实现");
    Check(!HasTool(tools, L"ToggleCheckboxes"), "未实现命令不在工具清单");

    // 依赖选中项的破坏性命令不暴露（AI 无法用参数指定目标）
    Check(!HasTool(tools, L"CmdDelete"), "CmdDelete 不暴露给 AI");
    Check(!HasTool(tools, L"CmdRename"), "CmdRename 不暴露给 AI");
    Check(!HasTool(tools, L"CmdPaste"), "CmdPaste 不暴露给 AI");

    // 弹系统对话框的不暴露
    Check(!HasTool(tools, L"FolderOptions"), "FolderOptions 不暴露给 AI");
    Check(!HasTool(tools, L"Preferences"), "Preferences 不暴露给 AI");

    // 纯导航/视图类应暴露。
    // 用点分名查：ToolSpec::name 现已是规范名（点分），驼峰名降为 legacyName。
    // HasTool 只比 name —— 早先这里用驼峰名查，结果 3 项一直失败，
    // 而整个文件又编译不过，所以没人发现。
    Check(HasTool(tools, L"nav.back"), "nav.back 暴露");
    Check(HasTool(tools, L"view.mode"), "view.mode 暴露");
    Check(HasTool(tools, L"tab.activate"), "tab.activate 暴露");

    // 顺带守住 legacyName：旧名必须仍能从工具里查到（既有脚本靠它对照）
    bool legacyKept = false;
    for (const auto& t : tools)
        if (t.legacyName == L"NavBack") legacyKept = true;
    Check(legacyKept, "nav.back 的 legacyName 仍是 NavBack");

    // ---- 2. 人机等价（R20）：每个暴露的命令都必须有 title（= 人能看懂、能找到）----
    std::printf("\n人机等价（R20）\n");
    bool allTitled = true;
    for (const auto& t : tools)
        if (t.title.empty()) allTitled = false;
    Check(allTitled, "每个工具都有中文 title（人工可达路径的入口名）");

    // ---- 3. 参数校验 ----
    std::printf("\n参数校验\n");
    Check(ArgsOk(L"ViewMode", L"{\"preset\":3}"), "ViewMode preset=3 通过");
    Check(ArgsOk(L"ViewSort", L"{\"by\":1}"), "ViewSort by=1 通过");
    Check(ArgsOk(L"TabActivate", L"{\"index\":2}"), "TabActivate index=2 通过");
    Check(ArgsOk(L"nav.back", L"{}"), "无参数命令 + 空对象 通过（点分名同样认）");

    // 必填参数缺失必须拒绝（不兜底）
    Check(!ArgsOk(L"ViewMode", L"{}"), "ViewMode 缺 preset -> 拒绝");
    Check(!ArgsOk(L"TabActivate", L"{}"), "TabActivate 缺 index -> 拒绝");

    // 无参数命令传了参数：显式拒绝，不静默忽略
    Check(!ArgsOk(L"nav.back", L"{\"oops\":1}"), "无参数命令传参数 -> 拒绝");

    // 类型不支持
    Check(!ArgsOk(L"ViewMode", L"{\"preset\":[1,2]}"), "数组参数 -> 拒绝");

    // ---- 4. 未暴露命令一律拒绝（fail-closed）----
    std::printf("\n未暴露命令\n");
    json::Value empty = json::Value::MakeObject();
    ai::CallResult r1 = ai::Call(L"CmdDelete", empty, true);
    Check(!r1.ok, "CmdDelete 即使 approve=true 也拒绝");
    Check(r1.error == L"未知或未暴露给 AI 的命令", "给出明确原因");
    // 中文原因之外还必须有机器码：客户端据此决定"补命令 / 补暴露 / 改参数"。
    Check(r1.errorCode == L"not_exposed", "CmdDelete -> errorCode=not_exposed");

    ai::CallResult r2 = ai::Call(L"NoSuchCommand", empty, false);
    Check(!r2.ok, "未知命令拒绝");
    Check(r2.errorCode == L"unknown_tool", "NoSuchCommand -> errorCode=unknown_tool");

    ai::CallResult r4 = ai::Call(L"ToggleCheckboxes", empty, false);
    Check(!r4.ok, "未实现命令拒绝");
    Check(r4.errorCode == L"not_implemented", "ToggleCheckboxes -> errorCode=not_implemented");

    // ---- 5. 危险命令门：设置未允许时，approve=true 也拒绝 ----
    std::printf("\n危险命令门\n");
    // 测试进程里 Config 默认 aiAllowDanger=false
    ai::CallResult r3 = ai::Call(L"TabCloseOther", empty, true);
    Check(!r3.ok, "TabCloseOther(Danger) 在 aiAllowDanger=false 时拒绝");
    Check(r3.error == L"danger_disabled", "错误码是 danger_disabled");
    Check(r3.errorCode == L"danger_disabled", "errorCode 同为 danger_disabled");

    // 参数不合规：这是"改请求"，不是"补代码"—— 分类必须能给对。
    json::Value badArgs = json::Value::MakeObject();
    ai::CallResult r5 = ai::Call(L"ViewMode", badArgs, false);
    Check(!r5.ok, "ViewMode 缺 preset 拒绝");
    Check(r5.errorCode == L"bad_args", "缺必填参数 -> errorCode=bad_args");

    // 关掉的门对 Write 类不影响（这里只验证不会被误判成 danger）
    // risk 字段在第 2 步被 Effects 四维取代（单轴表达不了"幂等的写" vs
    // "不可逆的写"）。这里改判 Effects，顺带守住"导航不是破坏性操作"。
    const CommandDef* navDef = FindCommand(L"nav.back");
    Check(navDef && !navDef->effects.destructive, "nav.back 不是破坏性操作");
    Check(navDef && !navDef->effects.readOnly, "nav.back 不是只读（它会改导航状态）");
    Check(navDef && navDef->aiExposed, "nav.back 已暴露");

    // ---- 5b. dev.build / dev.test 必须走危险双门 ----
    //
    // 【为什么单独立这一条】参数面收窄（target 枚举、filter 白名单）只挡住了
    // "往命令行里注入外部命令"，挡不住"仓库本身被改成会干坏事的程序" ——
    // 编译与测试都会真的执行仓库里的代码（测试二进制、build_fast.py 的 Python）。
    // 所以它们的判据不是"参数合法"，而是"人有没有批准在这一轮执行代码"。
    // 一旦有人把它改回 destructive=0，这条测试就是唯一会响的地方。
    std::printf("\n开发面双门\n");
    {
        const CommandDef* build = FindCommand(L"dev.build");
        const CommandDef* test = FindCommand(L"dev.test");
        Check(build && build->effects.destructive, "dev.build 声明 destructive（会执行仓库代码）");
        Check(test && test->effects.destructive, "dev.test 声明 destructive（会执行仓库代码）");
        Check(build && build->effects.openWorld, "dev.build 声明 openWorld（起外部进程）");

        // 设置未允许时，即使 approve=true 也必须拒绝（与 TabCloseOther 同一条门）。
        ai::CallResult rb = ai::Call(L"dev.build", empty, true);
        Check(!rb.ok, "dev.build 在 aiAllowDanger=false 时拒绝（即使 approve=true）");
        Check(rb.errorCode == L"danger_disabled", "dev.build -> errorCode=danger_disabled");

        ai::CallResult rt = ai::Call(L"dev.test", empty, true);
        Check(!rt.ok, "dev.test 在 aiAllowDanger=false 时拒绝（即使 approve=true）");
        Check(rt.errorCode == L"danger_disabled", "dev.test -> errorCode=danger_disabled");

        // 只读的兄弟不该被连带误判：dev.snapshot / dev.job 不执行代码。
        const CommandDef* snap = FindCommand(L"dev.snapshot");
        const CommandDef* job = FindCommand(L"dev.job");
        Check(snap && !snap->effects.destructive, "dev.snapshot 不是破坏性（只读工作树）");
        Check(job && !job->effects.destructive, "dev.job 不是破坏性（只查作业结果）");
    }

    // ---- 6. JSON 输出形状 ----
    std::printf("\nJSON 输出\n");
    std::wstring toolsJson = ai::ToolsToJson();
    Check(toolsJson.find(L"\"tools\"") != std::wstring::npos, "ToolsToJson 含 tools 键");
    Check(toolsJson.find(L"inputSchema") != std::wstring::npos,
          "含 inputSchema（MCP 要求是对象）");
    // inputSchema 必须是对象而不是字符串
    Check(toolsJson.find(L"\"inputSchema\":{") != std::wstring::npos,
          "inputSchema 序列化成对象而非字符串");

    ai::CallResult rr;
    rr.command = L"NavBack";
    rr.risk = L"write";
    rr.error = L"未知或未暴露给 AI 的命令";
    rr.errorCode = L"unknown_tool";
    std::wstring rj = ai::ResultToJson(rr);
    Check(rj.find(L"\"ok\":false") != std::wstring::npos, "CallResult ok=false 正确序列化");
    Check(rj.find(L"\"verdict\":false") != std::wstring::npos, "响应含顶层 verdict（无断言时=ok）");
    Check(rj.find(L"\"errorCode\":\"unknown_tool\"") != std::wstring::npos,
          "响应含 errorCode（中文 error 不变）");

    // ---- 7. 能力面自描述：让客户端不靠猜就能分清三类缺口 ----
    std::printf("\n能力面\n");
    std::wstring cap = ai::CapabilitiesToJson();
    int total = 0, impl = 0, exp = 0;
    for (const auto& d : CommandTable()) {
        ++total;
        if (!d.implemented) continue;
        ++impl;
        if (d.aiExposed) ++exp;
    }
    Check(cap.find(L"\"total\":" + std::to_wstring(total)) != std::wstring::npos,
          "capabilities.total 与命令表条数一致");
    Check(cap.find(L"\"implemented\":" + std::to_wstring(impl)) != std::wstring::npos,
          "capabilities.implemented 与命令表一致");
    Check(cap.find(L"\"aiExposed\":" + std::to_wstring(exp)) != std::wstring::npos,
          "capabilities.aiExposed 与命令表一致");
    Check(exp == static_cast<int>(tools.size()),
          "capabilities.aiExposed 与 tools/list 条数一致（两个面不能各说各话）");
    Check(cap.find(L"\"notImplemented\":[") != std::wstring::npos &&
          cap.find(L"\"notExposed\":[") != std::wstring::npos,
          "给出 notImplemented / notExposed 两张缺口清单");

    // 清单内容要真的能对上：未实现的非空（当前确实有未实现命令），
    // CmdDelete 属于"已实现但未开放"而不是"未实现"—— 这三类一旦混在一起，
    // 客户端就会去写一个已经存在的命令。
    json::Value capv;
    std::wstring capErr;
    Check(json::Parse(cap, capv, &capErr), "capabilities 是合法 JSON");
    const json::Value* ni = capv.Find(L"notImplemented");
    Check(ni && ni->IsArray() && !ni->arr.empty(), "notImplemented 非空（确实还有未实现命令）");
    if (ni && ni->IsArray()) {
        bool toggleInNotImpl = false;
        for (const auto& n : ni->arr)
            if (n.str == L"view.toggle.checkboxes") toggleInNotImpl = true;
        Check(toggleInNotImpl, "view.toggle.checkboxes 在 notImplemented（表里 implemented=false）");
    }
    const json::Value* ne = capv.Find(L"notExposed");
    bool hasCmdDelete = false;
    if (ne && ne->IsArray())
        for (const auto& n : ne->arr)
            // 规范名是点分名（file.delete），CmdDelete 只是 legacy 名。
            if (n.str == L"file.delete") hasCmdDelete = true;
    Check(hasCmdDelete, "file.delete（CmdDelete）出现在 notExposed（已实现但未开放）");

    ai::CallResult okNoAssert;
    okNoAssert.command = L"NavBack";
    okNoAssert.risk = L"write";
    okNoAssert.ok = true;
    okNoAssert.verdict = true;           // 无断言 → verdict=ok
    std::wstring okJson = ai::ResultToJson(okNoAssert);
    Check(okJson.find(L"\"verdict\":true") != std::wstring::npos,
          "无断言命令成功 → verdict=true");

    // 断言失败不改 ok，但 verdict=false：审计真相与效果判定分开。
    ai::CallResult badEffect;
    badEffect.command = L"ViewSort";
    badEffect.risk = L"write";
    badEffect.ok = true;
    badEffect.verdict = false;
    badEffect.hasAssertions = true;
    badEffect.assertions = json::Value::MakeArray();
    json::Value a = json::Value::MakeObject();
    a.Set(L"ok", json::Value::MakeBool(false));
    badEffect.assertions.arr.push_back(std::move(a));
    std::wstring badJson = ai::ResultToJson(badEffect);
    Check(badJson.find(L"\"ok\":true") != std::wstring::npos,
          "断言失败时 ok 仍是 handler 原值 true（审计不失真）");
    Check(badJson.find(L"\"verdict\":false") != std::wstring::npos,
          "断言失败 → verdict=false（调用方一眼知效果不对）");

    std::printf("\n撤销组元数据\n");
    Check(ai::IsValidUndoGroupId(L"turn-12"), "turn-12 是合法组 ID");
    Check(ai::IsValidUndoGroupId(L"a.b_c:d-1"), "分隔符合法");
    Check(!ai::IsValidUndoGroupId(L""), "空组 ID 拒绝");
    Check(!ai::IsValidUndoGroupId(L"-x"), "连字符开头拒绝");
    Check(!ai::IsValidUndoGroupId(std::wstring(65, L'x')), "超长组 ID 拒绝");
    Check(std::wstring(ai::UndoGroupPolicyName(ai::UndoGroupPolicy::Join)) == L"join",
          "join 是稳定协议取值");
    {
        const ai::ToolSpec* fsMove = nullptr;
        const ai::ToolSpec* bmkRemove = nullptr;
        const ai::ToolSpec* navBack = nullptr;
        const ai::ToolSpec* tabClose = nullptr;
        for (const auto& t : tools) {
            if (t.name == L"fs.move") fsMove = &t;
            else if (t.name == L"bmk.remove") bmkRemove = &t;
            else if (t.name == L"nav.back") navBack = &t;
            else if (t.name == L"tab.close") tabClose = &t;
        }
        Check(fsMove && fsMove->undoGroup == ai::UndoGroupPolicy::Join
              && fsMove->undoDomain == L"file", "fs.move 声明 file 撤销组");
        Check(bmkRemove && bmkRemove->undoGroup == ai::UndoGroupPolicy::Join
              && bmkRemove->undoDomain == L"bookmark", "bmk.remove 声明 bookmark 撤销组");
        Check(navBack && navBack->undoGroup == ai::UndoGroupPolicy::None
              && navBack->undoDomain.empty(), "只读导航命令不进撤销组");
        Check(tabClose && tabClose->undoGroup == ai::UndoGroupPolicy::None
              && tabClose->undoDomain.empty(), "tab.close 虽 undoable 但没有同域栈，不承诺 join");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
