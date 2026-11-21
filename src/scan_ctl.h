// 长任务的控制通道：取消标志 + 进度回报。
//
// 【为什么单独一个文件】长任务要能被"中途叫停"，数据层就得能问一句
// "还继续吗"；要能报进度，就得有个回调通道。这两件事在 disk_scan /
// file_tools / file_hash 三处都要用 —— 各自加一份字段就是三份实现，
// 而三份实现迟早分叉（一处查了取消、一处没查，表现就是"能点取消但还是卡住"）。
//
// 【层级】本文件只依赖 C++ 标准库，不认识窗口、不认识命令、不认识 AI。
// 下层（数据层）依赖它，上层（命令层 / 作业服务）也依赖它 —— 依赖方向单一。
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace scanctl {

// ---- 时间预算的统一表达 ----
//
// 【约定】**budgetMs == 0 表示不限时**，其余为正的毫秒数。
//
// 【为什么要有这个约定】命令层要能表达"这次作业不限时"（async 与人工路径的默认
// 语义就是它）。而三个数据层对 0 的原有解释并不一致：摘要那层 0 = 不限，
// 体积统计那层 0 = 立刻超时（tests/disk_scan_test.cpp 钉着这个行为，不能改）。
// 命令层若带着同一个 0 去调三层，就会出现"摘要算完了、体积统计秒超时"的怪事。
//
// 于是把职责切干净：
//   · 命令层只认 0 = 不限，其余一律是正的毫秒数；
//   · 数据层多认一个 noTimeLimit 标志，由命令层负责翻译。
// 这样"0 是什么意思"只有一处定义，各数据层只管"我有没有上限"。
constexpr unsigned long long kUnlimitedBudget = 0;

inline bool IsUnlimited(unsigned long long budgetMs) { return budgetMs == kUnlimitedBudget; }

// 进度回报：(阶段说明, 已处理数, 总量)。
//
// 阶段说明由**数据层自己给**（"正在统计大小" / "正在计算摘要"）——
// 数据层最清楚自己正在干什么，让上层猜只会猜错。总量为 0 表示总量未知
// （比如遍历前不知道有多少个文件），调用方按"不确定"渲染，不显示百分比。
using ProgressFn = std::function<void(const std::wstring& stage, unsigned long long done,
                                     unsigned long long total)>;

// 控制块。调用方持有，数据层只读。
struct Control {
    // 取消标志。非空时，置位即表示"别再往下走了"。
    // 指向谁由调用方决定（作业服务指向自己那份），数据层只读不改。
    const std::atomic<bool>* cancel = nullptr;

    // 进度回调。可空 —— 不关心进度时留空即可（不额外付开销）。
    ProgressFn onProgress;

    bool Cancelled() const {
        return cancel != nullptr && cancel->load(std::memory_order_relaxed);
    }

    void Report(const std::wstring& stage, unsigned long long done,
                unsigned long long total) const {
        if (onProgress) onProgress(stage, done, total);
    }
};

}  // namespace scanctl
