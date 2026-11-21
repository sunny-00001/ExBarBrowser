// IFileOperation 执行器 —— 全项目**唯一**实现。
//
// 【为什么单独成文件】shell_ops.cpp 一千五百行里既有"视图作用域"的操作
//（作用于用户碰巧选中的项），又有"路径作用域"的操作（撤销栈专用）。
// 两边都要执行 IFileOperation，而"执行"这件事本身有一套非平凡的判据：
// 必须同时看 PerformOperations 的 HRESULT **和** GetAnyOperationsAborted。
// 这套判据若留在某个大文件里，换个文件的人很容易只抄走一半 ——
// 于是同一个操作在删除路径上会查 aborted、在改名路径上不查。
//
// 职责：只做"执行 + 判成败 + 给出失败原因"，不碰任何文件语义。
#pragma once
#include <string>

#include "common.h"

namespace shellopexec {

// 执行 IFileOperation 并**同时**检查两件事：
//   1. PerformOperations 的 HRESULT
//   2. GetAnyOperationsAborted
//
// 只查 ① 是不够的：用户点取消、目标在过程中消失、部分失败，都会让
// PerformOperations 返回 S_OK 而 GetAnyOperationsAborted 为 TRUE。
// 原来只有 RenameTo 查了 ②，其余四处（删除/新建文件夹/转移/新建文本文件）
// 直接 return SUCCEEDED(...) —— 于是"用户取消了删除"在审计里被记成 ok=1，
// 调用方也以为文件已经没了。
//
// why（非空时写入失败原因）：shell 的失败原因**只存在于 HRESULT 里**
//（"目标已存在" / "访问被拒绝" / "名称非法"），返回 bool 就等于把
// 三种完全不同的下一步压成同一个 false —— 调用方只能给出一句
// "重命名失败（名称非法或目标被占用）"，这句话对每一种原因都不算错，
// 但对使用者毫无用处（该换名？该关掉占用它的程序？还是权限问题？）。
bool Run(IFileOperation* op, std::wstring* why = nullptr);

// HRESULT 的十六进制文本（给"文件操作失败"这类回执用）。
// 为什么自己转：Windows 没有现成的 wchar_t 版本格式化函数，
// 而这些代码路径上不能引入 CRT 格式化的 locale 差异。
std::wstring HexHr(HRESULT hr);

}  // namespace shellopexec
