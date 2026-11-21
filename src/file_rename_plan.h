// 批量改名的"计划"（纯文本，不碰磁盘）。
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
//
// 独立成文件（而非塞进 file_tools.cpp）：查重/比较要读文件内容，
// 这一组只做字符串运算 —— 混在一起会让"改名计划"淹没在遍历与哈希的代码里。
#ifndef FILE_RENAME_PLAN_H_
#define FILE_RENAME_PLAN_H_

#include <string>
#include <vector>

namespace filetools {

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

#endif  // FILE_RENAME_PLAN_H_
