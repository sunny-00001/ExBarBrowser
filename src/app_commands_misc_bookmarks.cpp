// 书签编辑命令的注册（收藏夹的增删改查与拖放/重排的结构化入口）。
//
// 【为什么从 app_commands_misc.cpp 拆出来】"零散命令"这个筐原本把窗口、终端、
// 书签、设置、会话全塞在一起 —— 书签这一族就占了 380 行，是全文件的三分之一，
// 而且它是**自成一体的一族**（都要过 BookmarkStore，都有 id/parentId 语义）。
// 按作用域拆开后：app_commands_misc.cpp 只剩进程级动作，
// 书签这条线的改动不必再与窗口/会话的命令混在一个文件里找位置。
#include "app.h"

#include "app_commands.h"
#include "bookmarks.h"
#include "icon_resolve.h"   // ico::DisplayName：命名空间节点的显示名（收藏夹里也要显示真名）

void App::RegisterBookmarkCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;

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
        ShellView* host = ActiveShellViewHost();
        if (!host) return false;
        std::vector<ShellView::SelectionItem> sel;
        host->SelectionPaths(256, sel);   // 与状态快照同一上限，口径一致
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
        // parentId 给了 = **只排这一个分类里的直接子项**（不动别的层）。
        //
        // 为什么单开一条路：整树排序会把用户没碰过的分类也重排一遍 ——
        // 那不是"整理这一个分类"这个动作。分类右键菜单里的"排序本分类"走这里。
        const std::wstring parentId = args.Str(L"parentId");
        if (!parentId.empty()) {
            if (!store.SortChildren(parentId, true, ascending)) {
                CommandBus::Get().NoteFailure(L"parentId 不是分类或不存在：" + parentId);
                return false;
            }
        } else {
            store.SortRecursive(true, ascending, recursive);
        }
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
        // 拖放落点（插入线）。省略 = 追加；给了 0..size 表示插到该位置。
        // 与 bmk.move 的 toIndex 同义，避免"拖新文件夹"和"拖已有文件夹"两套口径。
        const int toIndex = args.Has(L"toIndex") ? args.Int(L"toIndex", -1) : -1;

        // 已存在同路径：给了 parentId 或插入线就移动过去（拖放语义），否则如实报失败
        // + 原因。静默成功会让调用方以为"加了一条新的"，而实际什么都没变。
        if (const BookmarkNode* existing = store.FindByPath(path)) {
            if ((!parentId.empty() || toIndex >= 0) &&
                store.MoveTo(existing->id, parentId, toIndex)) {
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
        const std::wstring id = store.AddPath(name, path, parentId, toIndex);
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

    // 解散分类：把分类里的直接子项提到它所在的上一层（顶层分类 → 书签栏），
    // 分类本身消失。
    //
    // 【为什么走命令而不是在菜单里直接改 store】菜单改 store 就绕过了
    // 撤销栈与审计 —— 而"解散"一次能挪动很多条书签，是最需要能撤回来的动作。
    // 走命令后：撤销栈有快照、审计有记录、AI 与人做的是同一件事（R20）。
    regR(CmdId::BmkDissolve, [this](const Args& args, json::Value* out) -> bool {
        const std::wstring id = args.Str(L"id");
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"缺少 id 参数");
            return false;
        }
        BookmarkStore& store = BookmarkStore::Get();
        const BookmarkNode* node = store.Find(id);
        if (!node) {
            CommandBus::Get().NoteFailure(L"书签不存在：" + id);
            return false;
        }
        if (!node->isFolder) {
            CommandBus::Get().NoteFailure(L"只能解散分类（文件夹），这一项是书签");
            return false;
        }
        const std::vector<std::wstring> ids = store.Dissolve(id);
        store.Save();
        bookmarkBar_.Reload();
        LogDbg(L"[bmk] 解散分类 id=%s 提升 %zu 条", id.c_str(), ids.size());
        if (out) {
            json::Value root = json::Value::MakeObject();
            json::Value arr = json::Value::MakeArray();
            for (const auto& s : ids) arr.arr.push_back(json::Value::MakeStr(s));
            root.Set(L"ids", std::move(arr));
            root.Set(L"count", json::Value::MakeInt(static_cast<int>(ids.size())));
            *out = std::move(root);
        }
        return true;
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

}
