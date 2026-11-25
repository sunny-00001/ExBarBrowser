// shellops 各 TU 共用的内部辅助（只对工程内部可见，不进对外 API）。
//
// 【为什么收进头文件】视图作用域原先都在 shell_ops.cpp 一个 TU 里，
// 匿名命名空间里的 GetFv2 / GetFolder / HiddenFlags 与 ItemFullPath / SamePath
// 随手就能用。拆出 shell_ops_query.cpp 之后这些机制必须只有一份：
//   · 前三个是薄包装/常量，直接 inline 在这里；
//   · 后两个有实质逻辑，声明在此，唯一实现仍在 shell_ops.cpp。
// 不要让两个 TU 各抄一份 —— QI 的 ComPtr::Reset 细节与路径比较的 ordinal
// 语义都曾经踩过坑，写第二遍就是给下一次分叉留门。
#pragma once
#include <string>

#include "shell_ops.h"

namespace shellops::detail {

inline ComPtr<IFolderView2> GetFv2(IShellView* view) { return FolderView2(view); }

inline ComPtr<IShellFolder> GetFolder(IFolderView2* fv2) { return ShellFolder(fv2); }

// 写面（ToggleHidden / CycleHiddenState）与读面（GetHiddenFlags）必须用同一个掩码，
// 否则"读到的是另一个开关"，比没有读面更糟。
inline DWORD HiddenFlags() {
    return SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN | SSF_SHOWEXTENSIONS;
}

bool ItemFullPath(IShellFolder* folder, PCUITEMID_CHILD pidl, std::wstring* out);
bool SamePath(const std::wstring& a, const std::wstring& b);

}  // namespace shellops::detail
