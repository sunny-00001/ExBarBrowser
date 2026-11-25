#pragma once
#include <cwchar>

// ---------------------------------------------------------------- 调用来源
//
// 【为什么 actor 必须是类型，不能是字符串】
//
// 以前 actor 是 `const wchar_t*`，"是不是机器"只认 `actor != L"ui"`。
// 于是「系统菜单点的」「快捷键触发的」「拖进来的」「命令面板选的」
// 全被判成**机器** —— 而 fromMachine() 是四件事的共同前提：
//   ① 撤销边界（AI 的撤销只该退到自己那一步，不穿透用户的手工操作）
//   ② 审计归类（这条命令是人点的还是机器发的）
//   ③ 审批强度（机器侧不得自我批准破坏性操作）
//   ④ 人工改写率（人改写了机器的产出 vs 机器自己改）
//
// 前提答错，后面四件事全部错，而且**错得毫无征兆**：
// 用户按 Ctrl+Z 想撤自己刚才的操作，却因为"键盘算机器"被
// "只撤到机器那一步"的规则拦住 —— 用户看到的是"撤销坏了"。
//
// 判据本身必须是**闭集**：枚举 + 编译期检查，
// 而不是"约定大家只写这几个字符串"（约定守不住，下一个人加个 L"touch" 就漏了）。
enum class ActorKind {
    Human,  // 人点的（UI 控件 / 快捷键 / 鼠标 / 系统菜单 / 命令面板 / 拖拽 / 设置面板）
    Ai,     // 机器（AI 命名管道）
};

// 一次调用的来源。
//
// kind 决定"是不是机器"（唯一判定点）；source 只是人类来源的细分标签，
// 用来回答"人是从哪儿点的" —— 它**不参与**判定，只进审计与日志。
struct Actor {
    ActorKind kind = ActorKind::Human;
    const wchar_t* source = L"ui";

    // 人类来源的固定标签。集中在这里而不是散在各调用点，
    // 是为了让"新增一种人类来源"变成一次显式的登记，
    // 而不是又一次在调用点随手写个字符串。
    static Actor Ui()       { return {ActorKind::Human, L"ui"}; }
    static Actor Key()      { return {ActorKind::Human, L"key"}; }
    static Actor Mouse()    { return {ActorKind::Human, L"mouse"}; }
    static Actor SysMenu()  { return {ActorKind::Human, L"sysmenu"}; }
    static Actor Palette()  { return {ActorKind::Human, L"palette"}; }
    static Actor Seq()      { return {ActorKind::Human, L"seq"}; }
    static Actor Drop()     { return {ActorKind::Human, L"drop"}; }
    static Actor Settings() { return {ActorKind::Human, L"ui.settings"}; }
    static Actor Ai()       { return {ActorKind::Ai, L"ai"}; }
};

// 失败的**机器可判读类别**。
//
// 【为什么不能只有一个 "blocked"】
// agent 拿到 "blocked" 无法决定下一步，因为"被拦住"有四种，下一步完全不同：
//   · PreconditionUnmet（前置不满足）→ 换目标 / 先做前置动作，**重试有用**
//   · LockConflict（锁冲突）        → 稍后重试，或换一个对象
//   · CanceledByUser（被用户取消）  → **停下来问人**，重试只是骚扰
//   · NotSupported（能力不存在）    → 换一条路，**重试永远没用**
// 一锅端成 "blocked" 等于让调用方去猜 —— 而"猜"正是 agent 最容易做错的地方：
// 它要么一直重试到超时，要么直接放弃，两种都是错的。
//
// 枚举值与对外的 errorCode 一一对应，映射只在 FailKindCode 一处（唯一真源）。
enum class FailKind {
    None = 0,           // 没有失败（不是一种错误，只是"没有错误"）
    PreconditionUnmet,  // 声明的前置条件不满足（没选中、目标不在视图、参数缺失…）
    LockConflict,       // 目标被占用 / 被锁（剪贴板被占、目标已存在…）
    CanceledByUser,     // 用户主动取消（拒绝批准、在进度窗口上点了取消…）
    NotSupported,       // 该环境 / 该对象不支持这件事（视图模式不对、能力不存在…）
    TimedOut,           // 等时限到了（后台作业仍在跑，该去查它而不是重试）
};

// FailKind → errorCode（唯一映射处）。
// None 返回空串：它不是一种错误，只是"没有错误"。
const wchar_t* FailKindCode(FailKind k);
