// 命令目录与派发（唯一动作入口）
//
// 目的：键盘 / 序列 / 鼠标 / 菜单 / 命令面板全部经同一张表与同一条派发路径，
// 保证"改键位不改代码"、"人机同源"（同一命令既能被快捷键触发，也能被面板触发）。
//
// 命名与 ExBar mod 的 data/capabilities.json 对齐；本工程未实现的命令仍然保留在表里
// （implemented=false），这样 mod 的 keymap.json 可以原样读取，未实现项在管理器里
// 明确标灰，而不是"键位静默消失"。
#pragma once
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#include "json.h"

#include "commands_args.h"
#include "commands_actor.h"
#include "commands_ids.h"
#include "commands_state.h"

// Args / Risk / Effects / Actor / FailKind / CommandState 分别住在 commands_args.h、
// commands_actor.h、commands_state.h。本文件保留 facade：调用点继续 include 这里，
// 不因为内部拆头而改一行调用代码。

// 前置条件（Req / Requirements / ReqReason）单独成文件：
// 见 cmd_req.h 的说明 —— 它是一块自成一体的东西（枚举 + 容器 + 原因文案），
// 与"命令表里有哪些命令"是两件事。
#include "cmd_req.h"

struct CommandDef {
    CmdId id;
    const wchar_t* name;   // 与 capabilities.json 的 id 一致
    const wchar_t* group;  // 机器标识：Tab / Go / Nav / Path / File / View / Window / External / Bookmark / Session
    const wchar_t* scopeUi;  // 中文展示串（标签栏/导航栏/…）。**只用于展示，不用于判定**
    const wchar_t* title;  // 中文显示名
    bool implemented;
    // ---- 结构化元数据（架构规则 R18 + 业界 P1/P3）----
    Effects effects;
    // JSON Schema 字符串，直接复用为 MCP inputSchema / 自动生成 UI。
    // 无参数命令必须显式写 additionalProperties:false（空 schema 语义是"接受任意参数"）。
    const char* argsSchema;
    bool aiExposed = false;      // 是否进 tools/list
    bool needsSelection = false; // 依赖当前选中项（无选中时机器侧应拒绝而非猜测）

    // 前置条件（AND）。空 = 无条件。
    // 判定由调用方通过 CanExecute 完成 —— CommandDef 本身不持有窗口/状态引用，
    // 保持纯数据（可导出、可静态检查、可单测）。
    Requirements requires_;

    // 结构化结果 schema（JSON Schema 字符串）。nullptr = 该命令不产出结果。
    // MCP 规定：给了 outputSchema 就必须按它返回；客户端会校验。
    // 也为空时表示"这个命令只回报成败"，不编造返回值。
    const char* outputSchema = nullptr;

    // ---- 自断言（让"可验证"从脚本层提升到契约层）----
    //
    // 【为什么需要】`ok=true` 只说明"handler 返回了 true"，不说明"效果对吗"。
    // 典型事故：改了内存但没落盘、改了 A 却以为改了 B、被前置条件静默跳过
    // 后仍返回成功。这些都不会让 ok 变 false —— 调用方以为成了，实际没成，
    // 而且错误要到很久以后才暴露（那时现场已丢）。
    //
    // 【形态】声明式，不是回调。理由与 Requirements 相同：
    //   · 静态检查能核对"声明了就真能求值"，回调核不了；
    //   · 能导出成文档 / 生成测试用例骨架；
    //   · 不需要每个命令自己写一段 if，避免逻辑散落。
    //
    // 【怎么求值】形如 "sortMode=$arg:by"：
    //   左值取自 exbar/state 的字段名（**必须是可回读的**，见 check_ai_meta 第 17 项），
    //   右值是期望值；`$arg:<key>` 表示"等于入参 key 的值"，
    //   `$arg:<key>.length` 取数组/字符串长度。
    //   执行后由 assert_check::Check 求值，结果进 CallResult.assertions。
    //
    // 【为什么右值要能引用入参】断言"sortMode 等于我刚传的 by"才是真实预期；
    // 写死成 "sortMode=2" 会在换参数时悄悄失效 —— 那种断言比没有更危险，
    // 因为它会一直绿。
    const char* assertion = nullptr;

    // ---- 显式豁免（"这条命令为什么写不出断言"）----
    //
    // 【为什么需要这个字段】断言覆盖率不能靠"能给多少条就写多少条"来达成 ——
    // 剩下的该怎么办？只有两种诚实的做法：写一条**真能求值**的断言，
    // 或者**显式说明为什么写不出来**。没有第三种。
    //
    // 以前第三种是默认的：命令表里绝大多数条目没有 assertion，
    // 没人知道哪些是"还没写"、哪些是"写不出来"。于是覆盖率是个死数字，
    // 静态检查也无从判断新加的命令是不是漏了断言。
    //
    // 【判据】本字段非空 ⇒ 本命令**豁免**断言求值；本字段为空且 assertion
    // 也为空 ⇒ **不合规**，check_ai_meta 第 23 项会直接报错。
    // 两者互斥（同时给 = 自相矛盾，也会被检查拦下）。
    //
    // 【豁免必须写理由】理由会被 tools/list 一并暴露给调用方
    // （expectedEffect 为空、assertionExempt 非空）——
    // 客户端于是知道"这条命令的成败信号不可回读验证"，
    // 而不会把 ok=true 当成"效果确实达成"。
    const char* assertionExempt = nullptr;
};

const std::vector<CommandDef>& CommandTable();
const CommandDef* FindCommand(const std::wstring& name);
const CommandDef* FindCommand(CmdId id);

// 命令规格 "Name[:arg]" —— keymap 里 8 档视图模式必须带第几档，
// 否则 Ctrl+Shift+1..8 会全部落到 arg=""（= 第 0 档）。这里是唯一拆分点。
bool SplitCommandSpec(const std::wstring& spec, std::wstring* name, std::wstring* arg);

// 把旧式单字符串参数转成 Args。
//
// 这是**适配层**，只为让既有的 keymap / 序列 / 鼠标规则（都写 "ViewMode:3" 这种
// 单值规格）继续工作。转换规则从命令自己的 argsSchema 推导 —— 取第一个
// required（没有 required 则取第一个 properties）作为键名，按 schema 声明的
// 类型决定存成数字还是字符串。
//
// 关键：这让"哪个键对应这一档参数"只有一份真源（schema），不再需要一张手工
// 维护的映射表（之前 ai_api 里的 kArgKeys 就是缺这个推导而不得不加的补丁）。
// schema 解析失败或无参数时返回空对象。
Args ArgsFromLegacyArg(const CommandDef& def, const std::wstring& legacyArg);

// 这条命令是否有**必填参数**（argsSchema 的 required 非空）。
//
// 【为什么要有这个公开判定】"能不能只靠参数为空的一条命令规格触发它"是个
// 跨模块都要用的判据，而它只能从 schema 读（命令表是唯一真源）：
//   · 命令面板：有必填参数就必须追问，**绝不空参调用** —— 否则 clipboard.copy
//     会把**空串**写进剪贴板（静默毁掉用户上一条复制内容，比失败更坏），
//     tab.rename 会变成"清空别名"；
//   · 菜单右端的快捷键列：键位表里的键按下后走的是**空参**路径
//     （InvokeByNameLegacy(cmd, L"")），对 `fs.open` / `clipboard.copy` 这类
//     命令来说，键根本按不出那一行的效果。给这种行标键 = 显示了一个
//     "按下去不是这个动作"的键。
// 两处必须共用同一份判定，否则"面板追问、菜单却照标键"就是两套自相矛盾的真相。
// 解析失败视为无必填参数（与 ArgsFromLegacyArg 的宽松口径一致）。
bool HasRequiredArgs(const CommandDef& def);

// ---------------------------------------------------------------- 返回值
//
// 只读操作要能返回结构化结果（业界 P6）。
//
// 为什么不让所有 handler 都返回 Value：131 条命令里绝大多数是"做了/没做成"，
// 让它们都构造一个 JSON 对象是纯负担，也容易让"写操作假装有返回值"。
//
// 做法：**单独一条注册通道** —— 需要返回值的命令用 RegisterWithResult 注册，
// 其余保持 bool。CommandBus 内部记一个标志，调用方按 hasResult() 判断。
//
// 结果统一是 JSON 对象：便于以后直接序列化成 MCP 的 structuredContent，
// 不需要为每种命令定义 C++ 结构体（那又会变成"只有 C++ 能读懂"的私有格式）。
class CommandBus {
public:
    // handler 收具名参数对象。不再有"字符串 arg"。
    using Handler = std::function<bool(const Args& args)>;
    // 带返回值的 handler。result 由 handler 填，失败时可不填。
    using ResultHandler = std::function<bool(const Args& args, json::Value* result)>;

    static CommandBus& Get();

    void Register(CmdId id, Handler fn);
    // 注册一个会产出结构化结果的命令。同一 id 只能注册一次（后者覆盖）。
    void RegisterWithResult(CmdId id, ResultHandler fn);
    void RegisterByName(const wchar_t* name, Handler fn);

    // 新入口：具名参数。所有新代码都该用它。
    // 返回值仍是"成功/失败"；要拿结构化结果请用 InvokeResult。
    //
    // actor 必须传 Actor::Xxx()（见 commands_actor.h 的 Actor）。传字符串字面量会被
    // check_components.py 判红 —— 那样才能保证"新增调用点不会漏掉分类"。
    bool Invoke(CmdId id, const Args& args, Actor actor = Actor::Ui());
    bool InvokeByName(const std::wstring& name, const Args& args, Actor actor = Actor::Ui());

    // 带结果的调用。result 可为空（只关心成败）；非空时若命令有返回值则填入，
    // 若该命令没有返回值则置为空对象（不编造数据）。
    bool InvokeResult(CmdId id, const Args& args, json::Value* result,
                      Actor actor = Actor::Ui());
    bool InvokeByNameResult(const std::wstring& name, const Args& args, json::Value* result,
                            Actor actor = Actor::Ui());

    // 这条命令是否会产出结构化结果。UI/文档侧据此决定要不要展示"结果"区域。
    bool HasResult(CmdId id) const;
    bool HasResult(const std::wstring& name) const;

    // 旧入口（适配层）：单字符串参数。保留是因为 keymap / 序列 / 鼠标规则里
    // 写的是 "ViewMode:3" 这种规格，且它们不关心参数形状。
    // 实现会经 ArgsFromLegacyArg 转成 Args，因此新旧路径最终走同一个 handler。
    bool InvokeLegacy(CmdId id, const std::wstring& arg, Actor actor = Actor::Ui());
    bool InvokeByNameLegacy(const std::wstring& name, const std::wstring& arg,
                            Actor actor = Actor::Ui());

    // 破坏性命令的**人工确认**（实现见 src/commands_confirm.cpp）。
    //
    // 为什么是回调而不是总线自己弹框：CommandBus 不持有窗口引用
    // （与 CanExecute 同一口径），所以"问用户"必须由 App 提供实现。
    // 口径：只在**人工**调用时问 —— AI 侧走危险双门（aiAllowDanger + approve）。
    using ConfirmFn = std::function<bool(const CommandDef& def)>;
    void SetConfirmFn(ConfirmFn fn) { confirmFn_ = std::move(fn); }
    // 本次调用是否已经确认过（handler 用它避免重复弹框：总线已问过就别再问）。
    bool IsCallConfirmed() const { return callConfirmed_; }
    // 这条命令执行前**是否需要人工确认**（破坏性 && 不可撤销 && 不在日常豁免表里）。
    bool NeedsConfirm(const CommandDef& def) const;
    // 确认收口：返回 false = 用户取消/通道未就绪，调用方必须中止本次执行。
    bool ConfirmIfDestructive(const CommandDef& def, const Args& args, Actor actor);

    // 最近一条命令（状态栏/日志用）
    std::wstring lastCommand() const { return last_; }

    // 最近一次"为什么没执行"。空串 = 上次调用没有被前置条件拦下。
    // 存在这里有两条理由：
    //   · UI 要能给出"这个按钮为什么是灰的"（与灰显同源，不另算一遍）；
    //   · AI/脚本侧要能回报真实原因，而不是笼统的"命令执行失败"。
    // 以前 Dispatch 算出了 reason 却只写进日志与审计，调用方看不到，
    // 于是对外只能报"执行失败"，把问题留给了猜。
    const std::wstring& lastBlockReason() const { return lastBlockReason_; }

    // 由 handler 主动给出"我为什么没做成"。
    //
    // 前置条件拒绝（CanExecute）有 reason，可 handler 内部失败（key 不存在、
    // 类型不符、路径为空……）以前只能 return false，原因烂在日志里，调用方
    // 拿到的是笼统的"执行失败"。调用方因此无法决策 —— 名字打错和值超范围
    // 该做的下一步完全不同。
    //
    // 约定：handler 在 return false 之前调用；Dispatch 随后把它搬到
    // lastBlockReason_，于是 UI / AI / 审计看到的是同一句话。
    // 返回 true 的 handler 不需要调用（残留会由下次 Dispatch 开头清空）。
    //
    // 【kind 是干什么的】只给一句话，调用方无法决定下一步 ——
    // "剪贴板被占用"和"这个视图不支持详情列"该做的完全不同。
    // kind 让调用点**在知道原因的地方**顺手给出机器可判读的类别，
    // 于是 ai_api 不用去匹配中文字符串（文案一改就断）。
    //
    // 默认 PreconditionUnmet：绝大多数失败确实是"前提没满足"
    // （没选中、目标不存在、参数缺失、id 不存在…），所以默认值是安全的那一个。
    // 不确定时**不要**硬凑一个类别 —— 留默认比猜一个错类别好。
    void NoteFailure(const std::wstring& reason,
                     FailKind kind = FailKind::PreconditionUnmet) {
        pendingReason_ = reason;
        pendingKind_ = kind;
    }

    // 本次失败/阻断的类别。没有失败时是 FailKind::None。
    //
    // 与 lastBlockReason() 同源：原因非空时它才有意义。
    FailKind lastFailKind() const { return lastFailKind_; }

    // 由 handler 主动给出"我做成了，但有件事得说"。
    //
    // 【为什么需要第三条通道】以前只有两种结果：成功（什么都不说）与失败
    // （NoteFailure）。可有一类结果既不是失败也不能沉默 —— 最典型的是
    // "设置已在本次运行中生效，但没能写盘"（数据目录只读）：报成失败是撒谎
    // （值真的改了），报成成功也是撒谎（下次启动就变回去）。
    // 用 NoteFailure 表达它会让调用方以为改动没生效、去重试或回滚。
    //
    // 【口径】只有 handler 确实完成了它的主要动作时才用。它不影响 ok，
    // 只被搬进 lastWarning() 与审计的 reason 字段（成功记录也要留下痕迹）。
    void NoteWarning(const std::wstring& text) { pendingWarning_ = text; }

    // 本次调用的告警（空 = 没有）。成功但"有代价"时非空。
    const std::wstring& lastWarning() const { return lastWarning_; }

    // 本次调用的来源是"机器"（AI / 脚本）还是"人"。
    //
    // 【为什么 handler 需要它】双入口命令（人有界面路径、机器有参数路径）原来只靠
    // args.IsEmpty() 判：空参数 = 人。这对**无参数命令**是错的 —— agent 传来的也是
    // {}，于是"人侧分支"被执行，弹出模态报告窗口，把管道调用卡到超时
    // （实测：session.list 从 AI 调返回 {"ok":false,"error":"timeout"}）。
    // 判据必须是"空参数 **且** 不是机器调用"，见 IsHumanCall()。
    //
    // 【口径】只看 ActorKind —— 人类来源的细分标签（key / mouse / sysmenu…）
    // 一律算"人"。以前这里是 `actor != L"ui"`，于是键盘、系统菜单、拖拽
    // 全被判成机器，撤销边界与审批强度跟着一起错。
    bool fromMachine() const { return actor_.kind == ActorKind::Ai; }

    // 记一条"**被拒绝**"的调用审计。
    //
    // 【为什么单独开一个入口】CommandBus::Audit 记的是"进了 Dispatch 的调用"。
    // 但 AI 通道有一整段在**进 Dispatch 之前**就返回了：
    // 未知命令 / 未暴露 / 未实现 / schema 非法 / 参数不合法 / 危险未批准。
    // 这些拒绝过去**一条审计都不写**（实测：连续 4 次被拒 → audit.log 0 增长）。
    //
    // 【为什么拒绝也要记】被拒的调用恰恰是最值得追的：
    // · 反复撞同一个错 → 调用方（或模型）用错了方法；
    // · 反复撞 danger_disabled → 有人在试探破坏性操作的边界；
    // · 审计断了这一段，"AI 动过什么"就不是完整的。
    // AGENTS.md §8.5 的口径是「没有审计的『成功』不认」—— 被拒的更不该消失。
    //
    // 【为什么不用 Audit】Audit 走 CommandDef 与 Dispatch 的内部状态，
    // 而拒绝发生时还没有 CommandDef（未知命令根本没有）。
    // 所以这里收"名字 + 原因"两个字符串，自己拼一行。
    //
    // errorCode 是**机器可判读**的分类；为什么这么设计见 audit_log.h。
    void AuditRejected(const std::wstring& commandName, const std::wstring& reason,
                       const std::wstring& errorCode);

private:
    CommandBus() = default;
    bool Dispatch(CmdId id, const Args& args, json::Value* result, Actor actor);

    // 记一条"执行了"的审计。参数为什么传 Args、verdict 为什么要结构化，
    // 见 src/audit_log.h 与 src/commands_audit.cpp。
    // errorCode 是**机器可判读**的分类（见 commands_actor.h 的 FailKind），
    // reason 是**人可读**的原因。两者分开是有意的：机器码不能塞中文，
    // 而中文原因也不该被机器码的位置挤掉。
    void Audit(const CommandDef& def, const Args& args, Actor actor, bool ok,
               const std::wstring& verdict, const std::wstring& errorCode,
               const std::wstring& reason, long long durationMs);

    std::vector<std::pair<CmdId, Handler>> handlers_;
    std::vector<std::pair<CmdId, ResultHandler>> resultHandlers_;
    std::wstring last_;
    std::wstring lastBlockReason_;
    FailKind lastFailKind_ = FailKind::None;  // 本次失败/阻断的类别
    std::wstring pendingReason_;   // handler 报的原因，本次 Dispatch 内消费
    FailKind pendingKind_ = FailKind::None;  // handler 报的类别，本次 Dispatch 内消费
    std::wstring lastWarning_;     // 本次调用的告警（成功但有代价）
    std::wstring pendingWarning_;  // handler 报的告警，本次 Dispatch 内消费
    Actor actor_;                  // 本次 Dispatch 的调用来源（handler 经 fromMachine 读）
    ConfirmFn confirmFn_;          // 破坏性命令的人工确认（App 注入）
    bool callConfirmed_ = false;   // 本次 Dispatch 内是否已确认过（handler 经 IsCallConfirmed 读）
};

// 双入口命令的统一判据："这次是人点的吗"。
//
// 用空参数当唯一信号会误判无参数命令（见 CommandBus::fromMachine 的注释）。
// 所有"人是这样、机器是那样"的分支都该用它。
inline bool IsHumanCall(const Args& args) {
    return args.IsEmpty() && !CommandBus::Get().fromMachine();
}
