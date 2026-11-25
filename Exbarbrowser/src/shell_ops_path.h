// 按**完整路径**的文件操作（撤销栈专用）。
//
// 【为什么与视图作用域的那组分开】
// 撤销时，目标**已经不在当前视图里**了（移动过的文件到了别的目录，
// 复制出来的副本在别处）。于是：
//   · "Selection" 语义（作用于用户碰巧选中的东西）用不了；
//   · RenameTo 的"目标必须在当前视图里"校验也过不了 ——
//     而那条校验的用意是"不要凭空操作视图外的目录"，撤销恰恰必须操作视图外。
//
// 所以这里另开一组**按完整路径**的文件操作，只被撤销栈调用，
// 不参与命令路径 —— 命令路径（file.* / fs.*）仍然只有一份实现。
//
// 共同点：都走 IFileOperation、都不弹 UI、冲突时**改名**而不是覆盖
//（FOF_RENAMEONCOLLISION），绝不静默覆盖用户文件。
//
// 放在独立文件而不是 shell_ops.cpp 里：shell_ops.cpp 的其余部分是
// "视图作用域"的 shell 操作（选中/排序/分组/隐藏/新建…）。两种作用域
// 混在一个文件里，读的人要一路翻到文件末尾才知道还有另一套语义。
#pragma once
#include <string>

#include "common.h"

namespace shellops {

bool RenamePath(HWND owner, const std::wstring& fullPath, const std::wstring& newName);
bool MovePathTo(HWND owner, const std::wstring& srcPath, const std::wstring& dstDir);
bool CopyPathTo(HWND owner, const std::wstring& srcPath, const std::wstring& dstDir);
bool DeletePath(HWND owner, const std::wstring& fullPath);

}  // namespace shellops
