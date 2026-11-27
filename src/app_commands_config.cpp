// config.* 命令组：设置项的枚举、读写、导出与还原。
//
// 【为什么单独成文件】设置域的 handler 有一个别的域没有的特点：它们要落地到
// **界面**（栏高、主题、窗格、状态栏段…），也就是要碰 App 的十来个成员。
// 混在 app_commands_misc.cpp（窗口/外部/书签/会话混放）里时，那一坨落地逻辑
// 把文件推过了 500 行欠账线；而"设置域自己一个文件"也让"改设置要动哪几处"
// 一眼可见 —— 读这个文件的人不必在杂事堆里翻。
//
// 【一条硬纪律：读写都走 settings_reg 的同一份键表】界面、AI、导出文件
// 三处都从那张表取，不另存副本。两份副本一旦分叉，"命令改了 A、界面显示 B"
// 就只有用户能发现（实测过：config.set bookmark=false 写盘成功、书签栏仍可见）。
#include "app.h"

#include "app_commands.h"
#include "config_apply.h"   // 设置项 -> 落地动作的判据表（纯函数，可单测）
#include "nav_path.h"       // ParentOf / Normalize：导出文件的目录处理
#include "settings_reg.h"
#include "text_file.h"      // tf::WriteText / ReadText：配置导出与还原走同一套读写

namespace {

// 路径是不是"已经写全了"的绝对路径。
//
// 【为什么不用 GetFullPathName】那玩意儿按**当前工作目录**展开相对路径，
// 而本进程的 CWD 随启动方式变（资源管理器里启动就是系统目录）——
// 同一句"导出到 backup.json"在不同启动方式下落到不同地方，
// 而用户以为它落在配置目录。这里自己判：只有盘符 / UNC / 命名空间才算绝对。
bool LooksAbsolutePath(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return true;            // C:\... / C:...
    if (p.rfind(L"\\\\", 0) == 0) return true;                  // UNC
    if (p.rfind(L"\\\\?\\", 0) == 0 || p.rfind(L"\\??\\", 0) == 0) return true;
    if (p.rfind(L"::", 0) == 0) return true;                    // 命名空间
    return false;
}

// 导出/还原用的文件路径：相对路径按**配置目录**解析（不是 CWD）。
//
// 【为什么固定成配置目录】导出与还原是一对：用户导出一份、换台机器还原，
// 相对路径必须落在同一个可预期的地方。跟着 CWD 走的话，
// "在资源管理器里启动"与"在终端里启动"会落到两个地方，备份就找不到了。
std::wstring ResolveConfigFilePath(const std::wstring& raw) {
    const std::wstring p = nav::Normalize(str::Trim(raw));
    if (p.empty()) return p;
    if (LooksAbsolutePath(p)) return p;
    return tf::Join(Config::Get().Dir(), p);
}

}  // namespace

void App::RegisterConfigCommands() {
    auto reg = cmd_reg::Register;
    // 产出结构化结果的命令用 regR（检查脚本按 `regR(CmdId::` 字面扫描"谁注册了谁"，
    // 所以不能写 cmd_reg::RegisterWithResult，也不能靠循环传 id）。
    auto regR = cmd_reg::RegisterWithResult;

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
        // 快捷键管理器面板尺寸（hkRowHeight / hkRailWidth / hkDetailWidth /
        // hkWindowWidth / hkWindowHeight）。工具栏、表头、状态栏都吃 hkRowHeight。
        //
        // 【为什么必须在这里推一次】它的 Layout() 是**按配置现算**的，但没人叫它
        // 就不会重排 —— 表现是"config.set hkRowHeight 32 返回 ok、界面还是 28"，
        // 而这类"命令成功但没变化"最难排查（看起来像命令没生效）。
        // 窗口宽高只影响下次打开的初始尺寸，不必动已开的窗口（用户的窗口大小
        // 是他自己调的，被一次设置覆盖掉是越权）。
        if (cfgapply::AffectsHotkeyPanel(key)) hotkeyManager_.ReloadTheme();
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

    // ---------------- 配置导出 / 还原（Opus 的 .ocb 等价物）----------------
    //
    // 【为什么导的是 settings_reg 而不是 settings.json】settings.json 里除了
    // 偏好，还有大量"运行时状态"（最近路径、关闭的标签、窗口几何）——
    // 那些不是"设置"，备份它们会把窗口位置也一起搬走。
    // settings_reg 的键表才是"设置"：哪些项可写、类型是什么、范围多大，
    // 全在这一份里，导出与还原都从它取，两边不可能对不上。
    regR(CmdId::ConfigExport, [](const Args& args, json::Value* out) -> bool {
        if (!out) return false;
        json::Value settings = json::Value::MakeObject();
        int count = 0;
        for (const auto& e : settings_reg::Entries()) {
            // 只导**可写**项：只读项是算出来的（当前路径、目录大小这类），
            // 备下来还原时用不上，导入时反而会刷出一堆"只读项不能写"的噪音。
            if (!e.set) continue;
            json::Value v;
            if (!settings_reg::Get(e.key, &v)) continue;
            settings.Set(e.key, std::move(v));
            ++count;
        }
        json::Value root = json::Value::MakeObject();
        root.Set(L"format", json::Value::MakeStr(L"exbar-settings"));
        root.Set(L"version", json::Value::MakeInt(1));
        root.Set(L"settings", std::move(settings));
        root.Set(L"count", json::Value::MakeInt(count));

        const std::wstring raw = str::Trim(args.Str(L"path"));
        if (raw.empty()) {
            // 不给 path = 只回显，不落盘：纯读，AI 直接读这份内容最省事。
            root.Set(L"path", json::Value::MakeStr(L""));
            *out = std::move(root);
            return true;
        }
        const std::wstring path = ResolveConfigFilePath(raw);
        // 已有文件**不覆盖**：导出是"备份"，把用户上一次的备份冲掉是不可逆的。
        // 要覆盖就显式传 overwrite=true —— 覆盖是意图，不该是默认。
        if (tf::Exists(path) && !args.Bool(L"overwrite")) {
            CommandBus::Get().NoteFailure(
                L"文件已存在，不覆盖（要覆盖请显式传 overwrite）：" + path);
            return false;
        }
        const std::wstring parent = nav::ParentOf(path);
        if (!parent.empty() && !tf::EnsureDir(parent)) {
            CommandBus::Get().NoteFailure(L"建不出目录：" + parent);
            return false;
        }
        if (!tf::WriteText(path, json::Stringify(root, 2))) {
            CommandBus::Get().NoteFailure(L"写入失败：" + path);
            return false;
        }
        root.Set(L"path", json::Value::MakeStr(path));
        *out = std::move(root);
        return true;
    });

    // 还原：**逐项**走 config.set —— 于是每一项都经过同一套校验
    // （类型、范围、枚举白名单）、同一套界面落地、同一份审计记录，
    // 与"人一条条改设置"完全同源。
    //
    // 【为什么不用另一条"批量写"的捷径】批量写省事，但那样会出现"导入说成功、
    // 界面没变"或"一半成功一半失败却报成功"。逐项走总线，每项的成败都能单独
    // 说出来 —— 导入失败时用户拿到的是"哪一项、为什么"，而不是一句"导入失败"。
    regR(CmdId::ConfigImport, [this](const Args& args, json::Value* out) -> bool {
        if (!out) return false;
        const std::wstring raw = str::Trim(args.Str(L"path"));
        if (raw.empty()) {
            CommandBus::Get().NoteFailure(L"缺少 path：要还原哪个文件");
            return false;
        }
        const std::wstring path = ResolveConfigFilePath(raw);
        std::wstring text;
        if (!tf::ReadText(path, &text)) {
            CommandBus::Get().NoteFailure(L"读不到文件：" + path);
            return false;
        }
        json::Value doc;
        std::wstring perr;
        if (!json::Parse(text, doc, &perr)) {
            // 解析失败 = **一个字都没改**。这里必须先失败再谈逐项：
            // 拿半份 JSON 去逐项还原，改到一半才发现文件是坏的，
            // 用户的设置就停在一个"改了一半"的状态上。
            CommandBus::Get().NoteFailure(L"不是合法的 JSON：" + perr);
            return false;
        }
        // 两种形状都吃：导出文件的 {"settings":{...}}，以及手写的扁平 {...}。
        // 只认一种形状的话，"手写一个只含三项的配置文件"这种最自然的用法就不成立。
        const json::Value* bag = doc.Find(L"settings");
        if (!bag || !bag->IsObject()) bag = &doc;
        if (!bag->IsObject()) {
            CommandBus::Get().NoteFailure(L"文件里没有设置项对象");
            return false;
        }
        json::Value applied = json::Value::MakeArray();
        json::Value failed = json::Value::MakeArray();
        int total = 0;
        // 跨字段约束有先后：tabMinWidth 不得高于 tabMaxWidth，二者的校验都在
        // 写入这一刻读对方的当前值。导出文件按表的顺序（min 在 max 前）排列，
        // 逐项还原时会拿"还没还原的旧 max"去校验新的 min —— 合法的一对
        // （300/420）在默认机器（112/230）上就会先拒掉 min，再设 max，
        // 结果是 112/420 却报成功。这里把有依赖的两项排序：上限先行、下限后置，
        // 其余保持文件里的原序（stable_sort 保证）。判据只在这一处，不碰校验本身。
        std::vector<std::pair<std::wstring, json::Value>> ordered;
        ordered.reserve(bag->obj.size());
        for (const auto& kv : bag->obj) ordered.emplace_back(kv.first, kv.second);
        auto importOrder = [](const std::wstring& key) {
            if (key == L"tabMaxWidth") return 0;   // 上限先放宽
            if (key == L"tabMinWidth") return 1;   // 下限后收紧
            return 2;
        };
        std::stable_sort(ordered.begin(), ordered.end(),
                         [&](const auto& a, const auto& b) {
                             return importOrder(a.first) < importOrder(b.first);
                         });
        for (const auto& kv : ordered) {
            ++total;
            // key 不存在 / 只读：在这里就拒掉，别让 config.set 去报一个更绕的错。
            const settings_reg::Entry* e = settings_reg::Find(kv.first);
            if (!e) {
                json::Value bad = json::Value::MakeObject();
                bad.Set(L"key", json::Value::MakeStr(kv.first));
                bad.Set(L"reason", json::Value::MakeStr(L"没有这个设置项"));
                failed.arr.push_back(std::move(bad));
                continue;
            }
            if (!e->set) {
                json::Value bad = json::Value::MakeObject();
                bad.Set(L"key", json::Value::MakeStr(kv.first));
                bad.Set(L"reason", json::Value::MakeStr(L"只读项，不能写"));
                failed.arr.push_back(std::move(bad));
                continue;
            }
            // 走总线：校验、界面落地、写盘、审计全在 config.set 里，
            // 这里不重复实现一遍（重复实现 = 两处判据迟早不一致）。
            json::Value a = json::Value::MakeObject();
            a.Set(L"key", json::Value::MakeStr(kv.first));
            a.Set(L"value", kv.second);
            if (!CommandBus::Get().Invoke(CmdId::ConfigSet, Args::FromJson(a), Actor::Ui())) {
                json::Value bad = json::Value::MakeObject();
                bad.Set(L"key", json::Value::MakeStr(kv.first));
                bad.Set(L"reason", json::Value::MakeStr(L"该值不被接受（类型/范围不符）"));
                failed.arr.push_back(std::move(bad));
                continue;
            }
            applied.arr.push_back(json::Value::MakeStr(kv.first));
        }
        json::Value root = json::Value::MakeObject();
        // 成功数要在 move **之前**取：applied 随后被 move 进 root，
        // 移动之后 applied.arr 是空的 —— 先判后移的顺序反了，
        // 就会把"60 项全部还原成功"报成失败（实测踩过）。
        const size_t appliedCount = applied.arr.size();
        root.Set(L"applied", std::move(applied));
        root.Set(L"failed", std::move(failed));
        root.Set(L"count", json::Value::MakeInt(total));
        *out = std::move(root);
        // 全部失败 = 这一趟什么都没还原成，如实报失败（逐项原因已在 failed 里）。
        return total > 0 && appliedCount > 0;
    });
}
