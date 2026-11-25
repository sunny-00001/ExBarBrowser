// 文件操作撤销栈的接线：记一条（NoteFileOp）与撤一条（ApplyFileUndo）。
//
// 单独成文件而不是塞进 app_commands_files.cpp：那个文件已经 1200+ 行，
// 再往里堆会让"撤销"这条链路淹没在命令注册里 —— 而撤销恰恰是最需要
// 能一口气读完的部分（它横跨 改名/移动/复制 三种逆操作 + 一个边界规则）。
//
// 机制见 src/file_undo.h；命令侧的记栈点见 app_commands_files.cpp。
#include "app.h"

#include "file_undo.h"
#include "nav_path.h"  // nav::Leaf：重做改名时取 afterPath 的末段作新名
#include "shell_ops_path.h"
#include "text_file.h"

void App::NoteFileOp(fileundo::Op op, std::vector<fileundo::Item> items,
                     const std::wstring& label) {
    if (items.empty()) return;
    fileundo::Entry e;
    e.op = op;
    e.items = std::move(items);
    // byMachine 取自**本次调用**的 actor —— 人工做的与机器做的必须分得开，
    // 否则机器一次批量撤销能把用户的手工整理全部滚掉（同 bmk.* 的口径）。
    e.byMachine = CommandBus::Get().fromMachine();
    e.label = label;
    fileUndo_.Push(std::move(e));
}

bool App::ApplyFileUndo(fileundo::Entry& e, std::wstring* why) {
    // 逆操作：改名 → 改回旧名；移动 → 移回原目录；复制 → 删掉副本。
    //
    // 【为什么用按路径的那组 shellops】
    // 撤销时目标已经不在当前视图里了（移动过的到了别的目录、复制出的副本在别处），
    // 既有的 Selection 语义与 RenameTo 的"必须在视图里"校验都用不了。
    // 见 shell_ops_path.h 里"按路径的文件操作"一段。
    std::vector<fileundo::Item> failed;
    for (auto& it : e.items) {
        bool ok = false;
        switch (e.op) {
        case fileundo::Op::Rename: {
            // 改回旧名。新名字就是旧名字 —— beforeName 记的是操作前的名字。
            ok = !it.beforeName.empty() && shellops::RenamePath(hwnd_, it.afterPath, it.beforeName);
            break;
        }
        case fileundo::Op::Move: {
            // 移回原目录（beforePath 的父目录）。
            const std::wstring dir = ParentOf(it.beforePath);
            ok = !dir.empty() && shellops::MovePathTo(hwnd_, it.afterPath, dir);
            break;
        }
        case fileundo::Op::Copy:
            // 副本删掉（进回收站，随时可还原）。
            ok = shellops::DeletePath(hwnd_, it.afterPath);
            break;
        }
        if (!ok) failed.push_back(it);
    }
    if (failed.empty()) {
        e.items.clear();
        return true;
    }
    // 部分失败：把已经撤成功的项剔掉，让下一次重试只试剩下的
    //（否则已撤掉的那几项会一直失败，把这条记录永久卡死）。
    e.items = std::move(failed);
    if (why)
        *why = L"撤销未完全成功（" + std::to_wstring(e.items.size()) + L" 项失败，可能是目标已被占用或已不存在）";
    return false;
}

bool App::ApplyFileRedo(fileundo::Entry& e, std::wstring* why) {
    // 正向：改名 → 改回新名；移动 → 移到新目录；复制 → 从源再复制一份。
    //
    // 三种操作里，撤销结束后的现状都是"回到 beforePath"：
    //   · Rename 撤销后 = beforePath（旧名），redo = 改成 afterPath 的末段；
    //   · Move   撤销后 = beforePath（原目录），redo = 移到 afterPath 的父目录；
    //   · Copy   撤销后 = 副本已删、源 beforePath 还在，redo = 从源复制到目标目录。
    // 字段名与 ApplyFileUndo 完全对称，不引入第二套记录格式。
    std::vector<fileundo::Item> failed;
    for (auto& it : e.items) {
        bool ok = false;
        switch (e.op) {
        case fileundo::Op::Rename: {
            const std::wstring newName = nav::Leaf(it.afterPath);
            ok = !newName.empty() && shellops::RenamePath(hwnd_, it.beforePath, newName);
            break;
        }
        case fileundo::Op::Move: {
            const std::wstring dir = ParentOf(it.afterPath);
            ok = !dir.empty() && shellops::MovePathTo(hwnd_, it.beforePath, dir);
            break;
        }
        case fileundo::Op::Copy: {
            const std::wstring dir = ParentOf(it.afterPath);
            ok = !dir.empty() && shellops::CopyPathTo(hwnd_, it.beforePath, dir);
            break;
        }
        }
        if (!ok) failed.push_back(it);
    }
    if (failed.empty()) {
        e.items.clear();
        return true;
    }
    e.items = std::move(failed);
    if (why)
        *why = L"重做未完全成功（" + std::to_wstring(e.items.size()) + L" 项失败，可能是目标已被占用或源已不存在）";
    return false;
}
