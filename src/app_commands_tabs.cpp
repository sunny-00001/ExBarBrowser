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
    if (p.size() >= 2 && p[1] == L':') {
        std::wstring drive(1, p[0]);
        return L"\\\\localhost\\" + drive + L"$" + p.substr(2);
    }
    return p;
}

}  // namespace


void App::RegisterTabCommands() {
    auto reg = cmd_reg::Register;

    // ---------------- 标签 ----------------
    reg(CmdId::TabNew, [this](const Args& args) {
        // path 省略时沿用当前目录（与"新建标签"按钮的行为一致）。
        // 以前这个参数只在 schema 里声明、handler 没读，属于"参数被静默丢弃"：
        // 调用方传了 path 却得到当前目录，是排查成本很高的一类错误。
        std::wstring p = args.Str(L"path");
        NewTab(p.empty() ? ActivePath() : p, NewTabIndex());
        return true;
    });
    reg(CmdId::TabClose, [this](const Args&) {
        CloseTab(activeId_);
        return true;
    });
    reg(CmdId::TabCloseOther, [this](const Args&) {
        CloseOthers(activeId_);
        return true;
    });
    reg(CmdId::TabCloseLeft, [this](const Args&) {
        CloseSide(activeId_, true);
        return true;
    });
    reg(CmdId::TabCloseRight, [this](const Args&) {
        CloseSide(activeId_, false);
        return true;
    });
    reg(CmdId::TabClone, [this](const Args&) {
        NewTab(ActivePath(), NewTabIndex());
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
        int idx = args.Int(L"index");
        if (idx > 0) --idx;  // 允许 1-based
        if (idx >= 0 && idx < tabSet_.Count()) Activate(tabSet_.IdAt(idx));
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
    reg(CmdId::GoHome, [this, go](const Args&) { return go(KnownFolder(FOLDERID_Profile)); });
    reg(CmdId::NavHome, [this, go](const Args&) { return go(KnownFolder(FOLDERID_Profile)); });
    reg(CmdId::GoDesktop, [this, go](const Args&) { return go(KnownFolder(FOLDERID_Desktop)); });
    reg(CmdId::GoDownloads, [this, go](const Args&) { return go(KnownFolder(FOLDERID_Downloads)); });
    reg(CmdId::GoDocuments, [this, go](const Args&) { return go(KnownFolder(FOLDERID_Documents)); });
    reg(CmdId::GoLocalAppData, [this, go](const Args&) { return go(KnownFolder(FOLDERID_LocalAppData)); });
    reg(CmdId::GoStartFolder, [this, go](const Args&) { return go(KnownFolder(FOLDERID_StartMenu)); });
    reg(CmdId::GoConfigPath, [this, go](const Args&) { return go(Config::Get().Dir()); });
    reg(CmdId::GoThisPC, [this, go](const Args&) { return go(nav::kThisPc); });
    reg(CmdId::GoControlPanel, [this, go](const Args&) {
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
