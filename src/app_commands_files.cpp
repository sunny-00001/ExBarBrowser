// 文件操作 / 视图 两组命令。
//
// 共同点：都要"拿当前 IShellView 干活再判成败"，统一走 App::WithView（失败记账 + 响铃）。
#include "app.h"

#include "app_commands.h"
#include "shell_ops.h"
#include "disk_scan.h"
#include "text_file.h"

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
    reg(CmdId::FsCopy, [this](const Args& args) {
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
    reg(CmdId::FsCut, [this](const Args& args) {
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
    reg(CmdId::FsProperties, [this](const Args& args) {
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
    reg(CmdId::CmdNewFolder, [this](const Args&) {
        return WithView([this](IShellView* v) {
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
        return WithView([this](IShellView* v) { return shellops::SelectIndex(v, 0); }, L"定位首项");
    });
    reg(CmdId::FileFocusBottom, [this](const Args&) {
        return WithView([this](IShellView* v) {
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
    auto regR = cmd_reg::RegisterWithResult;

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
}
