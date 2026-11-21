// 目录体积统计（OpDiskUsage / OpTopN 的数据源）。
//
// 为什么单独一个文件而不是塞进 shell_ops：
//   · 它**不碰 shell/COM**，纯文件系统遍历，可以脱离窗口单测；
//   · 它有明确的"可中断 + 可解释"约束，值得单独承载这些语义。
//
// 三条硬约束（都来自踩过的坑）：
//   1. **必须能中途停**：几万个文件的目录递归要几十秒，卡在 UI 线程就是假死。
//      调用方给 deadline，超时就返回已经统计到的部分，并置 timedOut。
//   2. **必须报告完整性**：只给一个数字而不说"没算完"，调用方会当成真实值。
//      所以有 truncated / timedOut / scanned 三个字段。
//   3. **不能跟随重解析点**（符号链接 / junction）：跟随会无限递归，
//      而且会把链接目标的大小算到链接所在目录下，数字是错的。
#pragma once
#include <string>
#include <vector>

#include "scan_ctl.h"

namespace diskscan {

struct Entry {
    std::wstring name;
    std::wstring path;
    unsigned long long bytes = 0;
    bool isFolder = false;
    unsigned items = 0;        // 目录下的条目数（文件为 0）
    bool complete = true;      // 该子树的统计是否完整（false = 中途放弃）
};

struct Result {
    std::wstring path;                 // 被统计的目录
    std::vector<Entry> entries;        // 直接子项，按 bytes 降序
    unsigned long long totalBytes = 0; // 该目录下所有内容总大小
    unsigned scannedFiles = 0;         // 实际访问了多少个文件
    unsigned scannedDirs = 0;
    // 打不开的目录数（权限不足/被占用）。>0 表示结果**偏小**，
    // 上层必须能把它与"真的就这么大"区分开 —— 文件头硬约束第 2 条要求
    // "必须报告完整性"，静默跳过就无法报告。
    unsigned deniedDirs = 0;
    bool complete = true;              // 整次统计是否完整（无超时、无无法访问项）
    bool timedOut = false;             // 到 deadline 提前结束
    bool truncated = false;            // 结果被 limit 截断
    // 被**取消**（调用方要求停）而提前结束。
    //
    // 为什么与 timedOut 分开：超时是"没那么多时间"，取消是"我不要了"。
    // 两者结果都不完整，但调用方要做的下一步完全不同 ——
    // 超时可以加大 budget 重试，取消是用户已经不要这个结果了。
    bool cancelled = false;
    unsigned long long elapsedMs = 0;
};

struct Options {
    int limit = 50;                 // 返回前 N 个子项（<=0 不限）
    unsigned long long budgetMs = 1500;  // 时间预算；超了就带着已有结果返回
    bool followReparsePoints = false;    // 默认不跟随（见文件头第 3 条）
    // 不设时间上限。budgetMs==0 在本层是"立刻超时"（历史语义，不改），
    // "不限时"走这个标志 —— 原因与翻译责任见 scan_ctl.h。
    bool noTimeLimit = false;
    // 取消 + 进度通道（可空 = 不支持取消、不报进度）。
    // 见 scan_ctl.h：为什么这两件事共用一个结构体。
    const scanctl::Control* ctl = nullptr;
};

// 统计 path 下的直接子项各自占多少字节，并按大小降序取前 limit 个。
// fail 时返回 false 且 err 有人可读原因。
bool Scan(const std::wstring& path, const Options& opt, Result* out, std::wstring* err);

}  // namespace diskscan
