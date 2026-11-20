// 窗口/系统、外部程序、书签、会话命令。
//
// 零散但共用一处：都是"进程级"动作（开新窗口、起终端、改配置、读写会话），不涉及单个视图。
#include "app.h"

#include "app_commands.h"
#include "hotkey_manager.h"
#include "bookmarks.h"

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


void App::RegisterMiscCommands() {
    auto reg = cmd_reg::Register;

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
        std::wstring id;
        if (!store.ContainsPath(path)) id = store.AddPath(LeafOf(path), path);
        bool saved = store.Save();
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
