// 评估面完整性：不让"改了评估器再宣布通过"发生。
//
// 【要防的是什么】
// 自我迭代的闭环是「改代码 → 编译 → 跑测试 → 看结果 → 决定留不留」。
// 这条链里，**测试与构建脚本本身就是评估器**。如果 Agent 有能力削弱评估器
// （把 tests/run_all.sh 改成 exit 0、把某个测试改空、删掉一个测试文件、
// 或者给 build_fast.py 塞一段任意代码），那么"测试全绿"就不再说明
// 代码是对的 —— 只说明评估器被改软了。
//
// Darwin Gödel Machine 论文把这叫 **objective hacking**，并把
// "如何防止模型改 evaluator"列为公开问题；STOP 论文的实测结论是
// "应该**测量**边界，而不是假设它守得住"。本文件就是那个测量。
//
// 【怎么防】
// 维护一份**清单**（tests/baselines/eval-surface.json），列出构成评估面的文件及其 SHA-256：
//   · 构建器与测试入口（build_fast.py / build.sh / tests/run_all.sh）
//   · 全部测试源（tests/*.cpp）—— 少一个、弱一个都算削弱
//   · 全部静态校验器（tools/*.py）—— 它们判的是"命令表与实现是否一致"
//
// 每次 dev.build / dev.test **之前**实测一次：
//   · 清单必须**已被 git 跟踪且已提交**（签发权归人的机械锚）；
//   · 清单里列的文件必须都在，且哈希一致；
//   · 受保护目录里的**文件集合**必须与清单完全一致
//     （新增也算变化：新增一个测试二进制同样可能是塞后门，签发一次即可）。
//
// 判定不通过就**拒绝启动**并说清差在哪 —— 不是"跑完再说"，
// 因为跑完的结果本身已经被污染了。
//
// 【为什么"已跟踪 + 已提交"这一层不可省】
// 清单里的 selfSeal 能挡住"手改清单"（改哈希却没重签），
// 但它**挡不住"改软评估面 + 同步重签"** —— 重签会算出一个**正确的**新 seal。
// 真正的锚是提交：重签会让清单以"未提交改动"出现，人在 git status /
// review / CI 里看得见；而本工程没有 commit 命令，所以 AI 自己走不到
// "提交"那一步 —— **重签之后门仍然关着**，直到人提交。
//
// 【为什么"新增"也算削弱】
// 直觉上"多一个测试文件只会更强"。但 tests/ 下的 .cpp 会被 run_all.sh
// 编译并**执行**，一个新增的 tests/evil.cpp 就能在测试阶段执行任意代码。
// 所以这里的规则是「只许签发，不许自改」：改动评估面必须由人显式签发
// （python tools/gen_eval_baseline.py），签发记录进 git，可追溯。
#pragma once
#include <string>
#include <vector>

namespace evalsurface {

struct Report {
    bool manifestFound = false;  // 清单在不在（不在 = 无法校验）
    bool ok = true;              // 评估面未被削弱
    std::wstring reason;         // 不 ok 时的完整原因（可直接给 agent 看）
    std::vector<std::wstring> changed;  // 被改 / 被删 / 新增的文件（仓库相对路径）
    std::wstring manifestPath;
    int checkedFiles = 0;        // 实测了几个文件
};

// 清单路径：<exe 所在目录>\data\eval-surface.json
// 用 ExeDir 而不是 Config::Dir()：清单是**工程资产**（要入库、可评审），
// 不是用户数据（不该随 profile 变）。
std::wstring ManifestPath();

// 实测评估面。读清单 + 逐个哈希比对 + 比对受保护目录的文件集合。
// 找不到清单 → manifestFound=false、ok=false（无法校验时不能当作通过）。
Report Check();

// 复算清单正文的 selfSeal（给单测与诊断用）。
//
// 【为什么单独暴露它】"手改清单必然让 seal 对不上"这条断言需要能**独立复算**，
// 否则单测只能验证"我改了一个副本"—— 而那证明不了任何事。
// 复算口径与 tools/gen_eval_baseline.py 的 compute_seal 逐字节一致。
bool ComputeSelfSeal(const std::wstring& manifestText, std::wstring* sealOut);

// 同一份结果的 JSON 文本（给 dev.snapshot / 诊断用）。
std::wstring CheckJson();

}  // namespace evalsurface
