// 结构化 diff 引擎（报告 P0-9 / B13）。
//
// 【这一层要解决的真问题】
// 自我迭代链路里，"改"这一步由外部 agent 用它自己的文件工具完成 ——
// 本仓提供的是**观测面**。观测面如果只给一句 `git diff --stat`，
// 审阅者就既看不到"改了哪些文件"，也看不到"改成了什么样"。
// 而"审批必须给动作本身的 diff"（不是描述）是这类系统的硬要求。
//
// 【旧实现的三处失明（这版逐个补上）】
//   1. **未跟踪的新文件在 git diff 里根本不存在**。`git diff` 只比已跟踪的路径，
//      所以 agent 写出来的**新文件**一个字都看不到 —— 而那恰恰是最该被审的部分。
//      更糟的是它还报 added=0 / deleted=0，看起来像"这个文件没有改动"。
//   2. **状态没说清**。只有 tracked 这个布尔，分不出"新增 / 修改 / 删除 /
//      重命名 / 未跟踪"。审阅者看不出区别就只能靠猜。
//   3. **路径解析被引号吃掉**。git 的非 -z 输出会把带特殊字符的路径加上引号，
//      老实按 substr 切会得到一个不存在的路径；而重命名在非 -z 输出里是
//      "old -> new"，取前半段拿到的是**旧路径** —— 旧路径在磁盘上已经没了。
//      本版全程走 -z（NUL 分隔、不加引号）。
//
// 【口径：工作树相对 HEAD，三样东西同源】
// 清单（--name-status）、行数（--numstat）、内容（patch）**全部出自 diff HEAD**。
// 不能拿 `git status` 的清单去配 diff HEAD 的内容：status 比的是**索引**、
// diff HEAD 比的是**工作树**，两者对"重命名"的判定口径不一样
// （实测：git mv 之后又改了内容，status 仍报 R，diff HEAD 报 D+A）——
// 混用就会"清单说这是重命名、patch 里却是删+增"，错位的那一边被静默丢掉。
//
// 【为什么单独一个文件】
//   ① 它要能被**脱离窗口**单测 —— 所以所有 git 调用都以 root 为参数，
//      不写死 tf::ExeDir()，单测可以在临时仓库里造出真实场景。
//   ② "git 输出怎么解析"必须只有一份真源：散在别处必然分叉，
//      而分叉的代价是"有的路径能看见、有的看不见"，且没人说得清为什么。
#pragma once

#include <string>
#include <vector>

namespace devdiff {

// 一个 diff 片段（hunk）：一段连续的改动。
//
// 字段与 git 的 unified diff 对齐，但**不依赖调用方会解析 git 格式** ——
// hunk 的行号已经算好了，调用方可以直接拿去定位。
struct Hunk {
    int oldStart = 0;   // 旧文件里的起始行（1 起）
    int oldLines = 0;   // 旧文件里被替换掉的行数
    int newStart = 0;   // 新文件里的起始行（1 起）
    int newLines = 0;   // 新文件里被替换掉的行数
    std::wstring header;  // "@@ -12,3 +12,4 @@"（人可读）
    std::wstring text;    // 正文：每行以 ' '（上下文）/ '+'（新增）/ '-'（删除）开头
};

// 一个文件的改动。
struct FileDiff {
    std::wstring path;      // 仓库内相对路径（斜杠分隔）
    // 封闭词表：untracked / added / modified / deleted / renamed / copied / typechange。
    // 词表是封闭的，调用方按这几个词分支 —— 不要靠猜 added/deleted 的组合
    // （"新增 0 行"和"没改动"在数字上一样，只有 status 能分开）。
    std::wstring status;
    std::wstring origPath;  // 仅 renamed/copied：改名前的路径
    bool tracked = false;   // git 跟踪的？
    unsigned long long bytes = 0;
    std::wstring mtime;     // 十进制 Unix 秒；空 = 取不到（文件已删/读不了）
    std::wstring sha256;    // 当前内容 SHA-256（小写 hex）；空 = 没算出来
    int added = 0;          // 新增行数
    int deleted = 0;        // 删除行数
    bool truncated = false;  // 内容超预算或不是文本 —— 只给了统计，没给逐行
    std::wstring reason;     // 为什么没给逐行（人可读）
    std::vector<Hunk> hunks;
};

struct Options {
    std::wstring root;      // 仓库根（绝对路径）
    std::wstring onlyPath;  // 只看这一个文件（仓库内相对路径）；空 = 全部
    // 预算都是**常量级**的默认值，调用方一般不改 ——
    // 它们是"防止一次拉回几 MB 文本把界面卡住"的闸门，不是功能旋钮。
    size_t maxFiles = 200;               // 最多展开几个文件
    size_t maxBytesPerFile = 128 * 1024; // 单文件的 diff 文本上限
    size_t maxTotalBytes = 2 * 1024 * 1024;
};

struct Report {
    bool ok = false;
    std::wstring error;        // ok=false 时的人可读原因
    std::wstring note;         // 补充说明（如"没有匹配的改动文件"）；空 = 无
    std::wstring head;         // 短 HEAD
    unsigned long long totalFiles = 0;  // git 报告的改动文件总数
    bool truncated = false;     // 整体超预算：还有文件没展开
    std::wstring truncatedReason;
    std::vector<FileDiff> files;
};

// 算出当前工作树的结构化 diff。
//
// 返回 ok=false 时 error 给原因（仓库不可用 / git 起不来）。
// **不编造数据**：git 不可用就报错，不返回一个"看起来没改动"的空 diff ——
// 那是最坏的一种假绿。
Report Compute(const Options& opt);

}  // namespace devdiff
