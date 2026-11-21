// 开发面工具：给「快照 → 改代码 → 编译 → 测试 → 回滚」提供结构化入口。
//
// 这条链是闭环的最后一层（见 docs/FULL-COVERAGE.md §10.4）：
// 前四层已经让"判据成立"，这里让 Agent 能自己使用判据。
// 所有耗时命令都异步执行并发完成事件 —— 不能在 UI 线程等编译跑完。
#pragma once
#include <string>

#include "json.h"

namespace devtools {

struct Job {
    std::wstring id;
    std::wstring kind;      // build / test
    std::wstring command;
    bool done = false;
    int exitCode = -1;
    std::wstring output;    // stdout + stderr，UTF-8 原文
    std::wstring error;     // 启动失败等原因
};

// 启动一条异步作业，立即返回 job id。完成时发布 dev.job.done 事件
// （事件载荷含 id/kind/exitCode/output 摘要），订阅者不用轮询。
std::wstring StartJob(const std::wstring& kind, const std::wstring& command);

// 查询作业；找不到返回 false。返回的是拷贝，调用方可安全长期使用。
bool GetJob(const std::wstring& id, Job* out);

// 当前工作树快照：HEAD / 脏文件 / diff 统计 / profile 目录 / 可用构建器。
// 返回 JSON 文本。失败不编造数据，给 error 字段。
std::wstring Snapshot();

// 把一个仓库内的相对路径恢复到 HEAD。
// 拒绝绝对路径、盘符、..、以及未处于修改状态的文件（避免误删新文件）。
// 返回是否成功；err 给原因。
bool Rollback(const std::wstring& relPath, std::wstring* before, std::wstring* after,
              std::wstring* err);

// 只做路径合法性校验（供单测与 UI 预校验，不执行 git）。
bool ValidRepoPath(const std::wstring& relPath, std::wstring* err);

}  // namespace devtools
