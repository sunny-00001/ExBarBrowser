// 跳转 / 导航 / 路径 三组命令（从 app_commands_tabs.cpp 按职责拆出）。
//
// 与原文件的分工：那边只管"标签集合"本身（新建/关闭/移动/激活/颜色/锁定）；
// 这里管"导航到哪里"（go.* 已知位置、nav.* 前进后退、path.* 复制路径）。
// 两组的入参、前置检查、失败语义都不同，混在一起时读的人要翻到文件末尾
// 才发现还有另一套语义（AGENTS.md §3「按作用域拆，不按行数拆」）。
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

void App::RegisterGoCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;
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
