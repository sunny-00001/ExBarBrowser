// 窗口/系统、外部程序、书签、会话命令。
//
// 零散但共用一处：都是"进程级"动作（开新窗口、起终端、改配置、读写会话），不涉及单个视图。
#include "app.h"

#include "app_commands.h"
#include "config_apply.h"   // 设置项 -> 落地动作的判据表（纯函数，可单测）
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

// 视图/布局/状态栏/主题/历史的落地判据收在 config_apply.h（纯函数、可单测）。
// 【为什么不是这里的文件级 static】这套判据以前散在本文件的一串 if/else if 里，
// 漏一个键的后果是"命令 ok=true、界面没变"（实测：config.set bookmark=false
// 写盘成功、书签栏仍 visible=True）。抽成纯函数之后，单测能拿**全部可写键**
// 逐个问"它被哪个落地类别覆盖"，漏项就从"运行期玄学"变成"编译期用例红"。

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
    // 现在是独立的设置窗口，与导航栏那一条路径弹的是同一个窗口实例。
    reg(CmdId::Preferences, [this](const Args&) {
        ShowSettingsDialog();
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
    // 书签这一族（收藏夹增删改查、拖放/重排的结构化入口）自成一体，
    // 注册实现见 app_commands_misc_bookmarks.cpp。
    RegisterBookmarkCommands();

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
        // 写盘失败不是"命令失败"：值**已经**改了、也已经生效了，只是下次启动会
        // 变回去。所以走告警通道（NoteWarning）而不是 NoteFailure —— 报成失败会
        // 让调用方以为改动没生效、去重试或回滚，那是另一种撒谎。
        if (!Config::Get().Save())
            CommandBus::Get().NoteWarning(L"已在本次运行中生效，但没能写盘：" +
                                          Config::Get().lastSaveError());
        // 刷新放在命令层：只有这里同时知道设置项和窗口。
        //
        // 【两条分支，不是一串 else if】"套用"分两类：
        //   · 有副作用的（SetDualPane 要建右视图、ApplyTheme 要重建字体画刷）——
        //     这些必须先做，且各自不能漏；
        //   · 只改布局的（栏显隐 / 宽度 / 分栏比例）—— 统一收在 AffectsLayout，
        //     最后一次性 Layout()。
        // 以前全是 else if 链，两条以上的键同时相关时只有第一条会跑，
        // 而漏掉的那项表现为"命令 ok=true、界面没变"（见 AffectsLayout 的实测记录）。
        // 栏高：先推给 Theme，后面的 SyncChrome/Layout 才会按新高度算。
        // 与启动时走同一个 App::ApplyBarHeights，避免两条路各写一次。
        if (cfgapply::AffectsLayout(key)) ApplyBarHeights();
        // 导航栏内宽与导航栏搜索框由 NavBar::Layout 现读配置；ReloadTheme 会
        // 重排它（顺带刷新 tooltip 宽度），不需要重建整条栏。
        if (key == L"navBarSearchWidth" || key == L"navBarAddressMinWidth") navBar_.ReloadTheme();
        // 导航树行距/缩进：TVM_* 可直接改，不必重建树。
        if (key == L"navPaneItemHeight" || key == L"navPaneIndent") navPane_.RefreshTreeMetrics();
        // 两条栏的提示窗最大宽：tooltip 在 ReloadTheme 里按配置重建。
        if (key == L"bookmarkBarTooltipWidth") bookmarkBar_.ReloadTheme();
        if (key == L"statusBarTooltipWidth") statusBar_.ReloadTheme();
        if (cfgapply::AffectsActiveView(key)) {
            ApplyPersistedViewState(ActiveShellViewHost());
            ApplyPaneChrome();
        }
        // 有副作用的套用（互不排斥，所以不是 else if 链）。
        // 双栏/分栏：SetDualPane 与 SetPaneSplit 内部各自 Save+Layout，按值设定幂等。
        if (key == L"dualPane") SetDualPane(Config::Get().s().dualPane);
        if (key == L"paneSplit") SetPaneSplit(Config::Get().s().paneSplit);
        // 方向：SetPaneVertical 是翻转语义（同值就短路），而这里新值**已经**写进
        // Settings 了 —— 调它等于什么都不做，所以走"按当前值落地"那个入口。
        if (key == L"paneVertical") ApplyPaneVertical();
        // 全局导航树的显隐只有一个开关（showNavPane）：它不再挂在任何窗格上，
        // 所以必须在这里落地 —— 否则改了设置要重启才看见。
        if (key == L"showNavPane") ApplyNavPane();
        // 深浅色档位：改完立刻重套主题（字体/画刷/四条栏配色全换）。
        // 与"系统切了深浅色"走同一条 ApplyTheme —— 两条路各写一份重建逻辑，
        // 迟早出现"跟随系统能换、强制档换不了"这种半截效果。
        if (cfgapply::AffectsTheme(key)) ApplyTheme();
        // 历史上限：三个上限都要推到各自的持有者（recent/search 在 Config，
        // closedTab 在 TabSet），并顺手把已有条目裁到新上限。
        // 以前 closedTabLimit 是一条独立分支，与另两条各写一遍 —— 收进判据表后
        // 只有一处，”新加第四种历史“只会漏在明面上。
        if (cfgapply::AffectsHistoryLimits(key)) ApplyHistoryLimits();
        // 状态栏段显隐：段集变了要重新喂给栏（Layout 由下面统一做）。
        if (cfgapply::AffectsStatusBar(key)) statusBar_.SetEnabledSegments(Config::Get().s().statusSegs);

        SyncChrome();
        // 布局最后做：它读的是 Settings 的**最终值**（上面的分支可能又改了字段，
        // 比如 SetDualPane 会再写一次 dualPane），早做一次就白排、还可能排错。
        if (cfgapply::AffectsLayout(key)) Layout();
        // AI 通道热插拔：设置已落盘，真正启停交给独立的 WM_APP_SYNC_AI。
        // 不能在这里直接 StopServer —— 关闭请求本身可能来自该通道，
        // 同步 join 监听线程会让响应写不回去。人的显式设置优先于 --ai 强制开关。
        if (key == L"aiEnabled") {
            Config::Get().SetAiForceEnabled(false);
            PostMessageW(hwnd_, WM_APP_SYNC_AI, 0, 0);
        }
        // 设置面板回灌：外部（AI / 命令面板 / 状态栏右键菜单 / 快捷键）改了配置，
        // 已经打开的面板必须跟上 —— 否则它显示的是**上一轮**的值，用户看到的
        // 下拉写着 system、实际已经是 dark。
        //
        // 【面板自己的写入也会走到这里，那没关系】OnConfigChangedExternally 只做
        // 三件事：重读值、Layout()（只 MoveWindow 行内编辑框，不碰它的文字与焦点）、
        // 重绘。所以它不会打断"输到一半的数字"—— 面板自己 RefreshValues 之后
        // 再走一遍是幂等的。不必为此加"跳过自己"的判据。
        settingsDialog_.OnConfigChangedExternally();
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
            if (!CommandBus::Get().IsCallConfirmed() && MessageBoxW(hwnd_, msg.c_str(), L"清除标签组",
                            MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
                return false;   // 用户取消：什么都没做（总线问过就别再问一遍：两道门）
            }
        }

        Config::Get().RememberSession({}, -1);
        Config::Get().Save();
        result->Set(L"cleared", json::Value::MakeBool(n > 0));
        result->Set(L"count", json::Value::MakeInt(n));
        return true;
    });
}
