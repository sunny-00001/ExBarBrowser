// 命令目录的内部契约：跨 TU 共享的表元数据常量 + 两段行表的声明。
//
// 【为什么要有这个头】命令表有 ~230 行条目，按「用户操作域 / 工具·Agent 域」
// 分在两个 TU 里（commands_table.cpp / commands_table_tools.cpp）。两段共用同一批
// schema/豁免/效果常量，所以常量必须有一份**唯一定义**放在头里 ——
// inline constexpr 在 C++17 起有外部链接且可重复定义，两边各内联一份、地址相同。
#pragma once
#include "commands.h"
#include "settings_bounds.h"   // kPaneSplitMin / kPaneSplitMax（与设置项、落盘同一份）

#include <vector>

// ---------------------------------------------------------------- 表元数据常量

// 无参命令的 schema（上百条命令都用它）：以前这个 41 字符的字面量在表里抄了上百遍，
// 占全表体积的一半以上，也让"哪些命令真的带参数"看不出来。提成常量后，带参数的条目自然凸显。
// 语义：显式声明"不接受任何参数"（additionalProperties:false），
// 这样 AI 多传一个字段会被 schema 校验拒绝，而不是被静默忽略。
inline constexpr const char* kNoArgs = R"({"type":"object","additionalProperties":false})";


// 双栏比例命令的入参 schema。
//
// 【为什么这里必须是字面量常量，而不是由常量拼出来】check_ai_meta.py 静态检查
// 只能解析字符串字面量/常量名 —— 之前改成运行时拼接后，检查器直接报
// "view.pane.split 的 argsSchema 解析不到"，等于把"schema 合法 / 参数被 handler
// 使用"这几项检查从这条命令上摘掉了。所以留字面量，改用**编译期断言**把数字
// 与数值域钉在一起：检查器读得到，漂移在编译时报错。
inline constexpr const char* kPaneSplitSchema = "{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":\"integer\",\"minimum\":20,\"maximum\":80,\"description\":\"主窗格占比百分比（左右并排=左栏宽度，上下堆叠=上栏高度）\"}},\"required\":[\"percent\"],\"additionalProperties\":false}";

// 从字面量里取 "minimum": / "maximum": 后面的整数（只看 ASCII 数字）。
// constexpr：判据必须在编译期成立，不能等运行时才发现 AI 拿到的边界是旧的。
constexpr int NumberAfter(const char* s, const char* token) {
    for (; *s; ++s) {
        const char* p = s;
        const char* t = token;
        while (*p && *t && *p == *t) {
            ++p;
            ++t;
        }
        if (!*t) {
            int v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                ++p;
            }
            return v;
        }
    }
    return -1;
}

static_assert(NumberAfter(kPaneSplitSchema, "\"minimum\":") == kPaneSplitMin,
              "view.pane.split schema 的 minimum 必须与 panesplit 的上下限一致");
static_assert(NumberAfter(kPaneSplitSchema, "\"maximum\":") == kPaneSplitMax,
              "view.pane.split schema 的 maximum 必须与 panesplit 的上下限一致");


// ---- 显式豁免理由（CommandDef::assertionExempt）----
//
// 为什么用常量：豁免理由会被 tools/list 暴露给调用方，逐条各写一份会写出十几种说法，
// 而静态检查只认非空。归成几类之后，哪一类豁免最多本身就是一条可追踪的信号。
//
// 每条理由都要回答同一个问题：**为什么这条命令写不出可求值的断言**，而不是"还没写"。
// 豁免不是说这条命令不可信，它只是说成败信号不能靠状态回读核对 —— 调用方拿到
// tools/list 里的 assertionExempt，就知道该自己补一次核对，而不是把 ok=true 当成效果达成。

// 导航是异步的：路径在 Shell 回调里才落地，执行后立刻回读仍是出发地。
inline constexpr const char* kExemptAsyncNav =
    "导航是异步的：路径在 Shell 回调里才落地，执行后立刻回读仍是出发地，写不出「跳过去了」的判据";

// 效果落在没有回读面的对象上：磁盘 / 剪贴板 / 进程外 / 另一个窗口。
inline constexpr const char* kExemptNoReadback =
    "效果落在没有回读面的对象上（磁盘 / 剪贴板 / 进程外 / 另一个窗口），状态快照里没有对应字段";

// 合法无变化：幂等，或「无事可做也算成功」 —— 效果没发生时命令仍返回成功。
inline constexpr const char* kExemptLegalNoChange =
    "合法无变化：幂等或「无事可做也算成功」，效果没发生时命令仍返回成功，$changed 必然误判";

// 效果相对入参点名的目标：状态快照只描述活动对象，写不出与入参无关的判据。
inline constexpr const char* kExemptTargetRelative =
    "效果落在入参点名的目标上，而状态快照只描述活动对象，写不出与入参无关的判据";

// 集合语义：期望值取决于上一次状态，重复调用同一命令时集合不变。
inline constexpr const char* kExemptSetSemantics =
    "选中集合语义：期望值取决于上一次选中状态，重复调用同一命令时集合不变，既没有唯一期望值也不能用 $changed";

// 焦点落在第几项：没有可回读的「当前焦点项序号」字段。
inline constexpr const char* kExemptFocus =
    "焦点落在第几项没有可回读字段（快照只有选中集合，没有焦点项序号）";

// 撤销的效果取决于上一次操作：改名/排序不改计数，删除/新增改计数。
inline constexpr const char* kExemptUndo =
    "撤销的效果取决于上一次操作（改名/排序不改变计数，删除/新增改变计数），没有单一期望值";

// 改哪个设置项由入参 key 决定，快照只暴露其中几项。
inline constexpr const char* kExemptConfig =
    "改哪个设置项由入参 key 决定，而状态快照只暴露其中几项，写不出与 key 无关的判据";

// 作业状态是后台对象的私有状态，不在 exbar/state 快照里 —— 回读面是 tool.job.get 的返回值本身。
inline constexpr const char* kExemptJobState =
    "作业状态是后台对象的私有状态，不在 exbar/state 快照里（回读面是 tool.job.get 的返回值本身），写不出快照级断言";

// ---- 同一操作的两条入口共用同一份 Effects（P0-11）----
//
// `file.*`（交互式：作用于用户当前选中）与 `fs.*`（确定式：目标由参数点名）
// 做的是**同一件事**，走的是同一批底层实现（shellops::DeleteSelection /
// RenameTo / TransferToFolder / CopyPaths），却各自抄了一份 Effects。
// 于是同一个事实有两个源头 —— 而且**暴露给 AI 的那份恰好是更乐观的那份**：
//   fs.delete 说 undoable=1（实际没有撤销栈）、fs.move 说 destructive=0
//   （移动确实让源文件消失）、fs.copy 与 file.copy.to 的 destructive 相反。
// AI 依据这些声明决定"敢不敢试错"，声明错了它就会**基于错的认知**行动。
//
// 修法不是"把两份改成一样"（下一个人加一条命令时又会分叉），
// 而是**让它们引用同一份常量** —— 编译期就绑死，抄不出来。
//
// 口径（每一条都有理由，不是随手填的）：
//   · undoable 说的是"**我们**能不能撤销"，不是"系统理论上能不能"。
//     2026-11-21 起：我们有自己的文件操作撤销栈（src/file_undo.h），
//     改名/移动/复制都记了逆操作 → 真能撤，声明 undoable=1；
//     删除撤不了（回收站还原不可靠、shell 的 undo 谓词在 IExplorerBrowser 里
//     取不到）→ 声明 0，AI 据此知道"这一步不可逆"，按 destructive 走审批。
//   · destructive 说的是"会不会让东西消失"。复制不消失 → 0；
//     移动会让源文件消失 → 1；删除 → 1。
//   · openWorld：只有"作用于进程外"才算。往剪贴板里写路径不是；
//     启动系统关联程序 / 弹系统属性对话框才是。
//   · scope：取**最坏情况**。fs.open 打开文件夹是进程内导航（CurrentTab），
//     打开文件是启动外部程序（System）—— 一条命令两种去向，声明必须覆盖大的那个。
inline constexpr Effects kEffectDelete{0, 1, 0, 0, 0, Scope::CurrentTab};
inline constexpr Effects kEffectRename{0, 0, 0, 0, 1, Scope::CurrentTab};
inline constexpr Effects kEffectCopyTo{0, 0, 0, 0, 1, Scope::CurrentTab};
inline constexpr Effects kEffectMoveTo{0, 1, 0, 0, 1, Scope::CurrentTab};
inline constexpr Effects kEffectOpen{0, 0, 0, 1, 0, Scope::System};
inline constexpr Effects kEffectOpenTab{0, 0, 0, 1, 0, Scope::App};
inline constexpr Effects kEffectOpenWindow{0, 0, 0, 1, 0, Scope::System};
inline constexpr Effects kEffectProperties{0, 0, 1, 1, 0, Scope::System};
inline constexpr Effects kEffectCopyToClipboard{0, 0, 1, 0, 0, Scope::CurrentTab};
inline constexpr Effects kEffectCutToClipboard{0, 0, 1, 0, 0, Scope::CurrentTab};

// ---------------------------------------------------------------- 两段行表

// 【为什么是两段而不是一张表】一张表跨两个 TU 写不了（匿名 namespace 是 TU 私有的），
// 而把 ~230 行条目塞进一个文件又会超单文件 500 行。按**作用域**切成两段：
//   · UserCommandRows() —— 面向用户交互的操作域（Tab/Go/Nav/File/View/Window/设置…）
//   · ToolCommandRows() —— 面向 Agent 与工具链的操作域（开发面/Session/作业/搜索域…）
// 拼接顺序 = 用户域在前、工具域在后，与拆分前的表顺序**逐行一致**（切点选在
// 「设置域」与「开发面」之间，那本来就是两个域的天然分界）。
std::vector<CommandDef> UserCommandRows();
std::vector<CommandDef> ToolCommandRows();
