// 文件集工具的数据层：重复文件查找（tool.dupfinder）与目录比较（tool.compare）。
//
// 【为什么和 disk_scan 分成两个文件】两者都遍历目录，但回答的问题不同：
//   disk_scan   —— "谁占空间"：只需要大小，不读内容，永远不必打开文件；
//   file_tools  —— "这两份是不是同一份"：必须先比大小（便宜），再比内容（贵）。
// 合并会让"只统计体积"的命令被迫背上哈希依赖。
//
// 三条硬约束（与 disk_scan 同源，理由见其文件头）：
//   1. 必须能中途停：UI 线程上的递归遍历不设预算就是假死。
//   2. 必须报告完整性：timedOut / deniedDirs / complete 三个字段缺一不可，
//      否则"没有重复"与"没扫完"无法区分 —— 用户会据此删错东西。
//   3. 不跟随重解析点：junction 会绕回自己，无限递归。
//
// 【已知边界】单个超大文件（几十 GB）的哈希不受预算约束：中途放弃会得到
// "半个文件的摘要"，那是错误结果，比慢更糟。所以预算只在**文件之间**生效。
// 报告里如实写 hashedFiles，调用方能看出停在哪一步。
#pragma once
#include <string>
#include <vector>

namespace filetools {

struct Options {
    unsigned long long budgetMs = 1500;   // 时间预算；超了带已有结果返回
    unsigned long long minBytes = 1;      // 小于此字节数的文件不参与（典型的 0 字节占位文件）
    bool followReparsePoints = false;     // 默认不跟随（见文件头第 3 条）
};

// ---- 重复文件 ----

struct DupFile {
    std::wstring path;
    unsigned long long bytes = 0;
};

struct DupGroup {
    unsigned long long bytes = 0;   // 单份大小（组内每份都相同）
    std::wstring hash;              // SHA-256（小写十六进制）
    std::vector<DupFile> files;     // 至少 2 个
};

struct DupResult {
    std::wstring root;
    std::vector<DupGroup> groups;   // 按"可回收字节"降序
    unsigned long long wastedBytes = 0;   // sum(bytes * (n-1))
    unsigned scannedFiles = 0;      // 遍历到多少个文件
    unsigned hashedFiles = 0;       // 实际哈希了多少个
    unsigned deniedDirs = 0;        // 打不开的目录数（>0 说明结果偏少）
    bool timedOut = false;
    bool complete = false;          // !timedOut
    unsigned long long elapsedMs = 0;
};

// 在 root 子树里找内容相同的文件。
// 判据是"大小相同 + SHA-256 相同"，不靠文件名 —— 改名后的副本也能找到。
bool FindDuplicates(const std::wstring& root, const Options& opt, DupResult* out,
                    std::wstring* err);

// ---- 目录比较 ----

enum class Diff {
    Same,       // 两侧都有，内容一致
    Differ,     // 两侧都有，内容不同
    OnlyLeft,   // 只在左侧
    OnlyRight,  // 只在右侧
};

struct CompareEntry {
    std::wstring rel;              // 相对路径（用左侧的写法；大小写不参与比较）
    unsigned long long leftBytes = 0;
    unsigned long long rightBytes = 0;
    bool isFolder = false;
    Diff state = Diff::Same;
};

struct CompareResult {
    std::wstring left;
    std::wstring right;
    std::vector<CompareEntry> entries;   // 只含差异项 + （byContent 时）同大小但内容不同的项
    unsigned sameCount = 0;
    unsigned differCount = 0;
    unsigned onlyLeftCount = 0;
    unsigned onlyRightCount = 0;
    unsigned long long leftBytes = 0;
    unsigned long long rightBytes = 0;
    unsigned deniedLeft = 0;
    unsigned deniedRight = 0;
    bool timedOut = false;
    bool truncated = false;   // 差异项超过 maxEntries，entries 只含前 maxEntries 条
    bool complete = false;
    unsigned long long elapsedMs = 0;
};

struct CompareOptions {
    Options walk;                 // 遍历预算等
    bool byContent = true;        // true=大小相同再比 SHA-256；false=只比大小（快，但"同大小不同内容"会算成 Same）
    unsigned maxEntries = 2000;   // 差异项上限（结果集可能很大；超了置 truncated 由调用方提示）
};

bool CompareFolders(const std::wstring& left, const std::wstring& right,
                    const CompareOptions& opt, CompareResult* out, std::wstring* err);

// ---- 批量改名的"计划"（纯文本，不碰磁盘）----
//
// 【为什么要单独拆成纯函数】批量改名最容易毁掉一个目录，而毁掉的方式都是
// "算错了名字"而不是"调用失败"：重名、撞到已有文件、生成空名/非法字符。
// 这些判定必须在**动手之前**全部做完，而且必须能脱离窗口测。
//
// 占位符（大小写不敏感）：
//   {name} 原文件名（不含扩展名）   {ext} 原扩展名（含点，可能为空）
//   {old}  原完整名                 {n} {nn} {nnn} 1 起的序号（补零到 1/2/3 位）
// **模式里既没有 {ext} 也没有 {old} 时，自动保留原扩展名** —— "改前半段"是最常见的
// 意图，而悄悄丢掉 .txt 会让文件失去关联（双击打不开）。这一点与 Total Commander
// 的"扩展名单独一栏、默认保留"是同一个取舍。
// 认不出的花括号原样保留（像是用户要的文本），但**模式里一个有效占位符都没有**时报错 ——
// 那等于给所有文件同一个名字，结果必然是重名，早点说清比后面一串冲突好。
struct RenamePair {
    std::wstring from;   // 原名（只给名字，不含路径）
    std::wstring to;     // 新名
};

// 生成改名计划。
//   oldNames   这批要改的文件名（含扩展名），顺序即 {n} 的计数顺序
//   existing   目录里现有的所有名字（用来检测"撞到没被选中的文件"）
// 全部校验通过才返回 true；否则 newNames/plan 不被使用，err 写原因。
bool PlanBatchRename(const std::vector<std::wstring>& oldNames,
                     const std::vector<std::wstring>& existing, const std::wstring& pattern,
                     std::vector<RenamePair>* plan, unsigned* skippedSame, std::wstring* err);

}  // namespace filetools
