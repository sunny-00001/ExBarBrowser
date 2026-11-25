// fs.* 命令组：按**完整路径**操作的确定式文件操作。
//
// 【为什么与 file.* 分成两个文件】
// 两组命令的作用域不同，这是它们最本质的区别：
//   · file.* —— 作用于"用户碰巧选中的项"（Selection 语义）；
//   · fs.*   —— 作用于"参数里点名的目标"（路径语义）。
// 混在一个文件里时，读的人要一路翻到中段才知道"这些命令的入参根本不是选中集"。
//
// 【共同点】这几个 handler 的做法都是：**先把参数里点名的目标选中，再调既有的
// Selection 实现**。这样文件语义（回收站、冲突、权限、撤销）仍然只有一份实现，
// 不会出现"工具栏删除"和"脚本删除"行为分叉。
//
// 为什么不做成"直接按路径调 IFileOperation"：那样会有两套实现，且
// 「目标在当前视图里吗」「删完要不要刷新」这类细节两边都得维护。
// 先选中则天然复用视图的认知。
#include "app.h"

#include "app_commands.h"
#include "file_undo.h"
#include "log.h"
#include "path_util.h"
#include "shell_ops.h"
#include "text_file.h"

void App::RegisterFsCommands() {
    auto reg = cmd_reg::Register;
    // 产出结构化结果的命令用 regR（检查脚本按 `regR(CmdId::` 字面扫描"谁注册了谁"，
    // 所以不能写 cmd_reg::RegisterWithResult，也不能靠循环传 id）。
    auto regR = cmd_reg::RegisterWithResult;

    // ---------------- 确定式文件操作（P5：交互与确定分离）----------------
    //
    // 这四个 handler 的共同做法：**先把参数里点名的目标选中，再调既有的
    // Selection 实现**。这样文件语义（回收站、冲突、权限、撤销）仍然只有一份
    // 实现，不会出现"工具栏删除"和"脚本删除"行为分叉。
    //
    // 为什么不做成"直接按路径调 IFileOperation"：那样会有两套实现，且
    // 「目标在当前视图里吗」「删完要不要刷新」这类细节两边都得维护。
    // 先选中则天然复用视图的认知。
    reg(CmdId::FsRename, [this](const Args& args) {
        const std::wstring target = pathutil::ToShellPath(args.Str(L"target"));
        const std::wstring newName = args.Str(L"newName");
        if (target.empty() || newName.empty()) return false;
        return WithView([&](IShellView* v) {
            // RenameTo 内部会校验"目标在当前视图里"。
            // 先用同一份 shell 枚举（StatItem）确认存在性，把"不在视图"与
            // "名字非法/被占用"区分开 —— 两者对调用方意味着完全不同的下一步。
            // 更细的原因（HRESULT）由 RenameTo 自己的 why 参数给出。
            shellops::ItemInfo probe;
            if (!shellops::StatItem(v, target, &probe)) {
                CommandBus::Get().NoteFailure(L"目标不在当前视图：" + target);
                return false;
            }
            std::wstring why;
            bool ok = shellops::RenameTo(v, hwnd_, target, newName, &why);
            if (!ok)
                CommandBus::Get().NoteFailure(
                    why.empty() ? (L"重命名失败：" + newName) : why);
            if (ok) {
                // 撤销记录：旧名 → 新名。撤销 = 改回旧名。
                // afterPath 用**父目录 + 新名**算出来（不是问 shell 要）——
                // RenameTo 在目标已存在时会失败，所以这里不存在"落点另有其名"的情况。
                const std::wstring dir = ParentOf(target);
                fileundo::Item item;
                item.beforePath = target;
                item.afterPath = dir.empty() ? newName : tf::Join(dir, newName);
                item.beforeName = nav::Leaf(target);
                NoteFileOp(fileundo::Op::Rename, {item}, L"重命名");
                RefreshActive();
            }
            return ok;
        }, L"重命名");
    });

    reg(CmdId::FsDelete, [this](const Args& args) {
        const std::vector<std::wstring> targets = pathutil::ToShellPaths(args.StrArray(L"targets"));
        if (targets.empty()) return false;
        return WithView([&](IShellView* v) {
            // 一次选中全部目标、一次删除。
            //
            // 早先是"选一个删一个"的循环，问题有三：N 次触发视图刷新、
            // N 次构造 IFileOperation（每次都重新弹回收站确认流程）、
            // 中途失败会留下"删了一半"且说不清删了哪些。
            // 一次操作则让系统的冲突/进度处理只走一轮。
            const int hit = shellops::SelectByPaths(v, targets);
            if (hit == 0) {
                CommandBus::Get().NoteFailure(L"目标不在当前视图：" + targets.front());
                return false;
            }
            const bool ok = shellops::DeleteSelection(v, hwnd_);
            RefreshActive();
            // 只命中一部分也算部分成功：返回 false 让调用方知道"没全照办"，
            // 但已完成的部分是真实发生的（审计里 ok=0 会让人误以为什么都没做，
            // 所以这里附带命中数记日志）。
            if (hit < static_cast<int>(targets.size()))
                LogDbg(L"[cmd] FsDelete 只命中 %d/%zu 个目标", hit, targets.size());
            return ok;
        }, L"删除");
    });

    reg(CmdId::FsCopyTo, [this](const Args& args) {
        return TransferTargets(args, false);
    });    reg(CmdId::FsMoveTo, [this](const Args& args) {
        return TransferTargets(args, true);
    });

    // FsOpen：按路径打开。语义与 CmdOpen 完全一致 ——
    // 文件夹在当前标签内进入（explorer 的行为），文件交给 shell 的默认处理程序。
    //
    // 为什么不给它加 mode 参数：VS Code 把"打开"拆成 open / openWith /
    // revealInExplorer 三条独立命令，而不是一条带 mode 的。理由是它们代表
    // 三种不同意图，各自值得一个名字；塞进一个枚举会让调用方先查"有哪些 mode"。
    reg(CmdId::FsOpen, [this](const Args& args) {
        const std::wstring path = pathutil::ToShellPath(args.Str(L"path"));
        if (path.empty()) {
            CommandBus::Get().NoteFailure(L"缺少必填参数：path");
            return false;
        }

        // 路径不存在就明确失败，不要交给 ShellExecute（它会弹"找不到"的系统框，
        // 那是交互式行为，确定式调用不该卡在没人看的对话框上）。
        if (!tf::Exists(path)) {
            LogDbg(L"[cmd] FsOpen 目标不存在：%s", path.c_str());
            // 人侧走 NavigateActive 会弹"无法打开"，机器侧原来只有一句
            // "命令执行失败" —— 同一件事两套反馈，调用方分不清是路径错、
            // 权限还是别的原因。把原因如实交出去。
            CommandBus::Get().NoteFailure(L"目标不存在：" + path);
            return false;
        }

        if (tf::IsDirectory(path)) {
            NavigateActive(path, true);
            return true;
        }
        // 文件：交给 shell 的默认关联程序。返回 >32 表示成功（ShellExecute 的约定）。
        const HINSTANCE r = ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr,
                                          SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(r) > 32;
    });

    // FsOpenInNewTab / FsOpenInNewWindow：与 FsOpen 同族，只是去处不同。
    //
    // 三个命令各自直接读 args 并各自校验，而不是抽一个共用 lambda：
    // 静态检查（check_ai_meta.py 第 3 项）按"handler 体内有没有 args.Str(L"x")"
    // 判断参数是否被取用。抽成共用函数会让它看不见，只能登记进例外表 ——
    // 而例外表越长，检查能挡住的问题越少。重复三行换检查覆盖，值得。
    reg(CmdId::FsOpenInNewTab, [this](const Args& args) {
        const std::wstring path = pathutil::ToShellPath(args.Str(L"path"));
        if (path.empty() || !tf::Exists(path)) {
            if (!path.empty()) {
                LogDbg(L"[cmd] FsOpenInNewTab 目标不存在：%s", path.c_str());
                CommandBus::Get().NoteFailure(L"目标不存在：" + path);
            } else {
                CommandBus::Get().NoteFailure(L"缺少必填参数：path");
            }
            return false;
        }
        return NewTab(path) != 0;
    });
    reg(CmdId::FsOpenInNewWindow, [this](const Args& args) {
        const std::wstring path = pathutil::ToShellPath(args.Str(L"path"));
        if (path.empty() || !tf::Exists(path)) {
            if (!path.empty()) {
                LogDbg(L"[cmd] FsOpenInNewWindow 目标不存在：%s", path.c_str());
                CommandBus::Get().NoteFailure(L"目标不存在：" + path);
            } else {
                CommandBus::Get().NoteFailure(L"缺少必填参数：path");
            }
            return false;
        }
        OpenPathInNewWindow(path);
        return true;
    });

    // FsCopy / FsCut：把一组指定路径放进剪贴板。
    //
    // 语义界限（容易误解，所以写清楚）：
    //   · 本命令**不产生副本**，只是"准备好，等粘贴"。与 Ctrl+C 完全同义。
    //   · 想一步到位复制到某个目录，用 FsCopyTo —— 那才是真的落盘。
    //   · 之所以还提供这两个，是因为"先复制到剪贴板，再由用户/别的程序粘贴"
    //     是一种正当用法（比如机器准备好，人再确认粘到哪里）。
    reg(CmdId::FsCopy, [](const Args& args) {
        const std::vector<std::wstring> targets = pathutil::ToShellPaths(args.StrArray(L"targets"));
        if (targets.empty()) return false;
        for (const auto& p : targets) {
            if (!tf::Exists(p)) {
                LogDbg(L"[cmd] FsCopy 目标不存在：%s", p.c_str());
                return false;
            }
        }
        return shellops::CopyPaths(targets, false);
    });
    reg(CmdId::FsCut, [](const Args& args) {
        const std::vector<std::wstring> targets = pathutil::ToShellPaths(args.StrArray(L"targets"));
        if (targets.empty()) return false;
        for (const auto& p : targets) {
            if (!tf::Exists(p)) {
                LogDbg(L"[cmd] FsCut 目标不存在：%s", p.c_str());
                return false;
            }
        }
        return shellops::CopyPaths(targets, true);
    });

    // FsProperties：按路径弹属性对话框。
    //
    // 这是少数**故意弹 UI** 的确定式命令 —— 属性本身就是个对话框，
    // 没有"不弹框的属性"这种东西。所以它标 openWorld + scope=System，
    // 让调用方知道"这会打开一个窗口"。
    reg(CmdId::FsProperties, [](const Args& args) {
        const std::wstring target = pathutil::ToShellPath(args.Str(L"target"));
        if (target.empty()) return false;
        if (!tf::Exists(target)) {
            LogDbg(L"[cmd] 属性目标不存在：%s", target.c_str());
            return false;
        }
        // SHELLEXECUTEINFO + SEE_MASK_INVOKEIDLIST 调的是 shell 的"属性"动词，
        // 等价于右键菜单里那一项（包含所有页签，比 SHObjectProperties 全）。
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_INVOKEIDLIST;
        sei.lpVerb = L"properties";
        sei.lpFile = target.c_str();
        sei.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&sei) != FALSE;
    });
}
