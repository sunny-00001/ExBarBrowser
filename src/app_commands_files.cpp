// 文件操作 / 视图 两组命令。
//
// 共同点：都要"拿当前 IShellView 干活再判成败"，统一走 App::WithView（失败记账 + 响铃）。
#include "app.h"

#include "app_commands.h"
#include "prompt.h"
#include "report.h"
#include "shell_ops.h"
#include "disk_scan.h"
#include "file_hash.h"
#include "file_tools.h"
#include "text_file.h"
#include "view_stats.h"

namespace {

// 选目标文件夹（复制到/移动到）。用 IFileOpenDialog 的 FOS_PICKFOLDERS，
// 与系统一致（支持新建文件夹、地址栏、网络位置），比 SHBrowseForFolder 稳。
bool PickFolder(HWND owner, const std::wstring& start, std::wstring* out) {
    if (!out) return false;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg))) ||
        !dlg)
        return false;
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"选择目标文件夹");
    if (!start.empty()) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_IShellItem,
                                                   reinterpret_cast<void**>(&si))) &&
            si)
            dlg->SetFolder(si.get());
    }
    if (FAILED(dlg->Show(owner))) return false;
    ComPtr<IShellItem> pick;
    if (FAILED(dlg->GetResult(&pick)) || !pick) return false;
    PWSTR p = nullptr;
    if (FAILED(pick->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
    *out = p;
    CoTaskMemFree(p);
    return true;
}

}  // namespace


void App::RegisterFileCommands() {
    auto reg = cmd_reg::Register;
    // 产出结构化结果的命令用 regR（检查脚本按 `regR(CmdId::` 字面扫描"谁注册了谁"，
    // 所以不能写 cmd_reg::RegisterWithResult，也不能靠循环传 id）。
    auto regR = cmd_reg::RegisterWithResult;

    // ---------------- 文件操作 ----------------
    // 返回 fn 的真实成败：以前 `fn(view); return true;` 让删除/粘贴/新建/重命名
    // 的失败完全静默（审计日志还记 ok=1，排查时反而误导）。失败时统一在这里
    // 记日志 + 响一声，命令返回 false 让 Audit 记 ok=0。
    reg(CmdId::CmdCopy, [this](const Args&) {
        return WithView([this](IShellView* v) {
            bool ok = shellops::CopySelection(v, false);
            if (ok) RefreshActive();
            return ok;
        }, L"复制");
    });
    reg(CmdId::CmdCut, [this](const Args&) {
        return WithView([this](IShellView* v) {
            bool ok = shellops::CopySelection(v, true);
            if (ok) RefreshActive();
            return ok;
        }, L"剪切");
    });
    reg(CmdId::CmdPaste, [this](const Args&) {
        return WithView([this](IShellView* v) {
            bool ok = shellops::Paste(v, hwnd_);
            RefreshActive();
            return ok;
        }, L"粘贴");
    });
    reg(CmdId::CmdDelete, [this](const Args&) {
        return WithView([this](IShellView* v) {
            bool ok = shellops::DeleteSelection(v, hwnd_);
            RefreshActive();
            return ok;
        }, L"删除");
    });
    reg(CmdId::CmdRename, [this](const Args&) {
        return WithView([this](IShellView* v) {
            return shellops::InvokeSelectionVerb(v, hwnd_, "rename");
        }, L"重命名");
    });

    // ---- 批量重命名（Ctrl+Shift+R）----
    //
    // 刻意**不做**规则编辑器（查找替换 / 正则 / 大小写 / 日期 / 元数据）：那是
    // PowerRename 的领域，自建一套只能做一半，且解释成本比模式串高一个量级。
    // 这里只提供最小可解释集合：{name} {ext} {old} {n} {nn} {nnn}。
    //
    // 三条安全约束（都由 filetools::PlanBatchRename 保证，可单测）：
    //   1. 先算完整计划、全部校验通过才动手（重名/撞已有文件/非法字符/空名）；
    //   2. 人看到的是一份"旧 → 新"预览，确认后才落盘；
    //   3. 每个改名都走 shellops::RenameTo（FOF_ALLOWUNDO），所以 Ctrl+Z 能整体退回。
    regR(CmdId::BatchRename, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const bool human = args.IsEmpty();

        std::wstring pattern = args.Str(L"pattern");
        if (pattern.empty() && human) {
            // 人的路径：弹框问模式。初值给一个最常见的"原名 + 序号"，直接回车也有用。
            pattern = L"{name}_{nn}";
            if (!PromptText(hwnd_, theme_, L"批量重命名",
                            L"模式：{name} 原名　{ext} 扩展名　{n}/{nn}/{nnn} 序号　{old} 原名全称",
                            pattern, &pattern))
                return false;
        }
        if (str::Trim(pattern).empty()) {
            if (human)
                MessageBoxW(hwnd_, L"模式为空。", id::kBrandShort, MB_ICONINFORMATION);
            return false;
        }

        return WithView([&](IShellView* v) -> bool {
            // 目标：参数点名优先，否则用当前选中项（人的路径）。
            std::vector<std::wstring> targets = args.StrArray(L"targets");
            if (targets.empty()) targets = shellops::SelectedPaths(v);
            if (targets.empty()) {
                CommandBus::Get().NoteFailure(L"没有目标：targets 为空，且当前没有选中项");
                if (human)
                    MessageBoxW(hwnd_, L"先选中要改名的项目。", id::kBrandShort,
                                MB_ICONINFORMATION);
                return false;
            }

            // 目录里现有的全部名字（用视图列表，和用户看到的完全一致）。
            // limit=0 表示不截断：冲突检测少看一个名字就可能覆盖一个真文件。
            shellops::DirListing listing;
            if (!shellops::ListView(v, 0, &listing)) return false;
            std::vector<std::wstring> existing;
            existing.reserve(listing.entries.size());
            for (const auto& e : listing.entries)
                if (!e.name.empty()) existing.push_back(e.name);

            // 只取名字：RenameTo 要的是"名字"，路径由它自己算父目录。
            std::vector<std::wstring> oldNames;
            oldNames.reserve(targets.size());
            for (const auto& t : targets) {
                const size_t slash = t.find_last_of(L"\\/");
                oldNames.push_back(slash == std::wstring::npos ? t : t.substr(slash + 1));
            }

            std::vector<filetools::RenamePair> plan;
            unsigned same = 0;
            std::wstring err;
            if (!filetools::PlanBatchRename(oldNames, existing, pattern, &plan, &same, &err)) {
                LogDbg(L"[cmd] file.rename.batch 计划失败：%s", err.c_str());
                CommandBus::Get().NoteFailure(err);   // 具体到"哪一条冲突/哪个字符非法"
                if (human)
                    MessageBoxW(hwnd_, (L"没有改名：\r\n" + err).c_str(), id::kBrandShort,
                                MB_ICONWARNING);
                return false;
            }

            if (human) {
                // 预览最多 20 行：确认框不是给人读完整清单的，是给人判断"方向对不对"。
                std::wstring preview;
                size_t shown = 0;
                for (const auto& p : plan) {
                    if (shown++ >= 20) {
                        preview += L"…（共 " + std::to_wstring(plan.size()) + L" 项）\r\n";
                        break;
                    }
                    preview += p.from + L"  →  " + p.to + L"\r\n";
                }
                const std::wstring msg = L"即将改名 " + std::to_wstring(plan.size()) + L" 项" +
                                         (same ? L"（" + std::to_wstring(same) + L" 项不变）" : L"") +
                                         L"：\r\n\r\n" + preview + L"\r\n改名可用 Ctrl+Z 撤销。";
                if (MessageBoxW(hwnd_, msg.c_str(), L"批量重命名",
                                MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return false;
            }

            json::Value renamed = json::Value::MakeArray();
            json::Value failures = json::Value::MakeArray();
            std::wstring dir = ViewFolderPath(v);
            unsigned okCount = 0;
            for (const auto& p : plan) {
                const std::wstring from = dir.empty() ? p.from : tf::Join(dir, p.from);
                if (shellops::RenameTo(v, hwnd_, from, p.to)) {
                    ++okCount;
                    json::Value o = json::Value::MakeObject();
                    o.Set(L"from", json::Value::MakeStr(p.from));
                    o.Set(L"to", json::Value::MakeStr(p.to));
                    renamed.arr.push_back(std::move(o));
                } else {
                    failures.arr.push_back(json::Value::MakeStr(p.from));
                }
            }
            if (okCount) RefreshActive();

            result->Set(L"pattern", json::Value::MakeStr(pattern));
            result->Set(L"renamed", renamed);
            result->Set(L"failed", failures);
            result->Set(L"skippedSame", json::Value::MakeNum(static_cast<double>(same)));

            if (failures.arr.empty()) return true;
            LogDbg(L"[cmd] file.rename.batch：%u 项失败", static_cast<unsigned>(failures.arr.size()));
            if (human) {
                MessageBoxW(hwnd_,
                            (L"改了 " + std::to_wstring(okCount) + L" 项，" +
                             std::to_wstring(failures.arr.size()) +
                             L" 项失败（可能被其他程序占用）。\r\n改成功的部分可用 Ctrl+Z 撤销。")
                                .c_str(),
                            L"批量重命名", MB_ICONWARNING);
            }
            // 部分失败必须如实报失败，不能因为"改了一些"就算成功
            CommandBus::Get().NoteFailure(L"改了 " + std::to_wstring(okCount) + L" 项，" +
                                          std::to_wstring(failures.arr.size()) +
                                          L" 项失败（被占用/权限不足）");
            return false;
        }, L"批量重命名");
    });
    reg(CmdId::CmdProps, [this](const Args&) {
        return WithView([this](IShellView* v) {
            return shellops::InvokeSelectionVerb(v, hwnd_, "properties") ||
                   shellops::InvokeFolderVerb(v, hwnd_, "properties");
        }, L"属性");
    });

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
        const std::wstring target = args.Str(L"target");
        const std::wstring newName = args.Str(L"newName");
        if (target.empty() || newName.empty()) return false;
        return WithView([&](IShellView* v) {
            bool ok = shellops::RenameTo(v, hwnd_, target, newName);
            if (ok) RefreshActive();
            return ok;
        }, L"重命名");
    });

    reg(CmdId::FsDelete, [this](const Args& args) {
        const std::vector<std::wstring> targets = args.StrArray(L"targets");
        if (targets.empty()) return false;
        return WithView([&](IShellView* v) {
            // 一次选中全部目标、一次删除。
            //
            // 早先是"选一个删一个"的循环，问题有三：N 次触发视图刷新、
            // N 次构造 IFileOperation（每次都重新弹回收站确认流程）、
            // 中途失败会留下"删了一半"且说不清删了哪些。
            // 一次操作则让系统的冲突/进度处理只走一轮。
            const int hit = shellops::SelectByPaths(v, targets);
            if (hit == 0) return false;
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
        const std::wstring path = args.Str(L"path");
        if (path.empty()) return false;

        // 路径不存在就明确失败，不要交给 ShellExecute（它会弹"找不到"的系统框，
        // 那是交互式行为，确定式调用不该卡在没人看的对话框上）。
        if (!tf::Exists(path)) {
            LogDbg(L"[cmd] FsOpen 目标不存在：%s", path.c_str());
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
        const std::wstring path = args.Str(L"path");
        if (path.empty() || !tf::Exists(path)) {
            if (!path.empty()) LogDbg(L"[cmd] FsOpenInNewTab 目标不存在：%s", path.c_str());
            return false;
        }
        return NewTab(path) != 0;
    });
    reg(CmdId::FsOpenInNewWindow, [this](const Args& args) {
        const std::wstring path = args.Str(L"path");
        if (path.empty() || !tf::Exists(path)) {
            if (!path.empty()) LogDbg(L"[cmd] FsOpenInNewWindow 目标不存在：%s", path.c_str());
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
        const std::vector<std::wstring> targets = args.StrArray(L"targets");
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
        const std::vector<std::wstring> targets = args.StrArray(L"targets");
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
        const std::wstring target = args.Str(L"target");
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
    reg(CmdId::CmdOpen, [this](const Args&) {
        return WithView([this](IShellView* v) {
            // 文件夹在当前标签内进入（explorer 语义），文件才交给 shell 打开
            std::wstring folder;
            if (shellops::FirstFolderSelection(v, &folder) && !folder.empty()) {
                NavigateActive(folder, true);
                return true;
            }
            return shellops::InvokeSelectionVerb(v, hwnd_, "open");
        }, L"打开");
    });
    reg(CmdId::CmdSelectAll, [this](const Args&) {
        return WithView([this](IShellView* v) {
            bool ok = shellops::SelectAll(v);
            RefreshStats();
            return ok;
        }, L"全选");
    });

    // 聚焦搜索框（Ctrl+F）。搜索框此前不存在，所以这条命令无处可去 ——
    // 现在它把光标带到导航栏右侧的搜索框里（而不是"假装做了点什么"）。
    reg(CmdId::FocusSearch, [this](const Args&) {
        navBar_.FocusSearch();
        return true;
    });

    // 撤销（Ctrl+Z）：转发给 shell 视图自己的撤销栈。
    // 本工程所有落盘操作都带 FOF_ALLOWUNDO，已经进了系统栈 —— 自己再记一份会错位。
    reg(CmdId::CmdUndo, [this](const Args&) {
        return WithView([this](IShellView* v) {
            const bool ok = shellops::Undo(v);
            if (ok) RefreshActive();   // 撤销后的列表必须立刻反映出来
            return ok;
        }, L"撤销");
    });

    // ---- 选择族（Ctrl+I 反选 / 只选文件 / 只选文件夹 / 按模式选）----
    //
    // 这四条此前 implemented=false，而 keymap.json 里都已经绑了键 ——
    // 按下去既不触发也不报错（InputRouter 对未实现命令直接 continue）。
    // 用户已经按了很多次"没反应"，却没有任何线索说明是键位错了还是功能没做。
    //
    // 语义统一交给 shellops::SelectWhere / SelectByPattern（枚举与选中各只有一份），
    // 这里只表达"选什么"。选中后都要 RefreshStats：状态栏的"已选 N"必须立刻准。
    reg(CmdId::SelectInvert, [this](const Args&) {
        return WithView([this](IShellView* v) {
            // 反选 = 当前未选中的全都选上。判据用 SelectedPaths 的集合，
            // 而不是"再点一次 SVSI_SELECT"（那样等于全选，用户会觉得命令没区别）。
            const std::vector<std::wstring> cur = shellops::SelectedPaths(v);
            int n = shellops::SelectWhere(
                v,
                [&](const shellops::DirEntry& e) {
                    if (e.path.empty()) return false;
                    for (const auto& p : cur)
                        if (str::IEquals(p, e.path)) return false;   // 已选中 -> 反掉
                    return true;
                },
                shellops::SelectMode::Replace);
            // 一项都没选上不是错误：目录里全都被选中时"反选"的结果就是空。
            // 但要让用户看到状态变化（状态栏已选数归零），所以照样刷。
            RefreshStats();
            (void)n;
            return true;
        }, L"反向选择");
    });
    reg(CmdId::SelectFiles, [this](const Args&) {
        return WithView([this](IShellView* v) {
            const int n = shellops::SelectWhere(v, [](const shellops::DirEntry& e) {
                return !e.isFolder && !e.path.empty();
            });
            RefreshStats();
            // 一个文件都没有时必须如实失败（"只选文件"在全是目录的目录里做不到）。
            return n > 0;
        }, L"只选文件");
    });
    reg(CmdId::SelectFolders, [this](const Args&) {
        return WithView([this](IShellView* v) {
            const int n = shellops::SelectWhere(v, [](const shellops::DirEntry& e) {
                return e.isFolder && !e.path.empty();
            });
            RefreshStats();
            return n > 0;
        }, L"只选文件夹");
    });
    reg(CmdId::SelectByPattern, [this](const Args& args) {
        // 参数优先（命令面板/AI 可以点名模式）；没给才弹输入框。
        std::wstring pattern = args.Str(L"pattern");
        if (pattern.empty()) {
            // 交互式入口：给一个提示性初值，用户直接改。
            // 这是"人的路径"—— 与 Ctrl+Shift+K 一致（R20：同一条命令两种调用方式）。
            pattern = L"*.txt";
            if (!PromptText(hwnd_, theme_, L"按模式选择", L"匹配模式（分号分隔多个）", pattern,
                            &pattern))
                return false;
            if (str::Trim(pattern).empty()) return false;
        }
        shellops::PatternOptions opt;
        opt.matchFolders = args.Bool(L"folders", false);
        opt.caseSensitive = args.Bool(L"caseSensitive", false);
        return WithView([this, &pattern, &opt](IShellView* v) {
            const int n = shellops::SelectByPattern(v, pattern, opt);
            RefreshStats();
            return n > 0;   // 没匹配到要如实报 false，不能假装成功
        }, L"按模式选择");
    });
    reg(CmdId::SelectByType, [this](const Args&) {
        // 按类型 = "和第一个选中项同扩展名的都选上"（Total Commander 的 Num+ 口径）。
        // 不取"所有选中项类型的并集"：那样选了两三种之后点一次就等于全选，
        // 命令会退化成"全选"，用户第二次就会觉得它坏了。
        return WithView([this](IShellView* v) {
            const std::vector<std::wstring> cur = shellops::SelectedPaths(v);
            if (cur.empty()) {
                MessageBoxW(hwnd_, L"先选中一个文件作为样板（按它的扩展名选同类）。",
                            id::kBrandShort, MB_ICONINFORMATION);
                return false;
            }
            const std::wstring& sample = cur.front();
            const size_t slash = sample.find_last_of(L"\\/");
            const std::wstring base =
                slash == std::wstring::npos ? sample : sample.substr(slash + 1);
            const size_t dot = base.rfind(L'.');
            if (dot == std::wstring::npos || dot == 0) {
                MessageBoxW(hwnd_,
                            (L"“" + base + L"”没有扩展名，无法按类型选择。").c_str(),
                            id::kBrandShort, MB_ICONINFORMATION);
                return false;
            }
            const std::wstring ext = str::Lower(base.substr(dot));

            const int n = shellops::SelectWhere(v, [&ext](const shellops::DirEntry& e) {
                if (e.isFolder || e.name.empty()) return false;
                const size_t d = e.name.rfind(L'.');
                if (d == std::wstring::npos || d == 0) return false;
                return str::Lower(e.name.substr(d)) == ext;
            });
            RefreshStats();
            return n > 0;   // 只有样板本身时也算成功（它已经在选中里）
        }, L"按类型选择");
    });
    reg(CmdId::CmdNewFolder, [this](const Args&) {        return WithView([this](IShellView* v) {
            bool ok = shellops::NewFolder(v, hwnd_);
            if (ok) RefreshActive();
            return ok;
        }, L"新建文件夹");
    });
    reg(CmdId::CmdNewTextFile, [this](const Args&) {
        return WithView([this](IShellView* v) {
            std::wstring created;
            bool ok = shellops::NewTextFile(v, hwnd_, &created);
            if (ok) RefreshActive();
            return ok;
        }, L"新建文本文件");
    });
    reg(CmdId::CmdOpenWith, [this](const Args&) {
        auto paths = SelectedOrFolder();
        if (paths.empty()) return false;
        std::wstring args = L"shell32.dll,OpenAs_RunDLL \"" + paths.front() + L"\"";
        ShellExecuteW(hwnd_, L"open", L"rundll32.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    });
    // 复制到 / 移动到：选中项 + 目标文件夹（IFileOperation，撤销/冲突交给系统）
    auto transferTo = [this](bool move) {
        IShellView* view = ActiveView();
        if (!view) return false;
        std::wstring target;
        bool ok = PickFolder(hwnd_, ActivePath(), &target) &&
                  shellops::TransferToFolder(view, hwnd_, target, move);
        view->Release();
        if (ok) RefreshActive();
        return ok;
    };
    reg(CmdId::CopyToFolder, [transferTo](const Args&) { return transferTo(false); });
    reg(CmdId::MoveToFolder, [transferTo](const Args&) { return transferTo(true); });
    reg(CmdId::CmdOpenInNewTab, [this](const Args&) {
        auto paths = SelectedOrFolder();
        if (paths.empty()) return false;
        NewTab(paths.front(), NewTabIndex());
        return true;
    });
    reg(CmdId::CmdOpenInNewWindow, [this](const Args&) {
        auto paths = SelectedOrFolder();
        if (!paths.empty()) OpenPathInNewWindow(paths.front());
        return true;
    });
    reg(CmdId::FileFocusTop, [this](const Args&) {
        return WithView([](IShellView* v) { return shellops::SelectIndex(v, 0); }, L"定位首项");
    });
    reg(CmdId::FileFocusBottom, [this](const Args&) {
        return WithView([](IShellView* v) {
            ViewStats st;
            if (CollectViewStats(v, &st) && st.total > 0)
                return shellops::SelectIndex(v, static_cast<int>(st.total) - 1);
            return false;
        }, L"定位末项");
    });

    // ---------------- 视图 ----------------
    reg(CmdId::ViewMode, [this](const Args& args) {
        ApplyViewMode(args.Int(L"preset", viewMode_));
        return true;
    });
    reg(CmdId::ViewNext, [this](const Args&) {
        ApplyViewMode((viewMode_ + 1) % static_cast<int>(ViewModeCount()));
        return true;
    });
    reg(CmdId::ViewPrev, [this](const Args&) {
        int n = static_cast<int>(ViewModeCount());
        ApplyViewMode((viewMode_ - 1 + n) % n);
        return true;
    });
    reg(CmdId::ViewSort, [this](const Args& args) {
        int mode = args.Int(L"by");
        // 交互语义：对同一列再点一次就反向（由 SetSort 的 ascending=-1 表达）。
        const bool ok = WithView([mode](IShellView* v) { return shellops::SetSort(v, mode); },
                                 L"排序");
        if (!ok) return false;

        // 执行后从视图读回**真实**方向再落盘：交互时方向是 SetSort 内部推导的，
        // 这里不读回来就永远记不到降序。
        Settings& s = Config::Get().mutableSettings();
        bool changed = false;
        if (s.sortMode != mode) {
            s.sortMode = mode;
            changed = true;
        }
        if (IShellView* v = ActiveView()) {
            int gotMode = -1;
            bool gotAsc = true;
            if (shellops::GetSort(v, &gotMode, &gotAsc)) {
                if (s.sortAscending != gotAsc) {
                    s.sortAscending = gotAsc;
                    changed = true;
                }
            }
            v->Release();
        }
        if (changed) Config::Get().Save();
        return true;
    });
    reg(CmdId::ViewGroup, [this](const Args& args) {
        int mode = args.Int(L"by");
        // 先执行、成功后再落盘：反过来写会在 SetGroup 失败时留下
        // "配置说已分组、实际没分组"的分歧，下次启动又按错的状态恢复。
        if (!WithView([mode](IShellView* v) { return shellops::SetGroup(v, mode); }, L"分组"))
            return false;
        Settings& s = Config::Get().mutableSettings();
        if (s.groupBy != mode) {
            s.groupBy = mode;
            Config::Get().Save();
        }
        return true;
    });
    reg(CmdId::ToggleHidden, [this](const Args&) {
        return WithView([this](IShellView* v) {
            bool ok = shellops::ToggleHidden(v, 0, hwnd_);
            RefreshStats();
            return ok;
        }, L"隐藏文件");
    });
    reg(CmdId::ToggleSystemFiles, [this](const Args&) {
        return WithView([this](IShellView* v) {
            bool ok = shellops::ToggleHidden(v, 1, hwnd_);
            RefreshStats();
            return ok;
        }, L"系统文件");
    });
    reg(CmdId::ToggleExtensions, [this](const Args&) {
        return WithView([this](IShellView* v) {
            bool ok = shellops::ToggleHidden(v, 2, hwnd_);
            RefreshStats();
            return ok;
        }, L"扩展名");
    });
    reg(CmdId::CycleHiddenSeg, [this](const Args&) {
        return WithView([this](IShellView* v) {
            shellops::CycleHiddenState(v, hwnd_);
            RefreshStats();
            return true;
        }, L"隐藏状态三态");
    });
    reg(CmdId::TogglePreviewPane, [this](const Args&) {
        Tab* t = Active();
        if (!t || !t->view) return false;
        t->view->TogglePreviewPane();
        RefreshStats();
        return true;
    });
    reg(CmdId::ToggleNavPane, [this](const Args&) {
        Tab* t = Active();
        if (!t || !t->view) return false;
        t->view->ToggleNavPane();
        RefreshStats();
        return true;
    });

    // ---------------- 只读查询（P6：读操作返回结构化结果）----------------
    //
    // 这两条是第一批带 outputSchema 的命令。它们证明"命令能返回数据"这条路
    // 走得通 —— 以前 handler 只返回 bool，所以"列目录"这类需求只能另起一套
    // 并行的查询 API，破坏"命令是唯一契约"（P1）。
    regR(CmdId::OpListDir, [this](const Args& args, json::Value* result) {
        Tab* t = Active();
        if (!t || !t->view || !result) return false;

        shellops::DirListing listing;
        const int limit = args.Int(L"limit", 0);
        // ActiveView() 返回已 AddRef 的指针，必须 Release —— 否则每列一次目录漏一个引用。
        IShellView* view = ActiveView();
        if (!view) return false;
        const bool ok = shellops::ListView(view, limit, &listing);
        view->Release();
        if (!ok) return false;

        result->Set(L"path", json::Value::MakeStr(listing.path));
        result->Set(L"truncated", json::Value::MakeBool(listing.truncated));
        json::Value arr = json::Value::MakeArray();
        for (const auto& e : listing.entries) {
            json::Value o = json::Value::MakeObject();
            o.Set(L"name", json::Value::MakeStr(e.name));
            o.Set(L"path", json::Value::MakeStr(e.path));
            o.Set(L"isFolder", json::Value::MakeBool(e.isFolder));
            o.Set(L"hidden", json::Value::MakeBool(e.hidden));
            // 用 MakeNum 而不是 MakeInt：文件常超过 2 GB，强转 int 会变负数。
            if (e.sizeKnown) o.Set(L"size", json::Value::MakeNum(static_cast<double>(e.size)));
            arr.arr.push_back(std::move(o));
        }
        result->Set(L"entries", arr);
        return true;
    });

    regR(CmdId::OpStat, [this](const Args& args, json::Value* result) {
        const std::wstring path = args.Str(L"path");
        if (path.empty() || !result) return false;

        shellops::ItemInfo info;
        IShellView* view = ActiveView();
        if (!view) return false;
        const bool ok = shellops::StatItem(view, path, &info);
        view->Release();
        if (!ok) return false;

        result->Set(L"name", json::Value::MakeStr(info.name));
        result->Set(L"path", json::Value::MakeStr(info.path));
        result->Set(L"isFolder", json::Value::MakeBool(info.isFolder));
        result->Set(L"hidden", json::Value::MakeBool(info.hidden));
        result->Set(L"readonly", json::Value::MakeBool(info.readonly_));
        result->Set(L"typeName", json::Value::MakeStr(info.typeName));
        // 同上：大文件不能强转 int（会溢出成负数）。
        if (info.sizeKnown)
            result->Set(L"size", json::Value::MakeNum(static_cast<double>(info.size)));
        return true;
    });

    // ---- OpTopN / OpDiskUsage：目录体积统计 ----
    //
    // 两条共用 diskscan::Scan，区别只在语义：
    //   OpTopN       —— "这个目录里什么最占空间"，答的是内容本身
    //   OpDiskUsage  —— "这个目录占多少"，答的是总量
    // 之所以分成两条而不是一条带 mode：它们的**主要问题不同**，
    // 调用方关心的是不同的字段（前者看 entries，后者看 totalBytes）。
    // 参照 du（总量）与 ncdu（占比排序）在 Unix 上也是两个工具。
    //
    // 结果里必带 truncated / timedOut / elapsedMs 三个"诚实字段"：
    // 只给数字而不说"没算完"，调用方会把不完整值当真实值用。
    // 注意：这里写成两个显式的 regR(CmdId::X, ...) 而不是循环/工厂。
    // 静态检查（check_ai_meta.py 第 3/10 项）按 `regR(CmdId::名字` 的字面形式
    // 扫描"谁注册了谁"，用变量传 id 就扫不到，会误报"声明了参数却没取用"。
    // 换句话说：字面注册让检查能覆盖到，这比少写两行代码更重要。
    auto scanImpl = [this](const Args& args, json::Value* result, bool wantEntries) {
            if (!result) return false;
            const std::wstring path = args.Str(L"path");
            diskscan::Options opt;
            if (args.Has(L"limit")) opt.limit = args.Int(L"limit", 50);
            if (args.Has(L"budgetMs")) {
                const int ms = args.Int(L"budgetMs", 1500);
                // 夹到合理区间：太小必然没结果，太大会让人以为程序卡死
                opt.budgetMs = static_cast<unsigned long long>(
                    ms < 100 ? 100 : (ms > 30000 ? 30000 : ms));
            }

            diskscan::Result r;
            std::wstring err;
            // 没指定路径就统计当前目录 —— 这是最常见的用法（"这里什么最占空间"）。
            // 注意 ActiveView() 是 AddRef 过的，必须 Release，否则每调一次漏一个引用。
            std::wstring target = path;
            if (target.empty()) {
                if (IShellView* v = ActiveView()) {
                    target = ViewFolderPath(v);
                    v->Release();
                }
            }
            if (target.empty()) {
                LogDbg(L"[cmd] 扫描命令：拿不到当前目录");
                return false;
            }
            if (!diskscan::Scan(target, opt, &r, &err)) {
                LogDbg(L"[cmd] diskscan 失败：%s", err.c_str());
                return false;
            }

            result->Set(L"path", json::Value::MakeStr(r.path));
            result->Set(L"totalBytes", json::Value::MakeNum(static_cast<double>(r.totalBytes)));
            result->Set(L"truncated", json::Value::MakeBool(r.truncated));
            result->Set(L"timedOut", json::Value::MakeBool(r.timedOut));
            result->Set(L"elapsedMs", json::Value::MakeNum(static_cast<double>(r.elapsedMs)));
            result->Set(L"scannedFiles",
                        json::Value::MakeNum(static_cast<double>(r.scannedFiles)));
            result->Set(L"scannedDirs",
                        json::Value::MakeNum(static_cast<double>(r.scannedDirs)));
            result->Set(L"deniedDirs",
                        json::Value::MakeNum(static_cast<double>(r.deniedDirs)));
            result->Set(L"complete", json::Value::MakeBool(r.complete));

            json::Value arr = json::Value::MakeArray();
            for (const auto& e : r.entries) {
                json::Value o = json::Value::MakeObject();
                o.Set(L"name", json::Value::MakeStr(e.name));
                o.Set(L"path", json::Value::MakeStr(e.path));
                o.Set(L"bytes", json::Value::MakeNum(static_cast<double>(e.bytes)));
                o.Set(L"isFolder", json::Value::MakeBool(e.isFolder));
                o.Set(L"items", json::Value::MakeNum(static_cast<double>(e.items)));
                o.Set(L"complete", json::Value::MakeBool(e.complete));
                arr.arr.push_back(std::move(o));
            }
            result->Set(L"entries", arr);
            return true;
    };
    regR(CmdId::OpTopN, [scanImpl](const Args& a, json::Value* r) {
        return scanImpl(a, r, true);
    });
    regR(CmdId::OpDiskUsage, [scanImpl](const Args& a, json::Value* r) {
        return scanImpl(a, r, false);
    });

    // ---- FileHash / DupFinder / FolderCompare：文件集工具 ----
    //
    // 三条共用同一套设计：
    //   人工路径 —— **不带参数**调用（命令面板 / 热键）：目标取界面现状
    //               （哈希取选中项、查重取当前目录、比较弹两个路径框），
    //               结果用 ShowReport 弹只读文本报告；
    //   agent 路径 —— 带参数调用：返回结构化 JSON，**不弹窗**（没人看着屏幕）。
    // 判据是 args.IsEmpty() 而不是"有没有某个键"：命令面板与热键走的正是空参数。
    // 副作用一致（都只读），所以不需要两套命令 id。
    regR(CmdId::FileHash, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const bool human = args.IsEmpty();

        filehash::Algo algo = filehash::Algo::Sha256;
        if (args.Has(L"algorithm") && !filehash::ParseAlgo(args.Str(L"algorithm"), &algo)) {
            // 认不出的算法：失败，不静默退回 SHA-256。否则"我算的是 SHA-512"
            // 与实际结果无法区分 —— 拿错算法去核对下载包会得出错误结论。
            LogDbg(L"[cmd] tool.hash：认不出的算法 %s", args.Str(L"algorithm").c_str());
            // 原因要带给调用方：名字打错和"值超范围"该做的下一步完全不同。
            CommandBus::Get().NoteFailure(L"不支持的算法：" + args.Str(L"algorithm") +
                                          L"（可用 md5 / sha1 / sha256）");
            return false;
        }

        std::vector<std::wstring> paths = args.StrArray(L"paths");
        if (paths.empty()) {
            // 无参：当前选中项。这是人最自然的动作（选中 → 算摘要）。
            if (IShellView* v = ActiveView()) {
                paths = shellops::SelectedPaths(v);
                v->Release();
            }
        }
        if (paths.empty()) {
            CommandBus::Get().NoteFailure(L"没有目标：paths 为空，且当前没有选中项");
            if (human)
                MessageBoxW(hwnd_, L"先在文件列表里选中要计算摘要的文件。", id::kBrandShort,
                            MB_ICONINFORMATION);
            return false;
        }

        json::Value arr = json::Value::MakeArray();
        std::wstring report;
        unsigned okCount = 0, failCount = 0;
        std::wstring failures;
        for (const auto& p : paths) {
            filehash::Result hr;
            const bool ok = filehash::HashFile(p, algo, &hr);
            if (ok) {
                ++okCount;
                report += hr.hex + L"  " + FormatSize(hr.bytes) + L"  " + p + L"\r\n";
            } else {
                ++failCount;
                failures += L"  " + p + L"  —  " + hr.error + L"\r\n";
            }
            json::Value o = json::Value::MakeObject();
            o.Set(L"path", json::Value::MakeStr(p));
            o.Set(L"algorithm", json::Value::MakeStr(filehash::AlgoName(algo)));
            o.Set(L"ok", json::Value::MakeBool(ok));
            if (ok) {
                o.Set(L"hash", json::Value::MakeStr(hr.hex));
                o.Set(L"bytes", json::Value::MakeNum(static_cast<double>(hr.bytes)));
            } else {
                o.Set(L"error", json::Value::MakeStr(hr.error));
            }
            arr.arr.push_back(std::move(o));
        }

        result->Set(L"algorithm", json::Value::MakeStr(filehash::AlgoName(algo)));
        result->Set(L"entries", arr);

        if (human) {
            std::wstring text = std::wstring(filehash::AlgoName(algo)) + L"　成功 " +
                                std::to_wstring(okCount) + L" 个";
            if (failCount) text += L"，失败 " + std::to_wstring(failCount) + L" 个";
            text += L"\r\n\r\n" + report;
            if (failCount) text += L"\r\n读取失败：\r\n" + failures;
            ShowReport(hwnd_, theme_, L"文件摘要", text);
        }
        return okCount > 0;
    });

    regR(CmdId::DupFinder, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const bool human = args.IsEmpty();

        filetools::Options opt;
        if (args.Has(L"budgetMs")) {
            const int ms = args.Int(L"budgetMs", 1500);
            // 夹到合理区间（同 diskscan）：太小必然没结果，太大等于让 UI 卡住。
            opt.budgetMs = static_cast<unsigned long long>(ms < 100 ? 100 : (ms > 30000 ? 30000 : ms));
        }
        if (args.Has(L"minBytes")) {
            const int n = args.Int(L"minBytes", 1);
            opt.minBytes = n <= 0 ? 0ULL : static_cast<unsigned long long>(n);
        }

        std::wstring root = args.Str(L"path");
        if (root.empty()) {
            if (IShellView* v = ActiveView()) {
                root = ViewFolderPath(v);
                v->Release();
            }
        }
        if (root.empty()) {
            CommandBus::Get().NoteFailure(L"没有目录：path 为空，且当前视图没有文件系统路径");
            if (human)
                MessageBoxW(hwnd_, L"拿不到当前目录（虚拟位置没有文件系统路径）。",
                            id::kBrandShort, MB_ICONINFORMATION);
            return false;
        }

        filetools::DupResult r;
        std::wstring err;
        if (!filetools::FindDuplicates(root, opt, &r, &err)) {
            LogDbg(L"[cmd] tool.dupfinder 失败：%s", err.c_str());
            CommandBus::Get().NoteFailure(L"查找重复文件失败：" + err);
            if (human)
                MessageBoxW(hwnd_, (L"查找重复文件失败：\r\n" + err).c_str(), id::kBrandShort,
                            MB_ICONWARNING);
            return false;
        }

        // 报告里**先讲完整性再讲结论**：把"扫了多少、停在哪"写在数字前面，
        // 否则"没有重复"会被读成"这个目录很干净"，而实际可能是预算用完了。
        std::wstring head = L"目录：" + r.root + L"\r\n";
        head += L"扫描 " + std::to_wstring(r.scannedFiles) + L" 个文件，实际哈希 " +
                std::to_wstring(r.hashedFiles) + L" 个，用时 " +
                std::to_wstring(r.elapsedMs) + L" ms";
        head += r.timedOut ? L"（**未扫完**：预算用完，结果偏少）" : L"（已扫完）";
        if (r.deniedDirs)
            head += L"\r\n有 " + std::to_wstring(r.deniedDirs) + L" 个目录打不开（结果偏少）";
        head += L"\r\n可回收 " + FormatSize(r.wastedBytes) + L"，共 " +
                std::to_wstring(r.groups.size()) + L" 组\r\n";

        json::Value groups = json::Value::MakeArray();
        std::wstring body;
        constexpr size_t kMaxShown = 200;
        size_t shown = 0;
        for (const auto& g : r.groups) {
            if (shown < kMaxShown) {
                const unsigned long long wasted = g.bytes * (g.files.size() - 1);
                body += L"\r\n组 " + std::to_wstring(shown + 1) + L"：" +
                        std::to_wstring(g.files.size()) + L" 份 × " + FormatSize(g.bytes) +
                        L" = 可回收 " + FormatSize(wasted) + L"\r\n  SHA-256 " + g.hash + L"\r\n";
                for (const auto& f : g.files) body += L"  " + f.path + L"\r\n";
                ++shown;
            }
            json::Value go = json::Value::MakeObject();
            go.Set(L"bytes", json::Value::MakeNum(static_cast<double>(g.bytes)));
            go.Set(L"hash", json::Value::MakeStr(g.hash));
            json::Value files = json::Value::MakeArray();
            for (const auto& f : g.files) {
                json::Value fo = json::Value::MakeObject();
                fo.Set(L"path", json::Value::MakeStr(f.path));
                fo.Set(L"bytes", json::Value::MakeNum(static_cast<double>(f.bytes)));
                files.arr.push_back(std::move(fo));
            }
            go.Set(L"files", files);
            groups.arr.push_back(std::move(go));
        }
        if (r.groups.size() > kMaxShown)
            body += L"\r\n（仅显示前 " + std::to_wstring(kMaxShown) + L" 组）\r\n";

        result->Set(L"path", json::Value::MakeStr(r.root));
        result->Set(L"groups", groups);
        result->Set(L"wastedBytes", json::Value::MakeNum(static_cast<double>(r.wastedBytes)));
        result->Set(L"scannedFiles", json::Value::MakeNum(static_cast<double>(r.scannedFiles)));
        result->Set(L"hashedFiles", json::Value::MakeNum(static_cast<double>(r.hashedFiles)));
        result->Set(L"deniedDirs", json::Value::MakeNum(static_cast<double>(r.deniedDirs)));
        result->Set(L"timedOut", json::Value::MakeBool(r.timedOut));
        result->Set(L"complete", json::Value::MakeBool(r.complete));
        result->Set(L"elapsedMs", json::Value::MakeNum(static_cast<double>(r.elapsedMs)));

        if (human) ShowReport(hwnd_, theme_, L"查找重复文件", head + body);
        return true;
    });

    regR(CmdId::FolderCompare, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const bool human = args.IsEmpty();

        std::wstring left = args.Str(L"left");
        std::wstring right = args.Str(L"right");
        if ((left.empty() || right.empty()) && human) {
            // 人没有"两个参数"这回事：给一个两框输入，左侧预填当前目录。
            if (left.empty()) {
                if (IShellView* v = ActiveView()) {
                    left = ViewFolderPath(v);
                    v->Release();
                }
            }
            if (!PromptText2(hwnd_, theme_, L"比较文件夹", L"左侧（当前目录）", left, &left,
                             L"右侧（要对比的目录）", right, &right))
                return false;
        }
        if (left.empty() || right.empty()) {
            CommandBus::Get().NoteFailure(L"两个目录都必须给：left / right");
            return false;
        }

        filetools::CompareOptions opt;
        if (args.Has(L"byContent")) opt.byContent = args.Bool(L"byContent", true);
        if (args.Has(L"budgetMs")) {
            const int ms = args.Int(L"budgetMs", 1500);
            opt.walk.budgetMs =
                static_cast<unsigned long long>(ms < 100 ? 100 : (ms > 30000 ? 30000 : ms));
        }

        filetools::CompareResult r;
        std::wstring err;
        if (!filetools::CompareFolders(left, right, opt, &r, &err)) {
            LogDbg(L"[cmd] tool.compare 失败：%s", err.c_str());
            CommandBus::Get().NoteFailure(L"比较失败：" + err);
            if (human)
                MessageBoxW(hwnd_, (L"比较失败：\r\n" + err).c_str(), id::kBrandShort,
                            MB_ICONWARNING);
            return false;
        }

        std::wstring head = L"左：" + r.left + L"\r\n右：" + r.right + L"\r\n";
        head += L"相同 " + std::to_wstring(r.sameCount) + L"，内容不同 " +
                std::to_wstring(r.differCount) + L"，仅左 " + std::to_wstring(r.onlyLeftCount) +
                L"，仅右 " + std::to_wstring(r.onlyRightCount) + L"，用时 " +
                std::to_wstring(r.elapsedMs) + L" ms";
        head += r.timedOut ? L"（**未比完**：预算用完）" : L"（已比完）";
        if (r.deniedLeft || r.deniedRight)
            head += L"\r\n有目录打不开：左 " + std::to_wstring(r.deniedLeft) + L"，右 " +
                    std::to_wstring(r.deniedRight) + L"（结果偏少）";
        head += L"\r\n左合计 " + FormatSize(r.leftBytes) + L"，右合计 " +
                FormatSize(r.rightBytes) + L"\r\n";
        if (!opt.byContent) head += L"\r\n（只比了大小，同大小不同内容会算作“相同”）\r\n";

        json::Value entries = json::Value::MakeArray();
        std::wstring body;
        for (const auto& e : r.entries) {
            const wchar_t* tag = e.state == filetools::Diff::OnlyLeft    ? L"[L] 仅左侧"
                                 : e.state == filetools::Diff::OnlyRight ? L"[R] 仅右侧"
                                                              : L"[M] 内容不同";
            body += std::wstring(tag) + L"  " + FormatSize(e.leftBytes) + L" → " +
                    FormatSize(e.rightBytes) + L"  " + e.rel + L"\r\n";

            json::Value o = json::Value::MakeObject();
            o.Set(L"rel", json::Value::MakeStr(e.rel));
            o.Set(L"leftBytes", json::Value::MakeNum(static_cast<double>(e.leftBytes)));
            o.Set(L"rightBytes", json::Value::MakeNum(static_cast<double>(e.rightBytes)));
            o.Set(L"state", json::Value::MakeStr(e.state == filetools::Diff::OnlyLeft    ? L"onlyLeft"
                                                 : e.state == filetools::Diff::OnlyRight ? L"onlyRight"
                                                                              : L"differ"));
            entries.arr.push_back(std::move(o));
        }
        if (r.truncated)
            body += L"（差异项超过上限，仅列出前 " + std::to_wstring(opt.maxEntries) + L" 条）\r\n";

        result->Set(L"left", json::Value::MakeStr(r.left));
        result->Set(L"right", json::Value::MakeStr(r.right));
        result->Set(L"entries", entries);
        result->Set(L"sameCount", json::Value::MakeNum(static_cast<double>(r.sameCount)));
        result->Set(L"differCount", json::Value::MakeNum(static_cast<double>(r.differCount)));
        result->Set(L"onlyLeftCount", json::Value::MakeNum(static_cast<double>(r.onlyLeftCount)));
        result->Set(L"onlyRightCount", json::Value::MakeNum(static_cast<double>(r.onlyRightCount)));
        result->Set(L"timedOut", json::Value::MakeBool(r.timedOut));
        result->Set(L"complete", json::Value::MakeBool(r.complete));
        result->Set(L"elapsedMs", json::Value::MakeNum(static_cast<double>(r.elapsedMs)));

        if (human) ShowReport(hwnd_, theme_, L"比较文件夹", head + L"\r\n" + body);
        return true;
    });
}
