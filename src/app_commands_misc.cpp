// 窗口/系统、外部程序、书签、会话命令。
//
// 零散但共用一处：都是"进程级"动作（开新窗口、起终端、改配置、读写会话），不涉及单个视图。
#include "app.h"

#include "app_commands.h"
#include "hotkey_manager.h"
#include "bookmarks.h"
#include "icon_resolve.h"
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
    reg(CmdId::BmkAddCurrent, [this](const Args&) {
        std::wstring path = ActivePath();
        if (path.empty()) return false;
        BookmarkStore& store = BookmarkStore::Get();
        // 已经收藏过就给出**明确反馈**而不是静默成功。
        // 原实现里 `if (!ContainsPath) AddPath(...)` 之后照样 Save + return true：
        // 用户按 Ctrl+D 两次，第二次什么都没发生，也没有任何提示 ——
        // 看起来像"快捷键坏了"。重复收藏本身也不该静默（可能想加到别的分类）。
        const BookmarkNode* existing = store.FindByPath(path);
        if (existing) {
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
        if (id.empty()) return false;
        BookmarkStore& store = BookmarkStore::Get();
        if (!store.Find(id)) return false;
        // name/path 都没有 = 不修改任何东西（不是"清空"）。要清空路径请显式传 path:""。
        const bool hasName = args.Has(L"name");
        const bool hasPath = args.Has(L"path");
        if (!hasName && !hasPath) return false;
        const std::wstring name = args.Str(L"name");
        const std::wstring path = args.Str(L"path");
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
        if (id.empty()) return false;
        BookmarkStore& store = BookmarkStore::Get();
        if (!store.Find(id)) return false;
        const bool ok = store.Remove(id);
        if (ok) {
            store.Save();
            bookmarkBar_.Reload();
        }
        return ok;
    });
    reg(CmdId::BmkNewFolder, [this](const Args& args) {
        BookmarkStore& store = BookmarkStore::Get();
        const std::wstring name = args.Str(L"name", L"新建分类");
        const std::wstring parent = args.Str(L"parentId");
        const std::wstring id = store.AddFolder(name, parent);
        if (id.empty()) return false;   // parentId 不是分类时 AddFolder 返回空
        store.Save();
        bookmarkBar_.Reload();
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
        if (!store.Undo()) return false;
        store.Save();
        bookmarkBar_.Reload();
        return true;
    });
    reg(CmdId::BmkClearAll, [this](const Args&) {
        BookmarkStore& store = BookmarkStore::Get();
        if (store.roots().empty()) return false;
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
    reg(CmdId::BmkAdd, [this](const Args& args) {
        const std::wstring path = args.Str(L"path");
        if (path.empty()) return false;
        BookmarkStore& store = BookmarkStore::Get();
        const std::wstring parentId = args.Str(L"parentId");
        const std::wstring name = args.Str(L"name", LeafOf(path));

        // 已存在同路径：若给了 parentId 就移动过去（拖放语义），否则如实报 false。
        // 静默成功会让调用方以为"加了一条新的"，而实际什么都没变。
        if (const BookmarkNode* existing = store.FindByPath(path)) {
            if (!parentId.empty() && store.MoveTo(existing->id, parentId)) {
                store.Save();
                bookmarkBar_.Reload();
                return true;
            }
            return false;
        }
        const std::wstring id = store.AddPath(name, path, parentId);
        if (id.empty()) return false;   // parentId 不是分类
        store.Save();
        bookmarkBar_.Reload();
        return true;
    });

    // 移动（拖拽重排/移入分类的结构化版本）：toId 给目标容器，toIndex 给位置。
    reg(CmdId::BmkMove, [this](const Args& args) {
        const std::wstring id = args.Str(L"id");
        if (id.empty()) return false;
        BookmarkStore& store = BookmarkStore::Get();
        if (!store.Find(id)) return false;
        const std::wstring toId = args.Str(L"toId");
        const int toIndex = args.Has(L"toIndex") ? args.Int(L"toIndex", -1) : -1;
        // toId 省略 = 目标为顶层；给了 toId 但为空串也按顶层处理（与 bmk.edit 的
        // "显式空串 = 清空"不同：这里没有"清空容器"这种概念，只有"移到顶层"）。
        const bool ok = store.MoveTo(id, toId, toIndex);
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
}
