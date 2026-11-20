// 命令的命名空间别名（业界 P7：名字点分、稳定、可演进）。
//
// 为什么需要这一层：
//   · 现有名字是三种风格混杂（NavBack / TabClose / CmdDelete），`Cmd` 前缀是历史
//     残留（当初用来区分"命令"与"导航"），语义上无意义；
//   · 外部调用方（脚本、AI、命令面板）需要一个稳定、可预测、能分组的名字空间，
//     否则每接一个新客户端都要重新学一遍命名规律；
//   · 破坏性演进需要"加新名、旧名保留"，不能直接改名（会打断已有的 keymap、
//     序列、用户脚本）。
//
// 做法：一张纯数据表（旧名 → 点分名）。查表时**两个方向都认** ——
// FindCommand("NavBack") 和 FindCommand("nav.back") 返回同一条 CommandDef。
// 表里没列的名字自动退化成一个可推导的点分名（group 小写 + 原名小写），
// 所以新增命令不必强制填表，但填了更可控。
#pragma once
#include <string>

namespace cmdnaming {

// 旧名 -> 点分名。查不到时返回空串（调用方应回退到推导规则）。
std::wstring AliasOf(const std::wstring& legacyName);

// 点分名 -> 旧名。查不到时返回空串。
std::wstring LegacyOf(const std::wstring& dottedName);

// 是否为点分名（含 '.'）。用于决定查表方向。
bool LooksDotted(const std::wstring& name);

// 规范名：优先别名，没有则用原名。用于展示/文档/审计。
std::wstring CanonicalName(const std::wstring& anyName);

}  // namespace cmdnaming
