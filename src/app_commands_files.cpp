// 文件操作 / 视图 两组命令。
//
// 共同点：都要"拿当前 IShellView 干活再判成败"，统一走 App::WithView（失败记账 + 响铃）。
#include "app.h"

#include "app_commands.h"
#include "menu.h"           // 选择详细信息列的勾选菜单
#include "prompt.h"         // PromptText —— 批量改名/按模式选择的交互式输入
#include "search_url.h"     // searchurl::Trim —— 与导航栏搜索框共用同一套规范化
#include "shell_columns.h"  // IColumnManager 的唯一封装
#include "shell_ops.h"
#include "view_modes.h"     // 图标尺寸步进与边界
#include "file_tools.h"
#include "file_rename_plan.h"   // filetools::PlanBatchRename —— 批量改名计划（纯函数）
#include "text_file.h"

namespace {

// 目录清单里的名字集合（用于交互式改名后做差集）。
std::vector<std::wstring> NamesOf(const shellops::DirListing& listing) {
    std::vector<std::wstring> names;
    names.reserve(listing.entries.size());
    for (const auto& e : listing.entries)
        if (!e.name.empty()) names.push_back(e.name);
    return names;
}

// 集合差集：after 里多出来的 = added，before 里多出来的 = removed。
void DiffNames(const std::vector<std::wstring>& before, const std::vector<std::wstring>& after,
               std::vector<std::wstring>* added, std::vector<std::wstring>* removed) {
    if (added) added->clear();
    if (removed) removed->clear();
    for (const auto& n : after) {
        if (std::find(before.begin(), before.end(), n) == before.end() && added)
            added->push_back(n);
    }
    for (const auto& n : before) {
        if (std::find(after.begin(), after.end(), n) == after.end() && removed)
            removed->push_back(n);
    }
}

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
    reg(CmdId::CmdDeletePermanent, [this](const Args&) {
        return WithView([this](IShellView* v) {
            // 不记 fileundo：这条命令刻意绕过回收站/撤销栈，
            // 记一条撤不回去的"撤销记录"只会骗人。
            bool ok = shellops::DeleteSelectionPermanently(v, hwnd_);
            RefreshActive();
            return ok;
        }, L"永久删除");
    });
    reg(CmdId::CmdRename, [this](const Args&) {
        return WithView([this](IShellView* v) {
            // 撤销栈需要知道"旧名 → 新名"。
            // 交互式改名走 shell 的重命名框（InvokeSelectionVerb），新名字
            // 是人在框里敲的，**拿不到** —— 只能在前后各取一次目录清单做差集：
            //   多出来的名字 = 新名；消失的名字 = 旧名。
            // 差集不是 1:1（用户中途改了主意、或清单在改名期间变了）就不记 ——
            // 宁可少一条撤销记录，也不要记一条错的（错的记录撤回去会毁文件）。
            std::wstring oldPath;
            if (!shellops::SelectedSinglePath(v, &oldPath)) return false;
            std::vector<std::wstring> before;
            {
                shellops::DirListing listing;
                if (shellops::ListView(v, 0, &listing)) before = NamesOf(listing);
            }
            const bool ok = shellops::InvokeSelectionVerb(v, hwnd_, "rename");
            if (!ok) return false;
            std::vector<std::wstring> after;
            {
                shellops::DirListing listing;
                if (shellops::ListView(v, 0, &listing)) after = NamesOf(listing);
            }
            std::vector<std::wstring> added, removed;
            DiffNames(before, after, &added, &removed);
            if (added.size() == 1 && removed.size() == 1) {
                const std::wstring dir = ParentOf(oldPath);
                fileundo::Item item;
                item.beforePath = dir.empty() ? removed[0] : tf::Join(dir, removed[0]);
                item.afterPath = dir.empty() ? added[0] : tf::Join(dir, added[0]);
                item.beforeName = removed[0];
                NoteFileOp(fileundo::Op::Rename, {item}, L"重命名");
            } else {
                LogDbg(L"[undo] 交互式改名未能推出新旧名（差集 %zu/%zu），不记撤销记录",
                       added.size(), removed.size());
            }
            return true;
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
        const bool human = IsHumanCall(args);

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
            // 撤销记录：一次批量 = 一条（不是每个文件一条）——
            // 人按一次 Ctrl+Z 期望退掉整批，而不是退一个文件。
            std::vector<fileundo::Item> undoItems;
            for (const auto& p : plan) {
                const std::wstring from = dir.empty() ? p.from : tf::Join(dir, p.from);
                std::wstring why;
                if (shellops::RenameTo(v, hwnd_, from, p.to, &why)) {
                    ++okCount;
                    fileundo::Item item;
                    item.beforePath = from;
                    item.afterPath = dir.empty() ? p.to : tf::Join(dir, p.to);
                    item.beforeName = p.from;
                    undoItems.push_back(std::move(item));
                    json::Value o = json::Value::MakeObject();
                    o.Set(L"from", json::Value::MakeStr(p.from));
                    o.Set(L"to", json::Value::MakeStr(p.to));
                    renamed.arr.push_back(std::move(o));
                } else {
                    // 失败项要带**原因**，不能只报文件名：
                    // "a.txt 失败"对使用者没用 —— 是被占用？权限不足？还是重名？
                    json::Value f = json::Value::MakeObject();
                    f.Set(L"from", json::Value::MakeStr(p.from));
                    f.Set(L"reason", json::Value::MakeStr(why.empty() ? L"未知原因" : why));
                    failures.arr.push_back(std::move(f));
                }
            }
            if (okCount) RefreshActive();
            if (!undoItems.empty()) NoteFileOp(fileundo::Op::Rename, std::move(undoItems),
                                               L"批量重命名");

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
                                          L" 项失败（原因见 failed[].reason）",
                                          FailKind::LockConflict);
            return false;
        }, L"批量重命名");
    });
    reg(CmdId::CmdProps, [this](const Args&) {
        return WithView([this](IShellView* v) {
            return shellops::InvokeSelectionVerb(v, hwnd_, "properties") ||
                   shellops::InvokeFolderVerb(v, hwnd_, "properties");
        }, L"属性");
    });

    // fs.* 这一组按路径操作，单独成文件（app_commands_fs.cpp）：
    // 作用域不同（路径 vs 选中集），混在一起会让两组命令的入参语义对不上号。
    RegisterFsCommands();

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

    // 撤销（Ctrl+Z）：两层，先撤自己的，再落 shell 的。
    //
    // 【为什么要有自己的一层】
    // shell 的撤销谓词只能向视图的背景 IContextMenu 要，而宿主 IExplorerBrowser
    // 的背景菜单里**没有** undo 行（2026-11-21 实测：删除/改名之后这条路恒失败）。
    // 于是我们自己的文件操作（改名/移动/复制）记一份"做过什么"，
    // 撤的时候按逆操作做回去（见 src/file_undo.h）。
    //
    // 【顺序不能反】
    // 先撤自己的栈；栈空（或撤不动）才落 shell 的撤销。
    // 反过来的话，shell 那个"永远撤不掉"的 undo 谓词会把我们自己的记录挡住，
    // 于是"能撤的也撤不掉"。
    //
    // 【边界】机器的撤销不穿透人工操作：栈顶是人工做的，机器撤不了
    //（同 bmk.* 的口径，见 file_undo.h）。
    reg(CmdId::CmdUndo, [this](const Args&) {
        const bool machine = CommandBus::Get().fromMachine();
        if (fileUndo_.CanUndo()) {
            std::wstring why;
            const bool ok = fileUndo_.Undo(
                machine,
                [this](fileundo::Entry& e, std::wstring* w) { return ApplyFileUndo(e, w); },
                &why);
            if (ok) {
                RefreshActive();
                return true;
            }
            CommandBus::Get().NoteFailure(why.empty() ? L"撤销失败" : why);
            // 栈非空但撤不动 → **到此为止，不落回 shell**。
            //
            // 两种情形都必须停在这里：
            //   ① 栈顶是人工操作而调用方是机器 → 让机器去撤 shell 里的
            //      操作 = 穿透人工操作，这正是双门机制要防的。
            //   ② 撤销执行本身失败（文件被占用、权限不足）→ 那条记录**还在栈里**
            //      （失败不弹栈），此时落回 shell 会去撤**另一件不相干的事**：
            //      "撤 A 失败 → 居然撤了 B"。那比失败更糟 —— 用户以为退回了 A，
            //      实际丢的是 B。
            //
            // 只有"我们的栈本来就是空的"才轮到 shell 的撤销栈。
            return false;
        }
        // 自己没有可撤的 → 落 shell 的撤销栈（用户右键菜单做的操作在那里）。
        const bool ok = WithView([this](IShellView* v) {
            const bool r = shellops::Undo(v);
            if (r) RefreshActive();   // 撤销后的列表必须立刻反映出来
            return r;
        }, L"撤销");
        // 撤销走的是视图背景菜单里的 undo 谓词（不自建栈）。谓词不存在时
        // shellops::Undo 返回 false，调用方只看到"命令执行失败" —— 分不清是
        // "真的没有可撤的"还是"撤销执行失败"。这里至少把话说清楚。
        if (!ok) {
            CommandBus::Get().NoteFailure(
                L"没有可撤销的操作（应用内撤销栈为空，系统的撤销栈里也没有可撤的项）");
        }
        return ok;
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
        if (!PickFolder(hwnd_, ActivePath(), &target)) {
            view->Release();
            return false;
        }
        // 撤销记录要在**搬走之前**取源路径（搬完之后原路径已不存在）。
        const std::vector<std::wstring> sel = shellops::SelectedPaths(view);
        const bool ok = shellops::TransferToFolder(view, hwnd_, target, move);
        if (ok) {
            // 落点 = 目标目录 + 原文件名。冲突时 shell 会自动改名
            //（FOF_RENAMEONCOLLISION），那种情况下算出来的落点并不存在 ——
            // 不记（记一条指向不存在路径的撤销 = 给用户一个假的"能撤"承诺）。
            std::vector<fileundo::Item> items;
            for (const std::wstring& p : sel) {
                const std::wstring leaf = nav::Leaf(p);
                if (leaf.empty()) continue;
                const std::wstring after = tf::Join(target, leaf);
                if (!tf::Exists(after)) continue;
                fileundo::Item item;
                item.beforePath = p;
                item.afterPath = after;
                item.beforeName = leaf;
                items.push_back(std::move(item));
            }
            if (!items.empty())
                NoteFileOp(move ? fileundo::Op::Move : fileundo::Op::Copy, std::move(items),
                           move ? L"移动到" : L"复制到");
        }
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
            // D9：GetSort 只认 0..3 四列，用户此前若用 view.sort.bykey 设了任意
            // 属性列，它会返回 false → 方向落盘被跳过，配置与视图越来越偏。
            // GetSortKey 对任意列都成立，方向读写必须走它。
            PROPERTYKEY got{};
            bool gotAsc = true;
            if (shellops::GetSortKey(v, &got, &gotAsc)) {
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
    reg(CmdId::NavTreeExpandAll, [this](const Args&) {
        Tab* t = Active();
        if (!t || !t->view) return false;
        const bool ok = t->view->ExpandNavTree();
        if (!ok)
            CommandBus::Get().NoteFailure(L"没找到可展开的原生导航树（窗格可能被隐藏）",
                                         FailKind::NotSupported);
        return ok;
    });

    // 详细信息列：list 给机器稳定 id，set 真正落列布局，choose 是人用入口。
    regR(CmdId::ViewColumnsList, [this](const Args&, json::Value* result) {
        if (!result) return false;
        return WithView([result](IShellView* v) {
            std::vector<shellcolumns::ColumnDesc> cols;
            if (!shellcolumns::List(v, &cols)) {
                CommandBus::Get().NoteFailure(L"当前视图不支持详情列管理（需详细信息视图）",
                                         FailKind::NotSupported);
                return false;
            }
            json::Value arr = json::Value::MakeArray();
            for (const auto& c : cols) {
                json::Value row = json::Value::MakeObject();
                row.Set(L"id", json::Value::MakeStr(c.id));
                row.Set(L"name", json::Value::MakeStr(c.name));
                row.Set(L"visible", json::Value::MakeBool(c.visible));
                row.Set(L"canHide", json::Value::MakeBool(c.canHide));
                row.Set(L"width", json::Value::MakeInt(static_cast<int>(c.width)));
                arr.arr.push_back(std::move(row));
            }
            result->Set(L"columns", std::move(arr));
            return true;
        }, L"列出详情列");
    });
    reg(CmdId::ViewColumnsSet, [this](const Args& args) {
        const std::vector<std::wstring> visible = args.StrArray(L"visible");
        return WithView([visible](IShellView* v) {
            if (!shellcolumns::SetVisible(v, visible)) {
                CommandBus::Get().NoteFailure(L"详情列设置失败：列 id 不存在或没有可见列");
                return false;
            }
            return true;
        }, L"设置详情列");
    });
    reg(CmdId::ViewColumnsChoose, [this](const Args&) {
        ShowColumnMenu(navBar_.SettingsAnchor());
        return true;
    });
    // P3-4：按任意属性排序/分组。key 用 shell_columns 的 {GUID}:pid（与
    // view.columns.list / view.props.list 输出的 id 完全同格式，可直接抄）。
    // 解析失败按 bad_args 分类 —— 机器侧拿到的是能改参数的码，不是一句中文。
    regR(CmdId::ViewSortByKey, [this](const Args& args, json::Value* result) {
        PROPERTYKEY key{};
        if (!shellcolumns::ColumnIdFromString(args.Str(L"key"), &key)) {
            CommandBus::Get().NoteFailure(L"属性键格式非法，应为 {GUID}:pid（见 view.props.list）",
                                          FailKind::PreconditionUnmet);
            return false;
        }
        int ascending = -1;
        if (args.Has(L"ascending")) ascending = args.Bool(L"ascending", true) ? 1 : 0;
        Tab* t = Active();
        if (!t || !t->view || !result) return false;
        IShellView* v = t->view->CurrentView();
        if (!v) return false;
        const bool ok = SUCCEEDED(shellops::SetSortByKey(v, key, ascending));
        if (ok) {
            // 回读真实生效值（ascending 省略时方向由 shell 推导，不能复述入参）。
            PROPERTYKEY got{};
            bool gotAsc = false;
            if (shellops::GetSortKey(v, &got, &gotAsc)) {
                result->Set(L"key", json::Value::MakeStr(shellcolumns::ColumnIdToString(got)));
                result->Set(L"ascending", json::Value::MakeBool(gotAsc));
            }
        }
        v->Release();
        return ok;
    });
    regR(CmdId::ViewGroupByKey, [this](const Args& args, json::Value* result) {
        PROPERTYKEY key{};
        if (!shellcolumns::ColumnIdFromString(args.Str(L"key"), &key)) {
            CommandBus::Get().NoteFailure(L"属性键格式非法，应为 {GUID}:pid（见 view.props.list）",
                                          FailKind::PreconditionUnmet);
            return false;
        }
        Tab* t = Active();
        if (!t || !t->view || !result) return false;
        IShellView* v = t->view->CurrentView();
        if (!v) return false;
        const bool ok = SUCCEEDED(shellops::SetGroupByKey(v, key));
        if (ok) {
            PROPERTYKEY got{};
            if (shellops::GetGroupKey(v, &got))
                result->Set(L"key", json::Value::MakeStr(shellcolumns::ColumnIdToString(got)));
        }
        v->Release();
        return ok;
    });
    regR(CmdId::ViewPropsList, [this](const Args&, json::Value* result) {
        if (!result) return false;
        json::Value arr = json::Value::MakeArray();
        for (const auto& p : shellcolumns::KnownProperties()) {
            json::Value row = json::Value::MakeObject();
            row.Set(L"id", json::Value::MakeStr(p.id));
            row.Set(L"title", json::Value::MakeStr(p.title));
            arr.arr.push_back(std::move(row));
        }
        result->Set(L"properties", std::move(arr));
        return true;
    });
    reg(CmdId::ViewZoom, [this](const Args& args) {
        const int steps = args.Int(L"delta");
        if (!steps) return false;
        Tab* t = Active();
        if (!t || !t->view) return false;
        int fvm = 0, icon = 0;
        if (!t->view->GetViewMode(&fvm, &icon)) {
            CommandBus::Get().NoteFailure(L"读不到当前视图模式，无法缩放图标");
            return false;
        }
        t->view->SetViewMode(fvm, viewmode::ZoomIconPx(icon, steps));
        return true;
    });
    reg(CmdId::ViewZoomSet, [this](const Args& args) {
        const int px = args.Int(L"iconPx", 0);
        if (px < viewmode::kMinIconPx || px > viewmode::kMaxIconPx) {
            CommandBus::Get().NoteFailure(L"图标大小必须在 16..256 像素之间");
            return false;
        }
        Tab* t = Active();
        if (!t || !t->view) return false;
        int fvm = 0, icon = 0;
        if (!t->view->GetViewMode(&fvm, &icon)) {
            CommandBus::Get().NoteFailure(L"读不到当前视图模式，无法设置图标大小");
            return false;
        }
        t->view->SetViewMode(fvm, px);
        return true;
    });
    reg(CmdId::ViewZoomChoose, [this](const Args&) {
        ShowZoomMenu(navBar_.SettingsAnchor());
        return true;
    });

    // ---------------- 搜索 ----------------
    //
    // search.find 已于 2026-11-21 降级为 implemented=false：
    // 宿主 IExplorerBrowser 没有搜索带，RunSearch 走 search-ms: 必然失败。
    // 处理器一并移除（check_commands.py 要求 implemented=false 不能有处理器），
    // 让"搜索"在命令面板/快捷键/AI 侧都显示为未实现，而不是每次都失败。
    // 导航栏搜索框仍保留，提交时由 App::RunSearch 给出明确提示。

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

    // 体积统计 / 摘要 / 查重 / 比较这四条长任务命令搬去了
    // app_commands_scan.cpp 与 app_commands_compare.cpp：
    // 那些活可能跑很久，必须在工作线程上跑（进度 + 取消 + async），
    // 而并发约定只该有一份。搬走之后本文件回到行数上限以内。
}

void App::ShowColumnMenu(POINT at) {
    // 每次重开菜单都重新读列状态：上一次勾选已经通过 view.columns.set 落地，
    // 这里若沿用旧快照，第二次勾选会把前一次改动覆盖掉。
    for (;;) {
        std::vector<shellcolumns::ColumnDesc> cols;
        if (IShellView* v = ActiveView()) {
            const bool ok = shellcolumns::List(v, &cols);
            v->Release();
            if (!ok || cols.empty()) {
                CommandBus::Get().NoteFailure(L"当前视图不支持详情列管理（需详细信息视图）",
                                         FailKind::NotSupported);
                return;
            }
        } else {
            CommandBus::Get().NoteFailure(L"没有活动文件视图，无法选择详情列",
                                         FailKind::NotSupported);
            return;
        }

        ui::Menu menu;
        for (size_t i = 0; i < cols.size(); ++i)
            menu.AddChecked(cols[i].name, cols[i].visible, static_cast<int>(i),
                            ico::IconId::DetailsView, !cols[i].canHide);
        menu.Separator();
        menu.Add(L"完成", -2, ico::IconId::Check);

        ui::MenuOptions opt;
        opt.maxRows = 30;
        opt.toggleKey = L"view.columns";   // 同一实例内防重复打开
        const int pick = menu.Show(hwnd_, theme_, at, opt);
        if (pick == -2 || pick < 0 || pick >= static_cast<int>(cols.size())) break;

        std::vector<std::wstring> visible;
        for (size_t i = 0; i < cols.size(); ++i) {
            const bool shown = (static_cast<int>(i) == pick) ? !cols[i].visible : cols[i].visible;
            if (shown) visible.push_back(cols[i].id);
        }
        json::Value args = json::Value::MakeObject();
        json::Value arr = json::Value::MakeArray();
        for (const std::wstring& id : visible) arr.arr.push_back(json::Value::MakeStr(id));
        args.Set(L"visible", std::move(arr));
        CommandBus::Get().Invoke(CmdId::ViewColumnsSet, Args::FromJson(args), Actor::Ui());
    }
}

void App::ShowZoomMenu(POINT at) {
    static constexpr int kSizes[] = {16, 32, 48, 64, 96, 128, 192, 256};
    Tab* t = Active();
    if (!t || !t->view) {
        CommandBus::Get().NoteFailure(L"没有活动文件视图，无法调整图标大小");
        return;
    }
    int fvm = 0, current = 0;
    if (!t->view->GetViewMode(&fvm, &current)) {
        CommandBus::Get().NoteFailure(L"读不到当前图标大小");
        return;
    }

    ui::Menu menu;
    int initial = -1;
    for (size_t i = 0; i < _countof(kSizes); ++i) {
        const int row = menu.AddChecked(std::to_wstring(kSizes[i]) + L" 像素",
                                        kSizes[i] == current, static_cast<int>(i),
                                        ico::IconId::GridView);
        if (kSizes[i] == current) initial = row;
    }
    ui::MenuOptions opt;
    opt.toggleKey = L"view.zoom";
    opt.initialRow = initial;
    const int pick = menu.Show(hwnd_, theme_, at, opt);
    if (pick < 0 || pick >= static_cast<int>(_countof(kSizes))) return;

    json::Value args = json::Value::MakeObject();
    args.Set(L"iconPx", json::Value::MakeInt(kSizes[pick]));
    CommandBus::Get().Invoke(CmdId::ViewZoomSet, Args::FromJson(args), Actor::Ui());
}
