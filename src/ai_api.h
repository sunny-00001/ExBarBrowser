// AI 操作接口：让 AI 成为命令总线的第二个发起者（架构规则 R17–R20）。
//
// 设计基线（docs/ARCHITECTURE-RULES.md §5 + R18/R19/R20）：
//   · 人机等价（R20）：AI 能做的每件事，人也能通过鼠标/键盘/命令面板做到；
//     不存在"只有 AI 能触发"的命令。
//   · 唯一入口：AI 不走单独的写实现，全部落到同一个 CommandBus —— 否则
//     "工具栏删除"和"AI 删除"迟早行为分叉。
//   · 元数据驱动（R18）：命令表里 aiExposed/risk/argsSchema 是唯一真源；
//     本层只做"读表 + 校验 + 派发 + 审计"，不硬编码任何命令知识。
//   · 只读观察（R19）：GetState 返回与 UI 同源的状态快照，不额外缓存。
//
// 与 mod 的差别：mod 在 explorer 进程内、由 helper 进程（B）驱动；
// ExBarBrowser 是独立进程，因此对外的"B"就是**任意本地客户端**
// （AI 宿主、脚本、MCP 服务器都行）。通道是命名管道，JSON 进 / JSON 出。
//
// 安全边界（fail-closed）：
//   · 未列出的命令 → 拒绝，不猜。
//   · 参数不满足 argsSchema 的必填项 → 拒绝，不做默认值兜底。
//   · Risk::Danger 的命令默认拒绝，除非 Process 显式带 approve=true
//     （调用方必须先向人类要到确认）。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "commands.h"
#include "json.h"

namespace ai {

// 工具描述：把 CommandDef 的 AI 元数据投影成 tools/list 的一项。
//
// 用 Effects 而非单个 risk：调用方需要区分"幂等的写"（可安全重试）与
// "不可逆的写"（必须先确认），单轴表达不了。
struct ToolSpec {
    // 规范名（点分，如 nav.back）。这是给机器的稳定标识（业界 P7）。
    std::wstring name;
    // 旧名（如 NavBack）。保留给审计日志与既有脚本对照；新客户端用 name。
    std::wstring legacyName;
    std::wstring title;        // 中文显示名
    std::wstring group;        // 分组（Tab/Go/Nav/...）
    std::wstring argsSchema;   // JSON Schema 文本（原样来自命令表）
    std::wstring outputSchema; // 结果 schema；空 = 该命令只回报成败
    Effects effects;
    bool needsSelection = false;
};

// 列出所有可暴露给 AI 的命令（aiExposed && implemented）。
// 顺序稳定（按命令表顺序），便于客户端做 diff。
std::vector<ToolSpec> ListTools();

// 按名查一项；找不到返回 false。
bool FindTool(const std::wstring& name, ToolSpec* out);

// 派发一次 AI 调用。所有副作用都经 CommandBus，actor 固定为 "ai"。
//
// args: 命令参数对象，**原样**交给命令（不再压平成字符串）。
//       校验依据是该命令自己的 argsSchema：
//         · 缺必填项 / 未知键 / 类型不符 / 枚举或范围越界 → 拒绝
//         · 无参数命令收到任何键 → 拒绝（静默忽略参数最难查）
//
// approve: 对**不可逆**（effects.destructive）的命令必须为 true，否则拒绝
//          （返回 ok=false，error 里给出 need_approval）。
//          非破坏性命令忽略此字段 —— 幂等的写不需要批准。
//
// 返回的 result 结构（调用方序列化成 JSON）：
//   { "ok": bool, "command": "...", "effects": {...}, "error": "..." }
struct CallResult {
    bool ok = false;
    std::wstring command;
    std::wstring risk;    // 兼容字段：read / write / danger（由 effects 推导）
    std::wstring error;   // 失败原因（ok=false 时必填）——给人看的中文
    // 机器可读的失败类别。为什么必须与 error 分开：客户端要按类别决策
    // （改参数 / 补命令 / 要审批 / 等用户），而中文串一改客户端就断。
    // 取值见 docs/AI-INTERFACE.md §3.1；空表示没有失败（ok=true）。
    //   unknown_tool / not_exposed / not_implemented / bad_args /
    //   schema_invalid / danger_disabled / need_approval / blocked / failed
    std::wstring errorCode;
    // 结构化结果（P6）。命令有 outputSchema 且执行成功时填入；
    // 其余情况为空对象 —— 不编造返回值。
    json::Value result = json::Value::MakeObject();
    bool hasResult = false;

    // ---- 自断言结果（见 CommandDef::assertion）----
    //
    // 与 ok 的关系：ok 说"handler 说它成功了"，assertions 说"效果确实对"。
    // 两者独立：
    //   · ok=true + 断言全过  → 真的成了
    //   · ok=true + 断言失败  → handler 以为成了，实际没成（最危险的一类 bug）
    //   · ok=false            → 没成，断言通常也过不了
    //
    // 断言失败**不改 ok**：ok 是 handler 的返回值，篡改它会让审计失真。
    // 但 CallResult 有断言失败时，ai 层会把它写进审计并在响应里显式给出。
    json::Value assertions = json::Value::MakeArray();
    bool hasAssertions = false;

    // 总判定："这次调用到底成没成"——调用方只看这一个字段即可，不必自己去
    // 遍历 assertions 或纠结 ok 与断言的关系。语义：
    //   · 有断言：verdict = ok && 全部断言通过（真正"效果也对"）
    //   · 无断言：verdict = ok（手头只有 handler 的成功信号，不编造更强结论）
    // 注意 verdict 不篡改 ok：审计里 ok 始终是 handler 的原值，保持真相。
    bool verdict = false;
};

// 按 JSON Schema 校验参数。返回 false 时 err 给出人可读原因。
//
// 单独暴露的原因：它是"参数是否合法"的唯一判定，UI 预校验（灰掉提交按钮
// 并显示原因）和单测都该走同一个实现，而不是各写一套。
// 注意它**不做权限判断** —— 那是 Call 的事。
bool ValidateAgainstSchema(const json::Value& schema, const json::Value& args,
                           std::wstring* err);

// 执行一次调用（已含校验 + 审计）。
CallResult Call(const std::wstring& command, const json::Value& args, bool approve);

// 状态快照（只读，R19：与 UI 同源，不另存副本）。
// 由 App 注册的回调填充，避免 ai 层反向依赖 App。
//
// 【为什么这几个字段必须有】凡是 AI 能**改**的状态，就必须能**读**回来。
// 只有写没有读时，调用方拿到 isError=false 就等于"我改成功了"—— 而实际
// 可能被前置条件静默跳过、被用户刚才的操作覆盖、或参数取了默认值。
// 这类"写完无法验证"的接口会把错误推到很久以后才暴露（且现场已丢失）。
// 判据见 tools/check_ai_meta.py 第 17 项：可写的界面状态字段必须有回读。
struct StateSnapshot {
    std::wstring activePath;              // 当前标签路径
    std::vector<std::wstring> tabPaths;   // 所有标签路径（按标签栏顺序）
    int activeTab = -1;                   // 激活标签序号（0 起）
    unsigned itemCount = 0;               // 当前目录项目数
    int viewMode = -1;                    // 当前视图模式（FVM_*），-1 = 未知

    // ---- 可写状态的回读（每个都对应一条 view.* / bmk.* 命令）----
    int sortMode = -1;                    // view.sort 改写；-1 = 未知
    int groupBy = -1;                     // view.group 改写
    bool bookmarkBarVisible = false;      // bmk.show / bmk.hide / bmk.toggle 改写
    bool navPaneVisible = false;          // view.toggle.nav 改写
    bool previewPaneVisible = false;      // view.toggle.preview 改写

    // ---- 当前是否在搜索结果里（search.find 改写）----
    // 只有写没有读时，"搜索成功"就只能靠命令自己的回执来证明 —— 而回执
    // 恰恰是最容易说假话的地方（宿主没换视图也会返回 ok）。这三个字段是
    // 搜索态的**唯一**可回读来源。
    bool searching = false;               // 正在显示搜索结果
    std::wstring searchScope;             // 搜索来源目录（空 = 不在搜索里）
    std::wstring searchQuery;             // 当前生效的搜索词

    // ---- 选中与焦点（观测面：AI 要能回答"用户此刻选中了什么、焦点在哪"）----
    // selectionPaths 与 selectionCount 同源（同一条 shell 枚举路径）；
    // paths 是按快照上限截断后的部分，truncated=true 表示没带全。
    unsigned selectionCount = 0;          // 选中项总数
    std::vector<std::wstring> selectionPaths; // 选中项路径（文件系统=完整路径；命名空间=解析名）
    std::vector<bool> selectionFolders;   // 与 selectionPaths 一一对应：是否文件夹
    bool selectionTruncated = false;      // paths 被上限截断
    std::wstring focus;                   // 焦点面板：file_view / nav_tree /
                                          // bookmark_bar / address_bar / unknown
};
using StateFn = std::function<StateSnapshot()>;
void SetStateProvider(StateFn fn);
StateSnapshot GetState();

// 把结果转成 JSON 文本（管道响应 / 脚本消费）。
std::wstring ResultToJson(const CallResult& r);
std::wstring StateToJson(const StateSnapshot& s);
// tools/list 的 JSON（{"tools":[...]}，符合 MCP tools/list 形状）。
std::wstring ToolsToJson();

// 能力面自描述：命令总数 / 已实现 / 已暴露，以及两张缺口清单
//   {"total":N,"implemented":N,"aiExposed":N,
//    "notImplemented":["..."], "notExposed":["..."]}
//
// 为什么必须由产品回答：tools/list 只含已暴露的命令，客户端看到"没有这条"时
// 无法区分三种情况 —— 命令不存在（要新增）/ 存在但未暴露（要补元数据）/
// 存在但未实现（要写 handler）。这三类的修法完全不同，靠客户端猜必然猜错。
std::wstring CapabilitiesToJson();

}  // namespace ai
