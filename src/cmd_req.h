// 命令的前置条件（requirements）—— 单独成文件的原因见下。
//
// 【为什么从 commands.h 拆出来】commands.h 已经 700 行（行数棘轮记在
// tests/baselines/file-length-baseline.json），而"前置条件"是一块**自成一体**的东西：
// 一个枚举 + 一个定长数组容器 + 一句人可读的原因文案。它被 CommandDef 只用
// 一个字段（requires_）引用，其余读者（app_commands*.cpp 的 Requirements{...}）
// 只是用它。两个文件讲同一件事反而更清楚：命令表讲"有哪些命令"，
// 这里讲"命令在什么条件下可执行"。
#pragma once
#include <cwchar>

// ---------------------------------------------------------------- 前置条件
//
// "此刻能不能执行"的判定 + 人可读原因（业界共识 P4；Blender 的 poll + disabled hint）。
//
// 为什么必须是**声明式枚举**而不是回调：
//   · UI 要能直接读来灰显按钮并显示 tooltip 原因；
//   · 静态检查脚本要能校验"声明了 needsSelection 就必须有 HasSelection 前置"；
//   · 回调形式没法被枚举、没法被工具化，最后又会变成"每个命令自己写一段 if"。
//
// 组合语义：Requirements 里的项是 **AND** 关系；空集 = 无条件（总能执行）。
// 原因文案按第一个不满足的项给出 —— 同时说三条不满足对用户是噪音。
enum class Req {
    None = 0,
    HasHistoryBack,     // 有可后退的历史
    HasHistoryForward,  // 有可前进的历史
    HasSelection,       // 当前有选中项
    HasTabs,            // 至少有一个标签
    HasClipboard,       // 剪贴板有可粘贴内容
    CanGoUp,            // 上一级有去处（盘符根算有：去此电脑；命名空间/UNC 根没有）
    // 内部用：状态探测还没注册（App 未初始化完），此时不能声称任何命令可执行。
    // 单独一项是为了给出准确原因 —— 借用 HasTabs 会说"没有打开的标签"，
    // 那是误导（标签可能好端端地开着，只是我们还没拿到状态）。
    StateUnavailable,

    // ---- 以下按"边界处必须拒绝"补的（第 8 步）----
    //
    // 为什么需要：这些命令的实现里都有 `if (i > 0) ...` 这类静默防御，
    // 结果是"在边界上按了没反应、也不说为什么"。VS Code 把这种前置条件
    // 声明成 when 子句，菜单项据此灰掉并给出原因（enablement）。
    // 我们照做：声明出来就能在 UI 灰掉、在审计里留下可读原因。
    CanMoveTabLeft,     // 当前标签不在最左
    CanMoveTabRight,    // 当前标签不在最右
    HasClosedTabs,      // 有可重新打开的已关闭标签
    HasMultipleTabs,    // 至少 2 个标签（才谈得上"上一个/下一个/关左侧"）
    HasSelectionSingle, // 恰好一个选中项（属性、打开方式这类只接受单个）
};

struct Requirements {
    // 用固定上限的数组而不是 vector：这是编译期常量表，不需要堆分配，
    // 也让 CommandDef 保持可聚合初始化（constexpr 友好）。
    Req items[4] = {Req::None, Req::None, Req::None, Req::None};

    constexpr bool Empty() const { return items[0] == Req::None; }

    constexpr Requirements() = default;
    constexpr Requirements(Req a) : items{a, Req::None, Req::None, Req::None} {}
    constexpr Requirements(Req a, Req b) : items{a, b, Req::None, Req::None} {}
    constexpr Requirements(Req a, Req b, Req c) : items{a, b, c, Req::None} {}
};

// 人能读懂的原因文案（灰按钮的 tooltip / 机器返回的 error 共用同一份）。
const wchar_t* ReqReason(Req r);
