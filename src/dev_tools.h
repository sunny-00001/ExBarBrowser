// 开发面工具：给「快照 → 改代码 → 编译 → 测试 → 回滚」提供结构化入口。
//
// 这条链是闭环的最后一层（见 docs/FULL-COVERAGE.md §10.4）：
// 前四层已经让"判据成立"，这里让 Agent 能自己使用判据。
// 所有耗时命令都异步执行并发完成事件 —— 不能在 UI 线程等编译跑完。
//
// 【两条硬约束】
//   1. **不经 shell**：可执行文件与参数是分开的两个东西，
//      参数永远不可能变成命令分隔符（见 dev_proc.h）。
//   2. **不碰评估器**：构建器与测试脚本是评估面，改弱了就等于自己给自己判及格。
//      每次跑之前实测评估面完整性（见 eval_surface.h）。
#pragma once
#include <string>
#include <vector>

#include "json.h"

namespace devtools {

// 硬超时上限（毫秒）。到点**杀掉整棵进程树**（见 dev_proc.h）并按失败上报。
//
// 【为什么是常量而不是调用参数】让调用方（也就是 agent）自己填超时，
// 等于让被看管的人自己决定"看管多久" —— 那就没有闸门了。
// 真实需求（编译很慢、测试很慢）应该通过**改这里**来满足，
// 而不是通过运行时参数：那道改动会留在 git 里，而运行时参数不会。
constexpr unsigned long long kBuildTimeoutMs = 30 * 60 * 1000;  // 编译：给足
constexpr unsigned long long kTestTimeoutMs = 15 * 60 * 1000;   // 测试：留 5 倍余量

struct Job {
    std::wstring id;
    std::wstring kind;      // build / test
    std::wstring command;   // 人类可读的等效命令行（仅供展示）
    bool done = false;
    int exitCode = -1;
    std::wstring output;    // stdout + stderr（UTF-8 原文，已按"头+尾"裁剪）
    bool truncated = false; // output 是否被裁剪过
    std::wstring error;     // 启动失败等原因
    bool timedOut = false;  // 是被硬超时**杀掉**的（与"跑失败"是两回事）
    unsigned long long timeoutMs = 0;      // 这个作业的硬超时上限
    unsigned long long startedAt = 0;  // GetTickCount64 基准，用来算已用时长
};

// 启动一条异步作业，立即返回 job id。完成时发布 dev.job.done 事件
// （事件载荷含 id/kind/exitCode/output 摘要），订阅者不用轮询。
//
// program 与 args **分离**：命令文本不存在，注入面不存在。
//
// timeoutMs 是**硬超时**：到点就杀掉整棵进程树，作业按失败收场
// （Job::timedOut = true）。传 0 = 不限时 —— 开发面作业不该传 0。
std::wstring StartJob(const std::wstring& kind, const std::wstring& program,
                       const std::vector<std::wstring>& args,
                       unsigned long long timeoutMs);

// 查询作业；找不到返回 false。返回的是拷贝，调用方可安全长期使用。
bool GetJob(const std::wstring& id, Job* out);

// 当前工作树快照：HEAD / 脏文件 / diff 统计 / profile 目录 / 可用构建器 / 评估面状态。
// 返回 JSON 文本。失败不编造数据，给 error 字段。
std::wstring Snapshot();

// 【P0-9：结构化 diff，只读】返回 JSON 文本：
//   head / files[]（path/status/tracked/bytes/mtime/sha256/added/deleted）
//   / patch / stagedPatch / patchTruncated / stat / error
// 与 Snapshot() 的区别：Snapshot 会**落盘**一份内容副本（为了可回滚）；
// Diff 只看不改 —— 审阅一次改动不该产生副作用，否则"看一眼"也会污染工作树。
std::wstring Diff();

// 【回滚搬到 dev_snapshot.h 了】
// 旧版这里有个 Rollback()：直接 `git checkout -- <file>`，把工作树里那份改动
// 丢掉，**不可逆**。现在改成"按快照还原"（src/dev_snapshot.h）：
// 先落盘存内容 → 回滚必须带 snapshotId → 回滚前再把当前内容存进快照，
// 于是回滚这一步本身也是可逆的（P0-5）。

// 只做路径合法性校验（供单测与 UI 预校验，不执行 git）。
bool ValidRepoPath(const std::wstring& relPath, std::wstring* err);

}  // namespace devtools
