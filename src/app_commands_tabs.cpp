// 标签 / 跳转 / 导航 / 路径 四组命令。
//
// 共同点：只动"当前标签集合与历史栈"，不碰 IShellView 里的文件操作。
#include "app.h"

#include "app_commands.h"

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
    // 少数带 index 的命令按序号取标签；省略时仍作用于当前标签。
    auto tabIdFromArgs = [this](const Args& args) {
        if (!args.Has(L"index")) return activeId_;
        const int index = args.Int(L"index", -1);
        return (index >= 0 && index < tabSet_.Count()) ? tabSet_.IdAt(index) : 0;
    };

    // ---------------- 标签 ----------------
    reg(CmdId::TabNew, [this](const Args& args) {
        // 与"+"按钮（WM_APP_TAB_NEW）同一口径：省略 path 时打开此电脑，
        // 不是沿用当前目录 —— 后者由 tab.clone 负责。两条路径语义分开，
        // 人和 AI 说"新建标签"得到的是同一个结果（R20 人机等价）。
        // path 参数照旧支持：调用方明确给了目录就按目录开（此前 handler 丢弃它，
        // 现已修复；这里保留该能力，不因改默认值而回退）。
        std::wstring p = args.Str(L"path");
        NewTab(p.empty() ? nav::kThisPc : p, NewTabIndex());
        return true;
    });
    reg(CmdId::TabClose, [this](const Args&) {
        // 透传真实结果：锁定标签会拒绝关闭，那时不能报 ok=true。
        // 前置已要求 HasMultipleTabs，所以这里不会走到"关掉最后一个 = 退出程序"。
        return CloseTab(activeId_);
    });
    reg(CmdId::TabCloseOther, [this](const Args&) {
        // 如实回传：所有其他标签都锁定时一个也没关，不能报 ok=true。
        return CloseOthers(activeId_) > 0;
    });
    reg(CmdId::TabCloseLeft, [this](const Args&) {
        return CloseSide(activeId_, true) > 0;
    });
    reg(CmdId::TabCloseRight, [this](const Args&) {
        return CloseSide(activeId_, false) > 0;
    });
    reg(CmdId::TabClone, [this](const Args&) {
        NewTab(ActivePath(), NewTabIndex());
        return true;
    });
    // ---- 标签个性化：与右键菜单共用同一份实现（R6：动作只有一份）----
    // 菜单路径直接调 RenameTab/SetTabColor；命令路径经这里转一道，
    // 两条路上"改名的落盘口径"完全一致（否则会出现菜单改名能存、面板改名存不住）。
    reg(CmdId::TabRename, [this](const Args& args) {
        if (!tabSet_.Find(activeId_)) return false;
        RenameTab(activeId_, args.Str(L"name"));
        return true;
    });
    reg(CmdId::TabSetColor, [this](const Args& args) {
        if (!tabSet_.Find(activeId_)) return false;
        const int color = args.Int(L"color", 0);
        if (color < 0 || color >= Theme::kTabColorCount) return false;  // 越界如实报错
        SetTabColor(activeId_, color);
        return true;
    });
    reg(CmdId::TabCloseDuplicates, [this](const Args&) {
        if (!tabSet_.Find(activeId_)) return false;
        // 返回"是否真的关掉了至少一个"：一个重复都没有时应当是 false（ok=0），
        // 否则审计里会记成成功，AI 也以为自己清理过了。
        return CloseDuplicates(activeId_) > 0;
    });

    // ---- 标签锁定 / 复制标签名 ----
    // 这两条原先只存在于右键菜单的 switch 里（App::ShowTabMenu），
    // 于是键盘、命令面板、脚本、AI 都做不到 —— 只有鼠标能做。
    //
    // 目标标签：给了 index 就用它，省略则用当前标签。**不新增 activeId_ 之外的
    // 隐式状态**：index 越界如实报 false，不"就近取一个"（那会让调用方以为
    // 操作了自己指定的那个标签）。
    //
    // 为什么每条 handler 自己写 `args.Has(L"index")` 而不抽公共 lambda：
    // 静态检查靠"handler 里出现了 args.X(L"键")"来核对 schema 与实现一致
    // （check_ai_meta 第 3 项）。把取值藏进辅助函数会让那条检查失效 ——
    // 而它挡的正是"schema 声明了参数、handler 却没收，参数被静默丢弃"。
    // 宁可重复一行，也不让判据看不见。
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
        CopyText(core::DisplayTitle(*ref));
        return true;
    });
    reg(CmdId::TabReopenClosed, [this](const Args&) {
        std::wstring path;
        if (tabSet_.PopClosed(&path)) NewTab(path);
        return true;
    });
    reg(CmdId::TabMoveLeft, [this](const Args&) {
        MoveTab(activeId_, ActiveIndex() - 1);
        return true;
    });
    reg(CmdId::TabMoveRight, [this](const Args&) {
        MoveTab(activeId_, ActiveIndex() + 1);
        return true;
    });
    reg(CmdId::TabMoveFirst, [this](const Args&) {
        MoveTab(activeId_, 0);
        return true;
    });
    reg(CmdId::TabMoveLast, [this](const Args&) {
        MoveTab(activeId_, tabSet_.Count() - 1);
        return true;
    });
    reg(CmdId::TabPrevTab, [this](const Args&) {
        int prev = tabSet_.PrevMru(activeId_);
        if (prev) Activate(prev);
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
        //
        // 这里曾有一行 `if (idx > 0) --idx;  // 允许 1-based`，试图同时接受两种口径。
        // 结果是 0 和 1 都指向第 1 个标签、而最后一个标签用 0 起序号永远够不到
        // （必须传 tabCount 才能命中）—— 调用方按 schema 传值反而差一格，且
        // ok=true、审计无异常，属于最难查的"调 A 执行 B"。
        //
        // 两种口径不能在一个参数里共存：宁可只认 schema 写的那一种。需要
        // 1 起语义的调用方用 tab.goto.1..5（它们内部各自减 1），不要污染这个参数。
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
        if (i < tabSet_.Count()) Activate(tabSet_.IdAt(i));
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
        RunTerminal(hwnd_, ActivePath(), false);
        return true;
    });

    // ---------------- 导航 ----------------
    reg(CmdId::NavBack, [this](const Args&) { GoBack(); return true; });
    reg(CmdId::NavForward, [this](const Args&) { GoForward(); return true; });
    reg(CmdId::NavUp, [this](const Args&) { GoUp(); return true; });
    reg(CmdId::NavRefresh, [this](const Args&) { RefreshActive(); return true; });
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
        CopyText(all);
        return true;
    });
    reg(CmdId::PathCopyName, [this](const Args&) {
        auto paths = SelectedOrFolder();
        std::wstring all;
        for (const auto& p : paths) {
            if (!all.empty()) all += L"\r\n";
            all += LeafOf(p);
        }
        CopyText(all);
        return true;
    });
    reg(CmdId::PathCopyDir, [this](const Args&) {
        CopyText(ActivePath());
        return true;
    });
    reg(CmdId::PathCopyUnc, [this](const Args&) {
        auto paths = SelectedOrFolder();
        std::wstring all;
        for (const auto& p : paths) {
            if (!all.empty()) all += L"\r\n";
            all += ToUnc(p);
        }
        CopyText(all);
        return true;
    });
    reg(CmdId::PathCopyExt, [this](const Args&) {
        auto paths = SelectedOrFolder();
        std::wstring all;
        for (const auto& p : paths) {
            const wchar_t* ext = PathFindExtensionW(p.c_str());
            if (!all.empty()) all += L"\r\n";
            all += (ext && *ext) ? ext + 1 : L"";
        }
        CopyText(all);
        return true;
    });
    reg(CmdId::PathCopyTab, [this](const Args&) {
        std::wstring all;
        for (const auto& t : tabSet_.items()) {
            if (!all.empty()) all += L"\r\n";
            all += t.path;
        }
        CopyText(all);
        return true;
    });
}
