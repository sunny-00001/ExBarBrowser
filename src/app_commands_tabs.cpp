// 标签 / 跳转 / 导航 / 路径 四组命令。
//
// 共同点：只动"当前标签集合与历史栈"，不碰 IShellView 里的文件操作。
#include "app.h"

#include "app_commands.h"
#include "shortcut_clipboard.h"
#include "text_file.h"   // tf::IsDirectory（go.powershell 的目录存在性判定）

namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

std::wstring ToUnc(const std::wstring& p) {
    // 命名空间的解析串 "::{GUID}\..." 第二个字符也是 ':'，不能当盘符转成 UNC，
    // 否则「复制为 UNC 路径」得到 \\localhost\:$... 这种垃圾。用统一口径排除。
    if (p.size() >= 2 && p[1] == L':' && !nav::IsNamespace(p)) {
        std::wstring drive(1, p[0]);
        return L"\\\\localhost\\" + drive + L"$" + p.substr(2);
    }
    return p;
}

}  // namespace


void App::RegisterTabCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;
    // ---------------- 标签 ----------------
    reg(CmdId::TabNew, [this](const Args& args) {
        // 与"+"按钮（WM_APP_TAB_NEW）同一口径：省略 path 时打开此电脑（不是
        // 沿用当前目录 —— 那是 tab.clone），人和 AI 说"新建标签"结果一致（R20）。
        // path 照旧支持：调用方明确给了目录就按目录开（此前 handler 丢弃它）。
        std::wstring p = args.Str(L"path");
        const std::wstring target = p.empty() ? std::wstring(nav::kThisPc) : p;
        // 预检路径：不存在的文件系统路径直接拒绝，不制造"标签建了但没打开"的假成功。
        if (!PathOpenable(target)) {
            CommandBus::Get().NoteFailure(L"路径不存在或不可访问：" + target);
            return false;
        }
        // 透传真实创建结果：路径不存在/盘符离线时 NewTab 返回 0，
        // 此时报 ok=true 会让审计与 AI 都以为标签已开（旧实现在这里恒 true）。
        const int id = NewTab(target, NewTabIndex());
        if (!id) CommandBus::Get().NoteFailure(L"无法打开：" + target);
        return id != 0;
    });
    // 目标标签：index 可选，省略 = 活动标签（快捷键/面板口径）；右键菜单传
    // 被右键那一格的下标。越界如实失败，不就近取一个（与 tab.lock 同纪律）。
    reg(CmdId::TabClose, [this](const Args& args) {
        // 透传真实结果：锁定标签会拒绝关闭，那时不能报 ok=true。
        // 前置已要求 HasMultipleTabs，所以这里不会走到"关掉最后一个 = 退出程序"。
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        return id ? CloseTab(id) : false;
    });
    reg(CmdId::TabCloseOther, [this](const Args& args) {
        // 如实回传：所有其他标签都锁定时一个也没关，不能报 ok=true。
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        return id ? CloseOthers(id) > 0 : false;
    });
    // 边界返回 0 条时必须给原因：同族的 close.prev/close.next 会说"已在最左/最右"，
    // 而这里曾经只返回 false —— 调用方只看到"命令执行失败"，分不清是边界
    // 还是真出错（2026-11-21 全量结构化测试实测）。
    reg(CmdId::TabCloseLeft, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        if (id && CloseSide(id, true) > 0) return true;
        CommandBus::Get().NoteFailure(L"左侧没有标签");
        return false;
    });
    reg(CmdId::TabCloseRight, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        if (id && CloseSide(id, false) > 0) return true;
        CommandBus::Get().NoteFailure(L"右侧没有标签");
        return false;
    });
    reg(CmdId::TabClone, [this](const Args& args) {
        // 复制"被右键的那个"标签的路径；省略 index 才是当前标签。
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const core::TabRef* ref = tabSet_.Find(tabSet_.IdAt(idx));
        if (!ref) return false;
        const std::wstring p = ref->path;
        const int id = NewTab(p, NewTabIndex());
        if (!id) CommandBus::Get().NoteFailure(L"无法复制标签：" + p);
        return id != 0;
    });
    // ---- 标签个性化：与右键菜单共用同一份实现（R6：动作只有一份）----
    // 菜单路径直接调 RenameTab/SetTabColor；命令路径经这里转一道，
    // 两条路上"改名的落盘口径"完全一致（否则会出现菜单改名能存、面板改名存不住）。
    reg(CmdId::TabRename, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        if (!id || !tabSet_.Find(id)) return false;
        RenameTab(id, args.Str(L"name"));
        return true;
    });
    reg(CmdId::TabSetColor, [this](const Args& args) {
        // index 可选，省略 = 当前活动标签（与 tab.pin / tab.copy.title / tab.move.window
        // 同一口径）。【为什么必须支持 index】标签右键菜单的色板作用于**被右键的那个**
        // 标签，而"当前活动标签"在右键非活动标签时是另一个 —— 只认 activeId_ 的话，
        // 色板选完会给错误的标签上色。菜单侧传 index，命令面板/AI 省略即当前。
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        if (!id) return false;
        const int color = args.Int(L"color", 0);
        if (color < 0 || color >= Theme::kTabColorCount) return false;  // 越界如实报错
        SetTabColor(id, color);
        return true;
    });
    reg(CmdId::TabCloseDuplicates, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        if (!id || !tabSet_.Find(id)) return false;
        // 返回"是否真的关掉了至少一个"：一个重复都没有时应当是 false（ok=0），
        // 否则审计里会记成成功，AI 也以为自己清理过了。
        return CloseDuplicates(id) > 0;
    });

    // ---- 标签锁定 / 复制标签名 ----
    // 这两条原先只在右键菜单的 switch 里（键盘/面板/脚本/AI 都做不到）。
    // 目标标签：给了 index 就用它，省略则用当前标签；越界如实报 false，
    // 不"就近取一个"。为什么每条 handler 自己写 `args.Has(L"index")` 而不抽
    // 公共 lambda：静态检查靠"handler 里出现了 args.X(L"键")"核对 schema 与
    // 实现一致（check_ai_meta 第 3 项），藏进辅助函数会让那条检查失效 ——
    // 而它挡的正是"schema 声明了参数、handler 却没收"的静默丢弃。
    reg(CmdId::TabLock, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        if (!id) return false;
        const core::TabRef* ref = tabSet_.Find(id);
        if (!ref) return false;
        // 省略 locked = 切换（与右键菜单同义）；给了值就按值设定（幂等）。
        const bool want = args.Has(L"locked") ? args.Bool(L"locked", true) : !ref->locked;
        SetTabLocked(id, want);
        return true;
    });
    reg(CmdId::TabCopyTitle, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        if (!id) return false;
        const core::TabRef* ref = tabSet_.Find(id);
        if (!ref) return false;
        return CopyText(core::DisplayTitle(*ref));
    });
    // 固定标签：语义与 tab.lock 同构（按值幂等，省略 = 切换）。
    // 给 pinned 而不是复用 locked：两者保护的东西不同 ——
    // 固定管"位置 + 不被批量/中键误关"，锁定管"连显式关闭也拒绝"。
    reg(CmdId::TabPin, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        if (!id) return false;
        const core::TabRef* ref = tabSet_.Find(id);
        if (!ref) return false;
        const bool want = args.Has(L"pinned") ? args.Bool(L"pinned", true) : !ref->pinned;
        SetTabPinned(id, want);
        return true;
    });
    reg(CmdId::TabSwitcher, [this](const Args&) {
        ShowTabSwitcher();
        return true;
    });
    // Ctrl+Tab / Ctrl+Shift+Tab：按 MRU 循环，而不是按位置序。
    // 位置序仍可用 Ctrl+PageDown/PageUp（tab.next / tab.prev）。
    reg(CmdId::TabCycleNext, [this](const Args&) { return CycleMru(true); });
    reg(CmdId::TabCyclePrev, [this](const Args&) { return CycleMru(false); });
    reg(CmdId::TabMoveNewWindow, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int id = tabSet_.IdAt(idx);
        const core::TabRef* ref = tabSet_.Find(id);
        if (!ref) return false;
        if (ref->locked || ref->pinned) {
            CommandBus::Get().NoteFailure(L"锁定/固定的标签不能移到新窗口");
            return false;
        }
        if (ref->path.empty()) {
            CommandBus::Get().NoteFailure(L"该标签没有可打开的路径");
            return false;
        }
        // 有损近似：ShellView 是进程内 COM 对象，不能跨进程搬。新实例按路径打开，
        // 源标签关闭；历史/视图状态不迁移（描述里已注明）。
        OpenPathInNewWindow(ref->path);
        return CloseTab(id);
    });
    reg(CmdId::TabReopenClosed, [this](const Args&) {
        // 先建后弹栈，失败时保留关闭历史（见 App::ReopenClosed）。
        return ReopenClosed(0);
    });
    // 四条移动命令都以"目标标签"为对象：index 来自右键菜单；省略 = 活动标签。
    // 前置 CanMoveTabLeft/Right 已按同一 index 判断（见 commands.cpp 的 CanExecute），
    // 两处口径必须一致，否则会出现"前置放行但 handler 越界"或反之。
    reg(CmdId::TabMoveLeft, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        MoveTab(tabSet_.IdAt(idx), idx - 1);
        return true;
    });
    reg(CmdId::TabMoveRight, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        MoveTab(tabSet_.IdAt(idx), idx + 1);
        return true;
    });
    reg(CmdId::TabMoveFirst, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        MoveTab(tabSet_.IdAt(idx), 0);
        return true;
    });
    reg(CmdId::TabMoveLast, [this](const Args& args) {
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        MoveTab(tabSet_.IdAt(idx), tabSet_.Count() - 1);
        return true;
    });
    reg(CmdId::TabMoveTo, [this](const Args& args) {
        // 绝对位置重排（拖拽的结构化入口）：to = 0 起目标下标（TabSet::Move 的
        // 「摘出再插入」落点）。left/right/first/last 是相对表达，这里补绝对表达。
        const int idx = args.Has(L"index") ? args.Int(L"index", -1) : ActiveIndex();
        if (idx < 0 || idx >= tabSet_.Count()) return false;
        const int to = args.Int(L"to", -1);
        if (to < 0 || to >= tabSet_.Count()) {
            CommandBus::Get().NoteFailure(L"目标位置越界：0.." + std::to_wstring(tabSet_.Count() - 1));
            return false;
        }
        const int id = tabSet_.IdAt(idx);
        MoveTab(id, to);
        return tabSet_.IdAt(to) == id;   // 原地不动是 no-op，报成功没问题
    });
    reg(CmdId::TabPrevTab, [this](const Args&) {
        int prev = tabSet_.PrevMru(activeId_);
        if (!prev) {
            // 没有上一个 MRU 时必须如实失败：以前这里恒返回 true，
            // 于是"回到上一个标签"在只有一个标签时也报成功 ——
            // 调用方以为切走了，其实还在原地。
            CommandBus::Get().NoteFailure(L"没有上一个激活过的标签");
            return false;
        }
        Activate(prev);
        return true;
    });
    reg(CmdId::TabLast, [this](const Args&) {
        if (tabSet_.Count()) Activate(tabSet_.IdAt(tabSet_.Count() - 1));
        return true;
    });
    reg(CmdId::CloseLeftOne, [this](const Args&) {
        int i = ActiveIndex();
        if (i > 0) CloseTab(tabSet_.IdAt(i - 1));
        return true;
    });
    reg(CmdId::CloseRightOne, [this](const Args&) {
        int i = ActiveIndex();
        if (i >= 0 && i + 1 < tabSet_.Count()) CloseTab(tabSet_.IdAt(i + 1));
        return true;
    });
    reg(CmdId::TabActivate, [this](const Args& args) {
        // index 是 **0 起**的标签序号，与 schema 的 "minimum":0 一致。
        // 这里曾有一行 `if (idx > 0) --idx;` 试图同时接受 1-based：结果是 0 和 1
        // 都指向第 1 个标签、最后一个永远够不到 —— 调用方按 schema 传值反而差一格，
        // 且 ok=true，属于最难查的"调 A 执行 B"。两种口径不能共存：需要 1 起语义
        // 的调用方用 tab.goto.1..5（内部各自减 1），不要污染这个参数。
        const int idx = args.Int(L"index");
        if (idx < 0 || idx >= tabSet_.Count()) return false;   // 越界如实报错，不假装成功
        Activate(tabSet_.IdAt(idx));
        return true;
    });
    reg(CmdId::TabNext, [this](const Args&) {
        if (tabSet_.Count() < 2) return true;
        Activate(tabSet_.IdAt((ActiveIndex() + 1) % tabSet_.Count()));
        return true;
    });
    reg(CmdId::TabPrev, [this](const Args&) {
        if (tabSet_.Count() < 2) return true;
        int n = tabSet_.Count();
        Activate(tabSet_.IdAt((ActiveIndex() - 1 + n) % n));
        return true;
    });
    // g 1 … g 9：序列表里一直绑着 GoTabN，但目录里没有这些命令，
    // 结果 9 条序列全是空动作（按了没反应、审计里只有"未知命令"）。
    // 显式逐个注册（而不是循环 + 数组）：tools/check_commands.py 按 reg(CmdId::X) 审计。
    auto goTab = [this](int i) {
        // 越界必须如实失败：以前是 `if (i < count) Activate(...); return true;` ——
        // 标签不够时"切到第 5 个"返回成功却什么都没切，
        // 调用方据此以为已经切走，后续操作全在错的标签上做。
        if (i >= tabSet_.Count()) {
            CommandBus::Get().NoteFailure(L"没有第 " + std::to_wstring(i + 1) + L" 个标签");
            return false;
        }
        Activate(tabSet_.IdAt(i));
        return true;
    };
    reg(CmdId::GoTab1, [goTab](const Args&) { return goTab(0); });
    reg(CmdId::GoTab2, [goTab](const Args&) { return goTab(1); });
    reg(CmdId::GoTab3, [goTab](const Args&) { return goTab(2); });
    reg(CmdId::GoTab4, [goTab](const Args&) { return goTab(3); });
    reg(CmdId::GoTab5, [goTab](const Args&) { return goTab(4); });
    reg(CmdId::GoTab6, [goTab](const Args&) { return goTab(5); });
    reg(CmdId::GoTab7, [goTab](const Args&) { return goTab(6); });
    reg(CmdId::GoTab8, [goTab](const Args&) { return goTab(7); });
    reg(CmdId::GoTab9, [goTab](const Args&) { return goTab(8); });

    // ---------------- 跳转 ----------------
    auto go = [this](const std::wstring& path) {
        if (!path.empty()) NavigateActive(path, true);
        return true;
    };
    reg(CmdId::GoHome, [go](const Args&) { return go(KnownFolder(FOLDERID_Profile)); });
    reg(CmdId::NavHome, [go](const Args&) { return go(KnownFolder(FOLDERID_Profile)); });
    reg(CmdId::GoDesktop, [go](const Args&) { return go(KnownFolder(FOLDERID_Desktop)); });
    reg(CmdId::GoDownloads, [go](const Args&) { return go(KnownFolder(FOLDERID_Downloads)); });
    reg(CmdId::GoDocuments, [go](const Args&) { return go(KnownFolder(FOLDERID_Documents)); });
    reg(CmdId::GoLocalAppData, [go](const Args&) { return go(KnownFolder(FOLDERID_LocalAppData)); });
    reg(CmdId::GoStartFolder, [go](const Args&) { return go(KnownFolder(FOLDERID_StartMenu)); });
    reg(CmdId::GoConfigPath, [go](const Args&) { return go(Config::Get().Dir()); });
    reg(CmdId::GoThisPC, [go](const Args&) { return go(nav::kThisPc); });
    reg(CmdId::GoControlPanel, [go](const Args&) {
        return go(L"::{26EE0668-A00A-44D7-9371-BEB064C98683}");
    });
    reg(CmdId::GoPowerShell, [this](const Args&) {
        // 名字（go.powershell）、分组（Go=跳转到已知位置）、标题（"PowerShell 目录"）
        // 三处说的都是"去这个目录"。以前实现是 RunTerminal(当前目录) —— 起终端、
        // 不导航，名实不符，且与 ext.terminal 完全重复。
        // 起终端归 ext.terminal，这条只做导航。
        wchar_t win[MAX_PATH] = L"";
        if (!GetWindowsDirectoryW(win, MAX_PATH)) return false;
        std::wstring dir = std::wstring(win) + L"\\System32\\WindowsPowerShell\\v1.0";
        if (!tf::IsDirectory(dir)) {
            dir = std::wstring(win) + L"\\System32";   // 旧系统没有 v1.0 子目录
            if (!tf::IsDirectory(dir)) return false;
        }
        return NavigateActive(dir, true);
    });

    // ---------------- 导航 ----------------
    reg(CmdId::NavBack, [this](const Args&) { GoBack(); return true; });
    reg(CmdId::NavForward, [this](const Args&) { GoForward(); return true; });
    reg(CmdId::NavUp, [this](const Args&) { GoUp(); return true; });
    reg(CmdId::NavRefresh, [this](const Args&) {
        RefreshActive();
        // 空白区菜单也挂这条命令：刷新既有活动视图，也要让导航树自己重发现。
        if (navPane_.ok() && navPane_.visible()) navPane_.Refresh();
        return true;
    });
    reg(CmdId::CmdRefresh, [this](const Args&) { RefreshActive(); return true; });
    reg(CmdId::NavViewMenu, [this](const Args&) { ShowViewMenuAt(); return true; });

    // ---------------- 路径 ----------------
    reg(CmdId::PathCopy, [this](const Args&) {
        auto paths = SelectedOrFolder();
        std::wstring all;
        for (const auto& p : paths) {
            if (!all.empty()) all += L"\r\n";
            all += p;
        }
        return CopyText(all);
    });
    reg(CmdId::PathCopyName, [this](const Args&) {
        auto paths = SelectedOrFolder();
        std::wstring all;
        for (const auto& p : paths) {
            if (!all.empty()) all += L"\r\n";
            all += LeafOf(p);
        }
        return CopyText(all);
    });
    // 复制当前目录：把写入的内容一并返回。剪贴板没有读回面（不打算给 AI 读
    // 剪贴板——里面有密码），所以"到底复制了什么"只能由命令自己回报。
    regR(CmdId::PathCopyDir, [this](const Args&, json::Value* out) {
        const std::wstring text = ActivePath();
        const bool okc = CopyText(text);
        if (okc && out) out->Set(L"text", json::Value::MakeStr(text));
        return okc;
    });
    reg(CmdId::PathCopyUnc, [this](const Args&) {
        auto paths = SelectedOrFolder();
        std::wstring all;
        for (const auto& p : paths) {
            if (!all.empty()) all += L"\r\n";
            all += ToUnc(p);
        }
        return CopyText(all);
    });
    reg(CmdId::PathCopyExt, [this](const Args&) {
        auto paths = SelectedOrFolder();
        std::wstring all;
        for (const auto& p : paths) {
            const wchar_t* ext = PathFindExtensionW(p.c_str());
            if (!all.empty()) all += L"\r\n";
            all += (ext && *ext) ? ext + 1 : L"";
        }
        return CopyText(all);
    });
    regR(CmdId::PathCopyTab, [this](const Args&, json::Value* out) {
        std::wstring all;
        for (const auto& t : tabSet_.items()) {
            if (!all.empty()) all += L"\r\n";
            all += t.path;
        }
        const bool okc = CopyText(all);
        if (okc && out) out->Set(L"text", json::Value::MakeStr(all));
        return okc;
    });
    // 剪贴板里放的是 .lnk 虚拟文件而不是路径文本：粘贴到资源管理器得到
    // 真正的快捷方式。落点只到剪贴板，不改当前目录的任何文件。
    regR(CmdId::PathCopyLink, [this](const Args&, json::Value* out) {
        std::vector<std::wstring> names;
        const bool ok = shortcut_clipboard::CopyAsShortcut(SelectedOrFolder(), &names);
        if (ok && out) {
            json::Value arr = json::Value::MakeArray();
            for (const auto& n : names) arr.arr.push_back(json::Value::MakeStr(n));
            out->Set(L"names", arr);
            out->Set(L"count", json::Value::MakeInt(static_cast<int>(names.size())));
        }
        return ok;
    });

    // ---- 关闭历史 ----
    // 列表只读：菜单/命令面板/AI 都从这里拿"第几条"，再用 tab.reopen.index 打开。
    // 没有它，"最近关闭"就只能是一个盲盒按钮（用户不知道会开出哪个目录）。
    regR(CmdId::TabClosedList, [this](const Args&, json::Value* out) {
        json::Value paths = json::Value::MakeArray();
        for (const auto& p : tabSet_.closed()) paths.arr.push_back(json::Value::MakeStr(p));
        out->Set(L"paths", paths);
        out->Set(L"count", json::Value::MakeInt(static_cast<int>(tabSet_.closed().size())));
        return true;
    });
    regR(CmdId::TabReopenAt, [this](const Args& args, json::Value* out) {
        const int idx = args.Int(L"index", -1);
        if (idx < 0 || idx >= static_cast<int>(tabSet_.closed().size())) {
            CommandBus::Get().NoteFailure(L"关闭历史里没有第 " + std::to_wstring(idx) + L" 条");
            return false;
        }
        if (!ReopenClosed(idx)) return false;   // 失败原因已由 ReopenClosed 给出
        out->Set(L"path", json::Value::MakeStr(ActivePath()));
        return true;
    });

    // ---- 导航栏历史（后端下拉菜单的结构化版本）----
    // 地址框下拉里的历史此前只有鼠标能看能点：AI 只能盲按 nav.back 再读回
    // 路径，想跳第 3 条得试三次。列表先给出来，"会退到哪"就可核对。
    regR(CmdId::NavHistoryList, [this](const Args& args, json::Value* out) {
        if (!out) return false;
        const bool backward = args.Bool(L"backward", true);
        const std::vector<std::wstring> paths = HistoryList(backward);
        json::Value arr = json::Value::MakeArray();
        for (const auto& p : paths) arr.arr.push_back(json::Value::MakeStr(p));
        out->Set(L"backward", json::Value::MakeBool(backward));
        out->Set(L"paths", arr);
        out->Set(L"count", json::Value::MakeInt(static_cast<int>(paths.size())));
        return true;
    });
    regR(CmdId::NavHistoryJump, [this](const Args& args, json::Value* out) {
        if (!out) return false;
        const int depth = args.Int(L"depth", -1);
        const bool backward = args.Bool(L"backward", true);
        if (depth < 0) {
            CommandBus::Get().NoteFailure(L"缺少必填参数：depth");
            return false;
        }
        // 越界先拒绝：GotoHistory 对越界是静默忽略（按钮灰显已挡过一层），
        // 机器侧拿到"成功但没动"就会误以为跳过了。
        const std::vector<std::wstring> list = HistoryList(backward);
        const int n = static_cast<int>(list.size());
        if (depth >= n) {
            CommandBus::Get().NoteFailure(L"历史条目不够：只有 " + std::to_wstring(n) + L" 条");
            return false;
        }
        // to 取**历史条目**而不是跳完再读 ActivePath()：导航是异步的（Shell 回调
        // 才写路径），立刻回读只会读到出发地 —— 那正是"回报成功但内容不对"。
        const std::wstring from = ActivePath();
        const std::wstring to = list[depth];
        JumpHistory(depth, backward);
        out->Set(L"from", json::Value::MakeStr(from));
        out->Set(L"to", json::Value::MakeStr(to));
        return true;
    });
    regR(CmdId::NavHistoryClear, [this](const Args&, json::Value* out) {
        if (!out) return false;
        // 与地址框下拉底部「清除历史记录」同一实现（App::ClearHistory）。
        const int recentCleared = ClearHistory();
        out->Set(L"path", json::Value::MakeStr(ActivePath()));
        out->Set(L"recentCleared", json::Value::MakeInt(recentCleared));
        return true;
    });
    regR(CmdId::NavRecentList, [this](const Args&, json::Value* out) {
        if (!out) return false;
        const auto& recent = Config::Get().s().recentPaths;
        json::Value arr = json::Value::MakeArray();
        for (const auto& p : recent) arr.arr.push_back(json::Value::MakeStr(p));
        out->Set(L"paths", arr);
        out->Set(L"count", json::Value::MakeInt(static_cast<int>(recent.size())));
        return true;
    });
}
