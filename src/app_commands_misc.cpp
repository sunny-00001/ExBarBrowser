// 窗口/系统、外部程序、书签、会话命令。
//
// 零散但共用一处：都是"进程级"动作（开新窗口、起终端、改配置、读写会话），不涉及单个视图。
#include "app.h"

#include "app_commands.h"
#include "hotkey_manager.h"
#include "bookmarks.h"
#include "icon_resolve.h"
#include "prompt.h"      // PromptText：命名会话保存时要人名（人侧无参调用）
#include "report.h"       // ShowReport：只读多行报告窗口（session.list 等）
#include "settings_reg.h"

// 打开终端：wt 优先，失败退回 PowerShell。
// 不是本文件的私有工具 —— "跳转"分片里的 GoPowerShell 也要用（声明在 app_commands.h），
// 所以放在全局作用域而不是匿名命名空间，避免"一处 static、一处 extern 未定义"。
void RunTerminal(HWND owner, const std::wstring& dir, bool admin) {
    if (dir.empty()) return;
    std::wstring args = L"-d \"" + dir + L"\"";
    HINSTANCE r = ShellExecuteW(owner, admin ? L"runas" : L"open", L"wt.exe", args.c_str(),
                                nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) > 32) return;
    std::wstring ps = L"-NoExit -Command \"Set-Location -LiteralPath '" + dir + L"'\"";
    ShellExecuteW(owner, admin ? L"runas" : L"open", L"powershell.exe", ps.c_str(), nullptr,
                  SW_SHOWNORMAL);
}


// 这些设置项决定"当前标签的视图长什么样"。config.set 写完它们必须套用到
// 活动视图，否则用户/AI 改完屏幕上没有任何变化 —— 只能等下次启动才看见。
//
// 为什么这份名单不在注册表里：注册表描述的是"值与类型"，而"写完要推给谁"
// 是界面知识（视图、窗口、栏各自不同）。放在命令层，是因为只有这一层同时
// 知道设置项与窗口。
static bool AffectsActiveView(const std::wstring& key) {
    return key == L"viewMode" || key == L"sortMode" || key == L"sortAscending" ||
           key == L"groupBy";
}

void App::NotifyBookmarkSave(bool ok) {
    if (ok) return;
    BookmarkStore& store = BookmarkStore::Get();
    if (store.TakeConflictFlag()) {
        // 内存树已另存，用户不会丢东西，但必须知道"主文件没被改"。
        MessageBoxW(hwnd_,
                    (L"书签文件已被其它程序修改，本次改动**没有**写进 bookmarks.html。\n\n"
                     L"本程序的版本已另存为：\n"
                     + store.file() + L".conflict.html\n\n"
                     L"请关闭其它程序（编辑器 / 另一个实例）后右键书签栏“重新载入书签文件”，"
                     L"再手工合并这两份。").c_str(),
                    L"书签未保存", MB_OK | MB_ICONWARNING);
        return;
    }
    if (store.LoadFailed()) {
        MessageBoxW(hwnd_,
                    (L"书签文件读不懂（格式或编码问题），已拒绝写盘以防清空你的书签。\n\n"
                     L"请检查或先修好：\n" + store.file()).c_str(),
                    L"书签未保存", MB_OK | MB_ICONWARNING);
        return;
    }
    MessageBoxW(hwnd_, L"书签写入失败（文件被占用或磁盘只读？）。", L"书签未保存",
                MB_OK | MB_ICONWARNING);
}

void App::RegisterMiscCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;

    // ---------------- 窗口/系统 ----------------
    reg(CmdId::NewWindow, [this](const Args&) {
        OpenPathInNewWindow(ActivePath());
        return true;
    });
    reg(CmdId::QuitExplorer, [this](const Args&) {
        // 与标题栏 X / Alt+F4 走同一份确认：命令路径不能成为"绕过提示"的后门。
        // 用户取消时如实报 false，AI/脚本才知道窗口仍然开着。
        if (!ConfirmExit()) {
            CommandBus::Get().NoteFailure(L"用户取消退出", FailKind::CanceledByUser);
            return false;
        }
        OnDestroy();
        DestroyWindow(hwnd_);
        return true;
    });
    reg(CmdId::FolderOptions, [this](const Args&) {
        ShellExecuteW(hwnd_, L"open", L"control.exe", L"folders", nullptr, SW_SHOWNORMAL);
        return true;
    });
    reg(CmdId::MapDrive, [this](const Args&) {
        ShellExecuteW(hwnd_, L"open", L"rundll32.exe",
                      L"shell32.dll,SHHelpShortcuts_RunDLL Connect", nullptr, SW_SHOWNORMAL);
        return true;
    });
    reg(CmdId::UnmapDrive, [this](const Args&) {
        ShellExecuteW(hwnd_, L"open", L"rundll32.exe",
                      L"shell32.dll,SHHelpShortcuts_RunDLL Disconnect", nullptr, SW_SHOWNORMAL);
        return true;
    });
    reg(CmdId::CommandPalette, [this](const Args&) { ShowCommandPalette(); return true; });
    reg(CmdId::HotkeyManager, [this](const Args&) {
        ShowHotkeyManager(hwnd_, theme_, input_);
        return true;
    });
    // 首选项面板本身早就存在，但只有一条入口：导航栏右键 →「设置…」。
    // 于是命令面板里搜不到它、键位绑不上、Agent 更看不到 —— 用户被告知
    // "有首选项"却只能靠右键碰运气。这里补的是**入口**，不是新面板：
    // 锚点也复用导航栏那一个（NavBar::SettingsAnchor），两条路径弹在同一处。
    reg(CmdId::Preferences, [this](const Args&) {
        ShowSettingsPanel(navBar_.SettingsAnchor());
        return true;
    });
    reg(CmdId::FocusAddress, [this](const Args&) { navBar_.FocusAddress(); return true; });

    // ---------------- 外部 ----------------
    reg(CmdId::OpenTerminal, [this](const Args&) {
        RunTerminal(hwnd_, ActivePath(), false);
        return true;
    });
    reg(CmdId::OpenTerminalAdmin, [this](const Args&) {
        RunTerminal(hwnd_, ActivePath(), true);
        return true;
    });

    // ---------------- 书签 ----------------
    regR(CmdId::BmkAddCurrent, [this](const Args&, json::Value* out) -> bool {
        std::wstring path = ActivePath();
        if (path.empty()) {
            CommandBus::Get().NoteFailure(L"当前没有活动目录");
            return false;
        }
        BookmarkStore& store = BookmarkStore::Get();
        // 已经收藏过就给出**明确反馈**而不是静默成功。
        // 原实现里 `if (!ContainsPath) AddPath(...)` 之后照样 Save + return true：
        // 用户按 Ctrl+D 两次，第二次什么都没发生，也没有任何提示 ——
        // 看起来像"快捷键坏了"。重复收藏本身也不该静默（可能想加到别的分类）。
        // AI 侧不弹窗（会卡住管道线程），如实报失败 + 原因。
        const BookmarkNode* existing = store.FindByPath(path);
        if (existing) {
            if (CommandBus::Get().fromMachine()) {
                CommandBus::Get().NoteFailure(L"已收藏：" + existing->name);
                return false;
            }
            MessageBeep(MB_ICONASTERISK);
            MessageBoxW(hwnd_,
                        (L"“" + existing->name + L"” 已经收藏过了。\n\n"
                         L"要放到别的分类里，可直接把这条书签拖到分类上。").c_str(),
                        id::kBrandShort, MB_OK | MB_ICONINFORMATION);
            return true;
        }
        // 命名空间的 LeafOf 就是裸 "::{GUID}"；显示名统一走 ico::DisplayName。
        const std::wstring name =
            nav::IsNamespace(path) ? ico::DisplayName(path) : LeafOf(path);
        std::wstring id = store.AddPath(name, path);
        bool saved = store.Save();
        // Save 内部已触发 saveNotify_（App::NotifyBookmarkSave），不要在这里重复提示。
        LogDbg(L"[bmk] add path=%s id=%s saved=%d file=%s", path.c_str(), id.c_str(), saved ? 1 : 0,
               store.file().c_str());
        bookmarkBar_.Reload();
        // 返回值契约（OPERATION-MODEL 第 5 步）：声明了 outputSchema 就真返回。
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"id", json::Value::MakeStr(id));
            *out = std::move(root);
        }
        return true;
    });
    // 收藏**选中的**文件夹（可多选）。与 bmk.add.current 的"当前目录"是两个意图：
    // current = 我正在看的目录；selection = 我框选的条目。只收文件夹，文件与
    // 已收藏项跳过 —— 批量动作必须可预期：AI 从观测面预知 selectionCount 与
    // selectionFolders，执行后用 bmk.list 验证，不在这里引入第三份口径。
    reg(CmdId::BmkAddSelection, [this](const Args&) {
        Tab* t = Active();
        if (!t || !t->view) return false;
        std::vector<ShellView::SelectionItem> sel;
        t->view->SelectionPaths(256, sel);   // 与状态快照同一上限，口径一致
        if (sel.empty()) return false;       // HasSelection 前置已挡一层，双保险
        BookmarkStore& store = BookmarkStore::Get();
        bool any = false;
        for (const auto& si : sel) {
            if (!si.isFolder || si.path.empty()) continue;   // 文件不是书签目标
            if (store.FindByPath(si.path)) continue;         // 已收藏：跳过，不静默重复
            const std::wstring name =
                nav::IsNamespace(si.path) ? ico::DisplayName(si.path) : LeafOf(si.path);
            if (!store.AddPath(name, si.path).empty()) any = true;
        }
        if (!any) return false;   // 全部跳过（无新增）：如实失败，调用方回观测面查原因
        store.Save();
        bookmarkBar_.Reload();
        return true;
    });
    // 收藏**所有打开标签页**的目录。与 add.current（只收当前目录）互补：
    // "把今天打开的一组目录存成书签"。已收藏/空路径跳过（skipped 计数），
    // 一条没加也如实返回 added=0 —— 批量动作的可预期性靠响应数字，不靠猜。
    // 返回 ids（顺序同标签顺序）：补上"新条目不返回 id"这个调用方一直缺的东西。
    regR(CmdId::BmkAddTabs, [this](const Args&, json::Value* out) -> bool {
        BookmarkStore& store = BookmarkStore::Get();
        int added = 0, skipped = 0;
        json::Value ids = json::Value::MakeArray();
        for (const auto& kv : tabs_) {
            const std::wstring path = kv.second->path();
            if (path.empty()) { ++skipped; continue; }        // 新建空标签无目录可收藏
            if (store.FindByPath(path)) { ++skipped; continue; }  // 已收藏：不静默重复
            const std::wstring name =
                nav::IsNamespace(path) ? ico::DisplayName(path) : LeafOf(path);
            const std::wstring id = store.AddPath(name, path);
            if (id.empty()) { ++skipped; continue; }
            ids.arr.push_back(json::Value::MakeStr(id));
            ++added;
        }
        if (added > 0) {
            store.Save();
            bookmarkBar_.Reload();
        }
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"added", json::Value::MakeInt(added));
            root.Set(L"skipped", json::Value::MakeInt(skipped));
            root.Set(L"ids", std::move(ids));
            *out = std::move(root);
        }
        return true;
    });
    // 复制书签（分类连子树深拷贝，id 全部重分配）。新 id 返回给调用方：
    // 复制之后的下一步（编辑副本指向别的目录）没有 id 就做不了。
    regR(CmdId::BmkCopy, [this](const Args& args, json::Value* out) -> bool {
        BookmarkStore& store = BookmarkStore::Get();
        const std::wstring newId = store.Copy(args.Str(L"id"));
        if (newId.empty()) {
            CommandBus::Get().NoteFailure(L"要复制的书签不存在：" + args.Str(L"id"));
            return false;
        }
        store.Save();
        bookmarkBar_.Reload();
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"id", json::Value::MakeStr(newId));
            *out = std::move(root);
        }
        return true;
    });
    reg(CmdId::BmkToggleBar, [this](const Args&) {
        Settings& s = Config::Get().mutableSettings();
        s.showBookmark = !s.showBookmark;
        Config::Get().Save();
        Layout();
        return true;
    });
    reg(CmdId::BmkShowBar, [this](const Args&) {
        Config::Get().mutableSettings().showBookmark = true;
        Config::Get().Save();
        Layout();
        return true;
    });
    reg(CmdId::BmkHideBar, [this](const Args&) {
        Config::Get().mutableSettings().showBookmark = false;
        Config::Get().Save();
        Layout();
        return true;
    });

    // ---- 书签编辑族 ----
    //
    // 这些动作原先只存在于书签栏右键菜单里。补成命令不是为了"多几条命令"，
    // 而是因为菜单里那份实现**没有任何非鼠标入口**：
    // 键盘用户、命令面板、AI 都做不到 —— 而 R20 要的是"同一件事人能做"。
    // 菜单现在也改为调这些命令，于是只有一份实现。
    reg(CmdId::BmkEdit, [this](const Args& args) {
        const std::wstring id = args.Str(L"id");
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"缺少 id 参数");
            return false;
        }
        BookmarkStore& store = BookmarkStore::Get();
        if (!store.Find(id)) {
            CommandBus::Get().NoteFailure(L"书签不存在：" + id);
            return false;
        }
        // name/path 都没有 = 不修改任何东西（不是"清空"）。要清空路径请显式传 path:""。
        const bool hasName = args.Has(L"name");
        const bool hasPath = args.Has(L"path");
        if (!hasName && !hasPath) {
            CommandBus::Get().NoteFailure(L"没有给出要修改的字段（name/path）");
            return false;
        }
        const std::wstring name = args.Str(L"name");
        const std::wstring path = args.Str(L"path");
        // shell 命名空间路径入口校验：缺 } 的 CLSID 落盘后"打不开+通用图标"
        // 两头排查（2026-11-21 真实踩坑两次）。
        if (hasPath && !path.empty() && !IsValidShellNamespacePath(path)) {
            CommandBus::Get().NoteFailure(
                L"shell 命名空间路径不完整（应为 ::{8-4-4-4-12} 共 40 字符）：" + path);
            return false;
        }
        bool ok;
        if (hasPath)
            ok = store.SetNameAndPath(id, name, path);   // 显式给了 path：空串就是清空
        else
            ok = store.Rename(id, name);
        if (ok) {
            store.Save();
            bookmarkBar_.Reload();
        }
        return ok;
    });
    reg(CmdId::BmkRemove, [this](const Args& args) {
        const std::wstring id = args.Str(L"id");
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"缺少 id 参数");
            return false;
        }
        BookmarkStore& store = BookmarkStore::Get();
        if (!store.Find(id)) {
            CommandBus::Get().NoteFailure(L"书签不存在：" + id);
            return false;
        }
        const bool ok = store.Remove(id);
        if (ok) {
            store.Save();
            bookmarkBar_.Reload();
        }
        return ok;
    });
    regR(CmdId::BmkNewFolder, [this](const Args& args, json::Value* out) -> bool {
        BookmarkStore& store = BookmarkStore::Get();
        const std::wstring name = args.Str(L"name", L"新建分类");
        const std::wstring parent = args.Str(L"parentId");
        const std::wstring id = store.AddFolder(name, parent);
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"parentId 不是分类或不存在：" + parent);
            return false;
        }
        store.Save();
        bookmarkBar_.Reload();
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"id", json::Value::MakeStr(id));
            *out = std::move(root);
        }
        return true;
    });
    reg(CmdId::BmkFocus, [this](const Args&) {
        bookmarkBar_.FocusBar();
        // 栏为空时也算成功（栏拿到了焦点，只是没有项可走）—— 若返回 false，
        // 调用方会以为"命令失败"，而实际是"书签栏是空的"。
        return true;
    });
    reg(CmdId::BmkUndo, [this](const Args&) {
        BookmarkStore& store = BookmarkStore::Get();
        if (!store.CanUndo()) {
            CommandBus::Get().NoteFailure(L"没有可撤销的操作");
            return false;
        }
        // AI 的撤销只到自己的上一次修改为止，不穿透界面操作。
        // 否则机器侧一次批量 undo 会把用户几天前的手工整理全部滚掉
        // （2026-11-21 结构化审计实测：40 次 undo 穿透回滚全部历史）。
        if (CommandBus::Get().fromMachine() && !store.TopUndoFromMachine()) {
            CommandBus::Get().NoteFailure(L"上一次修改来自界面操作，AI 撤销到此为止");
            return false;
        }
        store.Undo();
        store.Save();
        bookmarkBar_.Reload();
        return true;
    });
    reg(CmdId::BmkClearAll, [this](const Args&) {
        BookmarkStore& store = BookmarkStore::Get();
        if (store.roots().empty()) {
            CommandBus::Get().NoteFailure(L"书签栏为空，无事可清");
            return false;
        }
        store.ReplaceAll(BookmarkList{});
        store.Save();
        bookmarkBar_.Reload();
        return true;
    });
    reg(CmdId::BmkSort, [this](const Args& args) {
        BookmarkStore& store = BookmarkStore::Get();
        // recursive 默认 true：菜单上的"按名称排序"用户预期是整棵都排，
        // 只排顶层等于"排了一半" —— 原实现就是这样，分类里的顺序纹丝不动。
        const bool recursive = !args.Has(L"recursive") || args.Bool(L"recursive", true);
        const bool ascending = !args.Has(L"ascending") || args.Bool(L"ascending", true);
        store.SortRecursive(true, ascending, recursive);
        store.Save();
        bookmarkBar_.Reload();
        return true;
    });
    reg(CmdId::BmkDedupe, [this](const Args&) {
        BookmarkStore& store = BookmarkStore::Get();
        const size_t n = store.RemoveDuplicatePaths();
        if (!n) return true;   // 没有重复不是失败，只是无事可做
        store.Save();
        bookmarkBar_.Reload();
        LogDbg(L"[bmk] 清理重复：删除 %zu 条", n);
        return true;
    });

    // 添加指定路径（拖放/脚本/AI 的共同入口）。与 bmk.add.current 的区别是
    // **目标由参数点名**，不读"当前目录" —— 机器侧不能假设前台标签就是它想的那个。
    regR(CmdId::BmkAdd, [this](const Args& args, json::Value* out) -> bool {
        const std::wstring path = args.Str(L"path");
        if (path.empty()) {
            CommandBus::Get().NoteFailure(L"缺少 path 参数");
            return false;
        }
        // shell 命名空间路径入口校验（与 bmk.edit 同一规则、同一句话）。
        if (!IsValidShellNamespacePath(path)) {
            CommandBus::Get().NoteFailure(
                L"shell 命名空间路径不完整（应为 ::{8-4-4-4-12} 共 40 字符）：" + path);
            return false;
        }
        BookmarkStore& store = BookmarkStore::Get();
        const std::wstring parentId = args.Str(L"parentId");
        const std::wstring name = args.Str(L"name", LeafOf(path));

        // 已存在同路径：若给了 parentId 就移动过去（拖放语义），否则如实报失败
        // + 原因。静默成功会让调用方以为"加了一条新的"，而实际什么都没变。
        if (const BookmarkNode* existing = store.FindByPath(path)) {
            if (!parentId.empty() && store.MoveTo(existing->id, parentId)) {
                store.Save();
                bookmarkBar_.Reload();
                if (out) {
                    json::Value root = json::Value::MakeObject();
                    root.Set(L"id", json::Value::MakeStr(existing->id));
                    root.Set(L"moved", json::Value::MakeBool(true));
                    *out = std::move(root);
                }
                return true;
            }
            CommandBus::Get().NoteFailure(L"路径已收藏：" + existing->name);
            return false;
        }
        const std::wstring id = store.AddPath(name, path, parentId);
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"parentId 不是分类或不存在：" + parentId);
            return false;
        }
        store.Save();
        bookmarkBar_.Reload();
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"id", json::Value::MakeStr(id));
            *out = std::move(root);
        }
        return true;
    });

    // 移动（拖拽重排/移入分类的结构化版本）：toId 给目标容器，toIndex 给位置。
    reg(CmdId::BmkMove, [this](const Args& args) {
        const std::wstring id = args.Str(L"id");
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"缺少 id 参数");
            return false;
        }
        BookmarkStore& store = BookmarkStore::Get();
        if (!store.Find(id)) {
            CommandBus::Get().NoteFailure(L"书签不存在：" + id);
            return false;
        }
        const std::wstring toId = args.Str(L"toId");
        const int toIndex = args.Has(L"toIndex") ? args.Int(L"toIndex", -1) : -1;
        // toId 省略 = 目标为顶层；给了 toId 但为空串也按顶层处理（与 bmk.edit 的
        // "显式空串 = 清空"不同：这里没有"清空容器"这种概念，只有"移到顶层"）。
        const bool ok = store.MoveTo(id, toId, toIndex);
        if (!ok) CommandBus::Get().NoteFailure(L"目标分类不存在或移动被拒：" + toId);
        if (ok) {
            store.Save();
            bookmarkBar_.Reload();
        }
        return ok;
    });

    // 列出书签（读）。其余 bmk.* 要的 id 由此获得 —— 没有它，
    // "删除书签"这类命令对机器侧是断的：知道能做，但不知道对哪一条做。
    regR(CmdId::BmkList, [](const Args& args, json::Value* out) -> bool {
        BookmarkStore& store = BookmarkStore::Get();
        const std::wstring parentId = args.Str(L"parentId");
        json::Value arr = json::Value::MakeArray();
        std::function<void(const BookmarkList&, const std::wstring&)> walk =
            [&](const BookmarkList& list, const std::wstring& parent) {
                for (const auto& n : list) {
                    json::Value o = json::Value::MakeObject();
                    o.Set(L"id", json::Value::MakeStr(n.id));
                    o.Set(L"name", json::Value::MakeStr(n.name));
                    o.Set(L"path", json::Value::MakeStr(n.path));
                    o.Set(L"isFolder", json::Value::MakeBool(n.isFolder));
                    o.Set(L"parentId", json::Value::MakeStr(parent));
                    arr.arr.push_back(std::move(o));
                    walk(n.children, n.id);
                }
            };
        if (parentId.empty()) {
            walk(store.roots(), L"");
        } else if (const BookmarkNode* folder = store.Find(parentId)) {
            if (!folder->isFolder) return false;   // 不是分类，没有"子项"可列
            walk(folder->children, parentId);
        } else {
            return false;   // id 写错：如实报失败，而不是返回空列表（那会像"没有书签"）
        }
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"entries", std::move(arr));
            *out = std::move(root);
        }
        return true;
    });

    // ---------------- 设置域 ----------------
    // 这三条是「AI 调试某个设置项」的入口。之前设置是编译期 struct，
    // 机器侧既不知道有哪些项，也没有稳定名字去定位 —— 那条路只能靠人眼看界面。
    //
    // 读写都走 settings_reg 的同一份表，不在这里另存副本：
    // 两份副本一旦分叉，"命令改了 A、界面显示 B"就只有用户能发现。
    regR(CmdId::ConfigList, [](const Args&, json::Value* out) -> bool {
        if (!out) return false;
        json::Value items = settings_reg::DumpAll();
        json::Value root = json::Value::MakeObject();
        root.Set(L"items", std::move(items));
        *out = std::move(root);
        return true;
    });
    regR(CmdId::ConfigGet, [](const Args& args, json::Value* out) -> bool {
        const std::wstring key = args.Str(L"key");
        if (key.empty() || !out) return false;
        json::Value value;
        if (!settings_reg::Get(key, &value)) {
            return false;   // key 不存在：如实报失败，不返回空值（那会像"值是空"）
        }
        json::Value root = json::Value::MakeObject();
        root.Set(L"key", json::Value::MakeStr(key));
        root.Set(L"value", std::move(value));
        *out = std::move(root);
        return true;
    });
    // config.set 之后必须写盘：改内存不落盘的话，重启就回退了 ——
    // 而"改了看不见效果、下次启动才暴露"正是设置类 bug 最难查的形态。
    reg(CmdId::ConfigSet, [this](const Args& args) {
        const std::wstring key = args.Str(L"key");
        if (key.empty()) {
            CommandBus::Get().NoteFailure(L"缺少 key：设置项的点分名（见 config.list）");
            return false;
        }
        // value 是任意 JSON（bool / int / string），类型由注册表按 key 校验。
        // 这里不用 args.Bool/Int —— 那会把"传了字符串给布尔项"变成静默的 false。
        const json::Value* value = args.Any(L"value");
        if (!value) {
            CommandBus::Get().NoteFailure(L"缺少 value：要设成什么");
            return false;
        }
        std::wstring err;
        if (!settings_reg::Set(key, *value, &err)) {
            CommandBus::Get().NoteFailure(err);   // 原因直达 UI / AI / 审计
            return false;
        }
        Config::Get().Save();
        // 刷新依赖设置的界面。只有这里知道该刷什么，
        // 所以刷新放命令层，而不是让注册表自己去找窗口。
        //
        // 视图那几项必须套到当前标签：只写配置的话，改完屏幕上毫无变化 ——
        // 正是"改了看不见效果、重启才发现"那类 bug。套用走启动时同一个
        // ApplyPersistedViewState，不另写一份。
        if (AffectsActiveView(key)) ApplyPersistedViewState(Active() ? Active()->view.get() : nullptr);
        SyncChrome();
        // AI 通道热插拔：设置已落盘，真正启停交给独立的 WM_APP_SYNC_AI。
        // 不能在这里直接 StopServer —— 关闭请求本身可能来自该通道，
        // 同步 join 监听线程会让响应写不回去。人的显式设置优先于 --ai 强制开关。
        if (key == L"aiEnabled") {
            Config::Get().SetAiForceEnabled(false);
            PostMessageW(hwnd_, WM_APP_SYNC_AI, 0, 0);
        }
        return true;
    });

    // ---------------- 命名会话 ----------------
    // 与 session.* 的差别：多条并存、按名字定位。保存 = 同名覆盖（幂等），
    // 恢复 = 追加开新标签（不关已有标签），删除前不再弹确认 —— 名字是
    // 显式意图，且误删的损失只是一组路径。
    regR(CmdId::NamedSessionSave, [this](const Args& args, json::Value* out) {
        std::wstring name = str::Trim(args.Str(L"name"));
        if (name.empty()) {
            // 人侧无参调用：弹框要名字；机器侧直接失败（schema 已要求 name）。
            if (IsHumanCall(args)) {
                name = L"标签组";
                if (!PromptText(hwnd_, theme_, L"保存命名会话", L"会话名称", name, &name))
                    return false;
                name = str::Trim(name);
                if (name.empty()) return false;
            } else {
                CommandBus::Get().NoteFailure(L"缺少会话名");
                return false;
            }
        }
        NamedSession sess;
        sess.name = name;
        for (const auto& t : tabSet_.items())
            if (!t.path.empty()) sess.paths.push_back(t.path);
        auto& list = Config::Get().mutableSettings().namedSessions;
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const NamedSession& e) { return e.name == name; });
        if (it != list.end()) *it = std::move(sess);
        else list.push_back(std::move(sess));
        Config::Get().Save();
        if (out) out->Set(L"tabs", json::Value::MakeInt(static_cast<int>(sess.paths.size())));
        return true;
    });
    regR(CmdId::NamedSessionList, [this](const Args& args, json::Value* out) {
        const auto& list = Config::Get().s().namedSessions;
        json::Value names = json::Value::MakeArray();
        std::wstring report;
        for (const auto& e : list) {
            names.arr.push_back(json::Value::MakeStr(e.name));
            report += e.name + L"（" + std::to_wstring(e.paths.size()) + L" 项）\r\n";
        }
        out->Set(L"names", names);
        out->Set(L"count", json::Value::MakeInt(static_cast<int>(list.size())));
        if (IsHumanCall(args)) {
            const std::wstring text = list.empty()
                ? std::wstring(L"还没有命名会话。\r\n\r\n用 session.named.save（或菜单）保存当前标签组。")
                : L"共 " + std::to_wstring(list.size()) + L" 个：\r\n\r\n" + report;
            ShowReport(hwnd_, theme_, L"命名会话", text);
        }
        return true;
    });
    regR(CmdId::NamedSessionRestore, [this](const Args& args, json::Value* out) {
        const std::wstring name = str::Trim(args.Str(L"name"));
        const auto& list = Config::Get().s().namedSessions;
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const NamedSession& e) { return e.name == name; });
        if (it == list.end()) {
            CommandBus::Get().NoteFailure(L"没有名为「" + name + L"」的会话");
            return false;
        }
        int opened = 0;
        for (const auto& p : it->paths) {
            if (!PathOpenable(p)) continue;   // 已失效的目录跳过，不阻塞整组恢复
            if (NewTab(p)) ++opened;
        }
        out->Set(L"opened", json::Value::MakeInt(opened));
        return opened > 0;
    });
    reg(CmdId::NamedSessionDelete, [this](const Args& args) {
        const std::wstring name = str::Trim(args.Str(L"name"));
        auto& list = Config::Get().mutableSettings().namedSessions;
        const auto it = std::find_if(list.begin(), list.end(),
                                     [&](const NamedSession& e) { return e.name == name; });
        if (it == list.end()) {
            CommandBus::Get().NoteFailure(L"没有名为「" + name + L"」的会话");
            return false;
        }
        list.erase(it);
        Config::Get().Save();
        return true;
    });

    // ---------------- 会话 ----------------
    reg(CmdId::SessionSave, [this](const Args&) {
        std::vector<std::wstring> paths;
        for (const auto& t : tabSet_.items())
            if (!t.path.empty()) paths.push_back(t.path);
        Config::Get().RememberSession(paths, ActiveIndex());
        Config::Get().Save();
        return true;
    });
    reg(CmdId::SessionRestore, [this](const Args&) {
        std::vector<std::wstring> paths = Config::Get().s().sessionPaths;
        for (const auto& p : paths) NewTab(p);
        return true;
    });
    // 会话在**本工程只有一条**：session.save 覆盖它，restore 整组重开。
    //
    // 所以这两条命令的语义按真实模型收窄，而不是假装有"多个命名标签组"：
    //   session.list   = 读出那一条（包含哪些路径、哪一个是活动标签）
    //   session.delete = 清空那一条（restore 从此没有可恢复的内容）
    // 名字里的复数来自 mod 期能力。真要做多会话是另一件事（要有名字、要有覆盖/
    // 重名策略），不该用这两条命令名去冒充。
    regR(CmdId::SessionList, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const auto& paths = Config::Get().s().sessionPaths;
        const int active = Config::Get().s().sessionActive;

        json::Value arr = json::Value::MakeArray();
        std::wstring report;
        for (size_t i = 0; i < paths.size(); ++i) {
            arr.arr.push_back(json::Value::MakeStr(paths[i]));
            const bool isActive = (static_cast<int>(i) == active);
            report += (isActive ? L"* " : L"  ") + paths[i] + L"\r\n";
        }
        result->Set(L"paths", arr);
        result->Set(L"count", json::Value::MakeInt(static_cast<int>(paths.size())));
        result->Set(L"active", json::Value::MakeInt(active));

        if (IsHumanCall(args)) {
            // 人侧：报告窗口。空的时候也要说清楚"空"和"怎么存"，否则像功能没生效。
            const std::wstring text =
                paths.empty()
                    ? std::wstring(L"还没有保存过标签组。\r\n\r\n"
                                   L"用菜单「保存标签组」或 session.save 把当前标签存下来。")
                    : L"共 " + std::to_wstring(paths.size()) + L" 项（* 为活动标签）：\r\n\r\n" + report;
            ShowReport(hwnd_, theme_, L"已保存的标签组", text);
        }
        return true;
    });
    regR(CmdId::SessionDelete, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const bool human = IsHumanCall(args);
        const int n = static_cast<int>(Config::Get().s().sessionPaths.size());

        if (human) {
            if (n == 0) {
                // 本来就没有：不弹确认，直接说清楚。报"成功删除 0 项"会让人以为
                // 之前有过东西被删掉了。
                MessageBoxW(hwnd_, L"没有保存过标签组，不需要清除。", L"清除标签组",
                            MB_OK | MB_ICONINFORMATION);
                result->Set(L"cleared", json::Value::MakeBool(false));
                result->Set(L"count", json::Value::MakeInt(0));
                return true;
            }
            const std::wstring msg = L"清除已保存的标签组？\r\n\r\n共 " + std::to_wstring(n) +
                                     L" 项。清除后 session.restore 没有可恢复的内容；"
                                     L"当前打开的标签不受影响。";
            if (MessageBoxW(hwnd_, msg.c_str(), L"清除标签组",
                            MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
                return false;   // 用户取消：什么都没做
            }
        }

        Config::Get().RememberSession({}, -1);
        Config::Get().Save();
        result->Set(L"cleared", json::Value::MakeBool(n > 0));
        result->Set(L"count", json::Value::MakeInt(n));
        return true;
    });
}
