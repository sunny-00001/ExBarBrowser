// 复制/移动的**撤销记录构造**：纯逻辑，不碰 App、不碰 shell。
//
// 【为什么要单独一份】两处调用（命令面板的 复制到/移动到、以及 fs.copy/fs.move
// 那条「选中 → 转移 → 记撤销」链路）以前各写一遍"目标目录 + 原文件名"的落点
// 推导。两处都错在同一个假设上：**冲突时落点不存在**。实际 FOF_RENAMEONCOLLISION
// 的冲突形态是"目标目录里本来就有同名文件"，于是算出来的 after 指向的是
// 用户原有的那个文件 —— 撤销复制会把它删掉（数据丢失），撤销移动会把不相干的
// 同名文件搬回源目录。
//
// 【正确口径】落点在转移**之前**是否已存在，必须由调用方在转移前探好传进来：
//   · 已存在 → 冲突，shell 会改名，真实落点不可知 → 不记；
//   · after == 源文件（同目录复制/移动）→ 撤销会删源 → 不记；
//   · 其余且 after 确实存在 → 记一条。
//
// 纯函数是为了能单测：这条判据错了不报错，只是"撤销时删错文件"。
#pragma once

#include <string>
#include <vector>

#include "file_undo.h"

namespace transferundo {

// 转移**之前**调用：对每个源算出"目标目录里的同名落点是否已存在"。
// 返回与 sources 等长；leaf 为空（源不是有效路径）的位置填 false。
std::vector<bool> PrecheckDest(const std::vector<std::wstring>& sources,
                               const std::wstring& to);

// 转移**成功之后**调用：按上面的口径挑出可记的项。可能返回空（全被跳过）。
// preExisted 可以比 sources 短/为空 —— 缺失的位置按"没探过"处理（偏保守：
// 仍然要求 after 存在才记）。
std::vector<fileundo::Item> BuildItems(const std::vector<std::wstring>& sources,
                                       const std::wstring& to,
                                       const std::vector<bool>& preExisted);

}  // namespace transferundo
