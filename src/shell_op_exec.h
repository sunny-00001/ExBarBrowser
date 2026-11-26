// IFileOperation 执行器 —— 全项目**唯一**实现。
//
// 【为什么单独成文件】shell_ops.cpp 一千五百行里既有"视图作用域"的操作
//（作用于用户碰巧选中的项），又有"路径作用域"的操作（撤销栈专用）。
// 两边都要执行 IFileOperation，而"执行"这件事本身有一套非平凡的判据。
// 这套判据若留在某个大文件里，换个文件的人很容易只抄走一半 ——
// 于是同一个操作在删除路径上会查 aborted、在改名路径上不查。
//
// 【2026-11-26 重构】原来这里只有 Run()：调用方各自
// CoCreateInstance → SetOwnerWindow → SetOperationFlags → 逐项排队 → Run。
// 那条舞步在 7 处重复，且"排队阶段"的 HRESULT 全被丢弃 —— 于是
//   · DeleteItem 全部排队失败 → 队列为空 → PerformOperations 返回 S_OK →
//     Run 返回 true → 调用方/AI 拿到"删除成功"，文件还在（假成功）；
//   · SetOperationFlags 失败 → FOF_ALLOWUNDO 没设上 → 删除绕过回收站且不提示。
// 现在把整条舞步收进 Op（RAII 构建器）+ Run（唯一执行点），
// 排队失败、逐项失败、标志设置失败都在这里被捕获，调用点无处可漏。
//
// 职责：只做"构建 + 执行 + 判成败 + 给出失败原因"，不碰任何文件语义。
#pragma once
#include <string>

#include "common.h"

namespace shellopexec {

// HRESULT 的十六进制文本（给"文件操作失败"这类回执用）。
// 为什么自己转：Windows 没有现成的 wchar_t 版本格式化函数，
// 而这些代码路径上不能引入 CRT 格式化的 locale 差异。
std::wstring HexHr(HRESULT hr);

// 一次 IFileOperation 的构建器。
//
// 用法（唯一正确写法）：
//   shellopexec::Op op;
//   if (!op.Begin(owner, FOF_ALLOWUNDO | FOF_WANTNUKEWARNING, why)) return false;
//   for (...) op.QueueDelete(item.get(), label);   // 失败会自己记账，不必逐点判
//   if (!op.Commit(why)) return false;
//
// 纪律：**不要**再在别处直接 CoCreateInstance(CLSID_FileOperation)。
// 队列方法返回 bool 只是给"要不要提前退出"用；即使全返回 false，
// Commit() 也会因为"一项都没排上"而判失败（这是假成功的第一道防线）。
class Op {
public:
    Op() = default;
    ~Op();
    Op(const Op&) = delete;
    Op& operator=(const Op&) = delete;

    // 创建 + 设标志 + 设 owner。
    // flags 设置失败**必须**判（它决定删除是否进回收站）；owner 失败只记日志
    //（只影响对话框归属，不影响操作语义）。
    bool Begin(HWND owner, DWORD flags, std::wstring* why = nullptr);

    bool valid() const { return static_cast<bool>(op_); }
    IFileOperation* get() const { return op_.get(); }

    // —— 排队。每个都可能失败（权限/只读/占用/路径失效），失败即记账。
    // label 只用于失败原因里"是哪一项"，可为空。
    bool QueueDelete(IShellItem* item, const std::wstring& label = L"");
    bool QueueRename(IShellItem* item, const wchar_t* newName,
                     const std::wstring& label = L"");
    bool QueueMove(IShellItem* item, IShellItem* destDir, const wchar_t* newName = nullptr,
                   const std::wstring& label = L"");
    bool QueueCopy(IShellItem* item, IShellItem* destDir, const wchar_t* newName = nullptr,
                   const std::wstring& label = L"");
    // 整批（选中集）移动/复制：一次调用排入多个项。
    bool QueueMoveAll(IShellItemArray* items, IShellItem* destDir);
    bool QueueCopyAll(IShellItemArray* items, IShellItem* destDir);
    bool QueueNewItem(IShellItem* destFolder, DWORD attrs, const wchar_t* name);

    // 执行 + 判成败。
    //
    // 四路判据（缺一不可）：
    //   ① PerformOperations 的 HRESULT；
    //   ② GetAnyOperationsAborted（用户点取消）；
    //   ③ 进度接收器逐项结果（部分失败：3 个被占用时 shell 跳过它们、其余照做，
    //      ①② 都是"成功"——只有 Post*Item 的 hrResult 能看出来）；
    //   ④ 实际尝试项数为 0（排队阶段就全失败了，队列是空的）。
    //
    // 原实现只查 ①②，于是"用户取消了删除"在审计里被记成 ok=1、
    // "3 个文件被占用"被报成全部成功。③④ 就是补这两个洞。
    bool Commit(std::wstring* why = nullptr);

    // 诊断用：排队成功数 / 排队失败数（Commit 内部也会用）。
    unsigned queued() const { return queued_; }
    unsigned queueFailed() const { return queueFailed_; }
    // 第一个排队失败的 HRESULT（排队方法返回 false 时用于拼失败原因）。
    HRESULT firstQueueFail() const { return firstQueueFail_; }

private:
    // 排队结果的唯一记账点：所有 Queue* 都走这里。
    bool NoteQueue(HRESULT hr, const std::wstring& label);

    ComPtr<IFileOperation> op_;
    unsigned queued_ = 0;        // 排队成功的项数
    unsigned queueFailed_ = 0;   // 排队失败的项数
    HRESULT firstQueueFail_ = S_OK;
    std::wstring firstQueueFailLabel_;
};

}  // namespace shellopexec
