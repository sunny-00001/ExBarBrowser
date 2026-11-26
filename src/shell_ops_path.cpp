#include "shell_ops_path.h"

#include "path_util.h"
#include "shell_op_exec.h"

namespace shellops {
namespace {

// 把一个完整路径的项移动/复制到目标目录（按路径，不依赖当前选中）。
bool TransferPath(HWND owner, const std::wstring& srcPathIn, const std::wstring& dstDirIn,
                   bool move, std::wstring* why) {
    // 路径归一化：撤销栈里的路径来自"当初那次操作"的记录，
    // 那里存的是归一化后的形式；这里再归一化一次是防御性的，
    // 保证"任何路径进 shell 都先过 ToShellPath"这条规则没有例外。
    const std::wstring srcPath = pathutil::ToShellPath(srcPathIn);
    const std::wstring dstDir = pathutil::ToShellPath(dstDirIn);
    if (srcPath.empty() || dstDir.empty()) return false;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(srcPath.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item))) ||
        !item)
        return false;
    ComPtr<IShellItem> dest;
    if (FAILED(SHCreateItemFromParsingName(dstDir.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&dest))) ||
        !dest)
        return false;
    shellopexec::Op op;
    if (!op.Begin(owner, FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION, why))
        return false;
    if (move)
        op.QueueMove(item.get(), dest.get());
    else
        op.QueueCopy(item.get(), dest.get());
    return op.Commit(why);
}

}  // namespace

bool RenamePath(HWND owner, const std::wstring& fullPathIn, const std::wstring& newName,
                std::wstring* why) {
    const std::wstring fullPath = pathutil::ToShellPath(fullPathIn);
    if (fullPath.empty() || newName.empty()) return false;
    // 名字里不能带路径分隔符 —— 那是"移动"，不是"重命名"。
    if (newName.find(L'\\') != std::wstring::npos || newName.find(L'/') != std::wstring::npos)
        return false;

    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(fullPath.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item))) ||
        !item)
        return false;

    shellopexec::Op op;
    if (!op.Begin(owner, FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION |
                             FOF_NOCONFIRMATION | FOF_NOERRORUI,
                  why))
        return false;
    if (!op.QueueRename(item.get(), newName.c_str(), newName)) return false;
    return op.Commit(why);
}

bool MovePathTo(HWND owner, const std::wstring& srcPath, const std::wstring& dstDir,
                std::wstring* why) {
    return TransferPath(owner, srcPath, dstDir, /*move=*/true, why);
}

bool CopyPathTo(HWND owner, const std::wstring& srcPath, const std::wstring& dstDir,
                std::wstring* why) {
    return TransferPath(owner, srcPath, dstDir, /*move=*/false, why);
}

bool DeletePath(HWND owner, const std::wstring& fullPathIn, std::wstring* why) {
    const std::wstring fullPath = pathutil::ToShellPath(fullPathIn);
    if (fullPath.empty()) return false;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(fullPath.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item))) ||
        !item)
        return false;
    // FOF_WANTNUKEWARNING：超配额/网络盘时 shell 必须弹警告，不能静默永久删除。
    // 撤销"复制"就是删副本 —— 副本回回收站，随时能还原。
    shellopexec::Op op;
    if (!op.Begin(owner, FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_WANTNUKEWARNING, why))
        return false;
    if (!op.QueueDelete(item.get(), fullPath)) return false;
    return op.Commit(why);
}

}  // namespace shellops
