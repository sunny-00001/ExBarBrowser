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

struct Job {
    std::wstring id;
    std::wstring kind;      // build / test
    std::wstring command;   // 人类可读的等效命令行（仅供展示）
    bool done = false;
    int exitCode = -1;
    std::wstring output;    // stdout + stderr（UTF-8 原文，已按"头+尾"裁剪）
    bool truncated = false; // output 是否被裁剪过
    std::wstring error;     // 启动失败等原因
    unsigned long long startedAt = 0;  // GetTickCount64 基准，用来判超时
};

// 启动一条异步作业，立即返回 job id。完成时发布 dev.job.done 事件
// （事件载荷含 id/kind/exitCode/output 摘要），订阅者不用轮询。
//
// program 与 args **分离**：命令文本不存在，注入面不存在。
std::wstring StartJob(const std::wstring& kind, const std::wstring& program,
                       const std::vector<std::wstring>& args);

// 查询作业；找不到返回 false。返回的是拷贝，调用方可安全长期使用。
bool GetJob(const std::wstring& id, Job* out);

// 当前工作树快照：HEAD / 脏文件 / diff 统计 / profile 目录 / 可用构建器 / 评估面状态。
// 返回 JSON 文本。失败不编造数据，给 error 字段。
std::wstring Snapshot();

// 【回滚搬到 dev_snapshot.h 了】
// 旧版这里有个 Rollback()：直接 `git checkout -- <file>`，把工作树里那份改动
// 丢掉，**不可逆**。现在改成"按快照还原"（src/dev_snapshot.h）：
// 先落盘存内容 → 回滚必须带 snapshotId → 回滚前再把当前内容存进快照，
// 于是回滚这一步本身也是可逆的（P0-5）。

// 只做路径合法性校验（供单测与 UI 预校验，不执行 git）。
bool ValidRepoPath(const std::wstring& relPath, std::wstring* err);

}  // namespace devtools
