// 命令目录（数据）· 用户操作域：Tab / Go / Nav / Path / File / View / Window / 设置…
//
// 【这一层的分工】
//   · commands_table.cpp          —— 用户操作域的行表 + 查表入口（CommandTable / FindCommand）
//   · commands_table_tools.cpp    —— 工具·Agent 域的行表
//   · commands_table.h            —— 两段共用的常量与行表声明
//   · commands.cpp                —— 派发总线（能不能执行、怎么路由、返回什么）
//
// 【为什么查表入口在这里】CommandTable() 负责把两段行表拼成一张，
// FindCommand() 要遍历的是**拼完的整张表**（不是只看某一段）——
// 所以这两个入口必须能看见拼接结果，就放在这里。
#include "commands_table.h"

#include "cmd_naming.h"
#include "str.h"

namespace {

}  // namespace

std::vector<CommandDef> UserCommandRows() {
    return {
    // ---- Tab ----
    {CmdId::TabNew, L"tab.new", L"Tab", L"标签栏", L"新建标签页", true, Effects{0, 0, 0, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要打开的目录；省略则打开此电脑（与 + 按钮一致）\"}},\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "tabCount=$changed"},
    // 【为什么要求 HasMultipleTabs】关掉最后一个标签 = 关闭程序（CloseTab 里
    // Count()==0 会 DestroyWindow）。而 win.quit 特意设了 aiExposed=false，
    // 理由是"退出程序不该由机器替人决定"。如果 tab.close 只要 HasTabs，
    // AI 就能用 tab.close 达成同一件事，把那条设计整个绕过去。
    // 单标签时关标签没有"关标签"的意义（等于退出），所以拒绝是语义正确的。
    {CmdId::TabClose, L"tab.close", L"Tab", L"标签栏", L"关闭标签页", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs, Req::HasMultipleTabs}, nullptr, "tabCount=$changed"},
    {CmdId::TabCloseOther, L"tab.close.others", L"Tab", L"标签栏", L"关闭其他标签", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "tabCount=$changed"},
    {CmdId::TabCloseLeft, L"tab.close.left", L"Tab", L"标签栏", L"关闭左侧标签", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "tabCount=$changed"},
    {CmdId::TabCloseRight, L"tab.close.right", L"Tab", L"标签栏", L"关闭右侧标签", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "tabCount=$changed"},
    {CmdId::TabClone, L"tab.clone", L"Tab", L"标签栏", L"复制当前标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "tabCount=$changed"},
    {CmdId::TabReopenClosed, L"tab.reopen", L"Tab", L"标签栏", L"重新打开关闭的标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasClosedTabs}, nullptr, "tabCount=$changed"},
    // tab.move.* 的 undoable 从 1 改成 0（2026-11-21，P0-3）：
    // core::TabSet 里**没有任何 Undo 符号**（grep 无）—— 标签重排没有撤销栈。
    // 以前声明 undoable=1 是"我们能撤"的承诺，但实际撤不了：
    // AI 据此以为"随便挪，错了能退"，退不了就是骗 AI。
    // 真要可撤销，得给 TabSet 加一份"移动前顺序"快照（与 bmk.* 同口径）。
    {CmdId::TabMoveLeft, L"tab.move.left", L"Tab", L"标签栏", L"标签左移", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::CanMoveTabLeft}, nullptr, "activeTab=$changed"},
    {CmdId::TabMoveRight, L"tab.move.right", L"Tab", L"标签栏", L"标签右移", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::CanMoveTabRight}, nullptr, "activeTab=$changed"},
    {CmdId::TabMoveFirst, L"tab.move.first", L"Tab", L"标签栏", L"标签移到最前", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::CanMoveTabLeft}, nullptr, "activeTab=$changed"},
    {CmdId::TabMoveLast, L"tab.move.last", L"Tab", L"标签栏", L"标签移到最后", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::CanMoveTabRight}, nullptr, "activeTab=$changed"},
    {CmdId::TabPrevTab, L"tab.activate.mru", L"Tab", L"标签栏", L"回到上一个激活的标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=$changed"},
    {CmdId::TabLast, L"tab.activate.last", L"Tab", L"标签栏", L"最后一个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTabIsLast=true"},
    {CmdId::CloseLeftOne, L"tab.close.prev", L"Tab", L"标签栏", L"关闭左侧一个", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs, Req::CanMoveTabLeft}, nullptr, "tabCount=$changed"},
    {CmdId::CloseRightOne, L"tab.close.next", L"Tab", L"标签栏", L"关闭右侧一个", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs, Req::CanMoveTabRight}, nullptr, "tabCount=$changed"},
    {CmdId::TabActivate, L"tab.activate", L"Tab", L"标签栏", L"切换到第 N 个标签", true, Effects{0, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"0 起的标签序号\"}},\"required\":[\"index\"],\"additionalProperties\":false}", true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=$arg:index"},
    {CmdId::GoTab1, L"tab.goto.1", L"Tab", L"标签栏", L"切换到第 1 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=0"},
    {CmdId::GoTab2, L"tab.goto.2", L"Tab", L"标签栏", L"切换到第 2 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=1"},
    {CmdId::GoTab3, L"tab.goto.3", L"Tab", L"标签栏", L"切换到第 3 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=2"},
    {CmdId::GoTab4, L"tab.goto.4", L"Tab", L"标签栏", L"切换到第 4 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=3"},
    {CmdId::GoTab5, L"tab.goto.5", L"Tab", L"标签栏", L"切换到第 5 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=4"},
    {CmdId::GoTab6, L"tab.goto.6", L"Tab", L"标签栏", L"切换到第 6 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=5"},
    {CmdId::GoTab7, L"tab.goto.7", L"Tab", L"标签栏", L"切换到第 7 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=6"},
    {CmdId::GoTab8, L"tab.goto.8", L"Tab", L"标签栏", L"切换到第 8 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=7"},
    {CmdId::GoTab9, L"tab.goto.9", L"Tab", L"标签栏", L"切换到第 9 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=8"},
    // 【为什么加 HasMultipleTabs】只有一个标签时 tab.next / tab.prev 无事可做，
    // 但 handler 以前照样返回 true（"幂等"）。于是命令说成功了、状态却没变，
    // 自断言 activeTab=$changed 只能把它判成"效果不符" ——
    // 那是**断言写错了**（期望并不成立），不是功能坏了。
    // 正确的修法是让前置条件说清楚：只有一个标签时这两个命令根本不该执行。
    {CmdId::TabNext, L"tab.next", L"Tab", L"标签栏", L"下一个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs, Req::HasMultipleTabs}, nullptr, "activeTab=$changed"},
    {CmdId::TabPrev, L"tab.prev", L"Tab", L"标签栏", L"上一个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs, Req::HasMultipleTabs}, nullptr, "activeTab=$changed"},
    // 单窗口模型下没有"合并 explorer 标签"这回事，保留在目录里只为让 keymap 不静默丢键。
    {CmdId::TabMergeAll, L"tab.merge", L"Tab", L"标签栏", L"合并所有窗口标签（不适用）", false, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, false, false},
    // ---- Go ----
    {CmdId::GoHome, L"go.home", L"Go", L"导航栏", L"回到主文件夹", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoDesktop, L"go.desktop", L"Go", L"导航栏", L"桌面", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoDownloads, L"go.downloads", L"Go", L"导航栏", L"下载", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoDocuments, L"go.documents", L"Go", L"导航栏", L"文档", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoConfigPath, L"go.config", L"Go", L"导航栏", L"配置目录", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoLocalAppData, L"go.localappdata", L"Go", L"导航栏", L"LocalAppData", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoControlPanel, L"go.controlpanel", L"Go", L"导航栏", L"控制面板", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoStartFolder, L"go.startfolder", L"Go", L"导航栏", L"开始菜单文件夹", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoThisPC, L"go.thispc", L"Go", L"导航栏", L"此电脑", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::GoPowerShell, L"go.powershell", L"Go", L"导航栏", L"PowerShell 目录", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    // ---- Nav ----
    {CmdId::NavBack, L"nav.back", L"Nav", L"导航栏", L"后退", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasHistoryBack}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::NavForward, L"nav.forward", L"Nav", L"导航栏", L"前进", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasHistoryForward}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::NavUp, L"nav.up", L"Nav", L"导航栏", L"向上一级", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::CanGoUp}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::NavRefresh, L"nav.refresh", L"Nav", L"导航栏", L"刷新", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activePath=$unchanged"},
    {CmdId::NavHome, L"nav.home", L"Nav", L"导航栏", L"回到主文件夹", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::NavViewMenu, L"nav.viewmenu", L"Nav", L"导航栏", L"视图菜单", true, Effects{0, 0, 0, 1, 0, Scope::App}, kNoArgs, false, false},
    // ---- Path ----
    {CmdId::PathCopy, L"path.copy", L"Path", L"导航栏", L"复制完整路径", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::PathCopyName, L"path.copy.name", L"Path", L"导航栏", L"复制文件名", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::PathCopyDir, L"path.copy.dir", L"Path", L"导航栏", L"复制当前目录", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"实际写入剪贴板的内容\"}},\"required\":[\"text\"]}", nullptr, kExemptNoReadback},
    {CmdId::PathCopyUnc, L"path.copy.unc", L"Path", L"导航栏", L"复制 UNC 路径", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, true},
    {CmdId::PathCopyExt, L"path.copy.ext", L"Path", L"导航栏", L"复制扩展名", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, true},
    {CmdId::PathCopyTab, L"path.copy.tab", L"Path", L"导航栏", L"复制所有标签路径", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"实际写入剪贴板的内容（每行一条）\"}},\"required\":[\"text\"]}", nullptr, kExemptNoReadback},
    {CmdId::PathCopyLink, L"path.copy.link", L"Path", L"导航栏", L"复制为快捷方式", false, Effects{0, 1, 0, 1, 0, Scope::CurrentTab}, kNoArgs, false, false},
    // ---- File ----
    {CmdId::CmdNewTextFile, L"file.new.text", L"File", L"文件视图", L"新建文本文件", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::CmdOpenWith, L"file.open.with", L"File", L"文件视图", L"打开方式", true, Effects{0, 0, 0, 1, 0, Scope::System}, kNoArgs, false, true, Requirements{Req::HasSelectionSingle}},
    {CmdId::CmdNewFolder, L"file.new.folder", L"File", L"文件操作", L"新建文件夹", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::CmdCopy, L"file.copy", L"File", L"文件操作", L"复制", true, kEffectCopyToClipboard, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::CmdCut, L"file.cut", L"File", L"文件操作", L"剪切", true, kEffectCutToClipboard, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::CmdPaste, L"file.paste", L"File", L"文件操作", L"粘贴", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasClipboard}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::CmdDelete, L"file.delete", L"File", L"文件操作", L"删除", true, kEffectDelete, kNoArgs, false, true, Requirements{Req::HasSelection}},
    // Shift+Delete 与 Delete 的唯一差别在底层 IFileOperation 的 FOF_ALLOWUNDO：
    // 这里不带它，所以系统直接永久删除。Effects 仍复用 kEffectDelete
    // （destructive=1 / undoable=0）—— 契约按物理后果声明，不按键位名字声明。
    // aiExposed=false：机器侧要删就用 fs.delete 并显式 approve，不能靠"永久删除"
    // 绕过审批语义；这条只给人用键盘。
    {CmdId::CmdDeletePermanent, L"file.delete.permanent", L"File", L"文件操作", L"永久删除", true, kEffectDelete, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::CmdRename, L"file.rename", L"File", L"文件操作", L"重命名", true, kEffectRename, kNoArgs, false, true, Requirements{Req::HasSelection}},
    // 【Effects 必须按物理后果声明，不按"这个命令看起来像什么"】
    // 打开属性对话框**能改东西**：只读位、隐藏属性、时间戳。所以 readOnly 必须是 0，
    // 与同义的 fs.properties 保持同一口径（那一行本来就是 0）。
    // 以前这里标 readOnly=1，而 AI 客户端正是按 annotations 决定"要不要先弹确认" ——
    // 两套口径等于契约不可信。
    {CmdId::CmdProps, L"file.properties", L"File", L"文件操作", L"属性", true, kEffectProperties, kNoArgs, false, true, Requirements{Req::HasSelectionSingle}},
    // file.undo 的 undoable = 0（2026-11-21，P0-3）：它**就是**撤销入口，
    // 自己不进撤销栈（"可撤销"说的是"这次操作能被撤"，撤销本身不能被撤）。
    // 它的实现在 App::RegisterFileCommands 里：先撤我们自己的文件操作栈
    // （src/file_undo.h），栈空才落 shell 的撤销谓词。
    {CmdId::CmdUndo, L"file.undo", L"File", L"文件操作", L"撤销", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::CmdSelectAll, L"file.select.all", L"File", L"文件操作", L"全选", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptSetSemantics},
    {CmdId::CmdOpen, L"file.open", L"File", L"文件操作", L"打开", true, kEffectOpen, kNoArgs, false, true},
    {CmdId::CmdOpenInNewTab, L"file.open.tab", L"File", L"文件操作", L"在新标签打开", true, kEffectOpenTab, kNoArgs, false, true},
    {CmdId::CmdOpenInNewWindow, L"file.open.window", L"File", L"文件操作", L"在新窗口打开", true, kEffectOpenWindow, kNoArgs, false, true},
    {CmdId::SelectInvert, L"file.select.invert", L"File", L"文件视图", L"反向选择", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptSetSemantics},
    {CmdId::SelectFiles, L"file.select.files", L"File", L"文件视图", L"仅选文件", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "selectionCount!=0"},
    {CmdId::SelectFolders, L"file.select.folders", L"File", L"文件视图", L"仅选文件夹", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "selectionCount!=0"},
    {CmdId::SelectByType, L"file.select.type", L"File", L"文件视图", L"按类型选择", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::FileFocusTop, L"file.focus.top", L"File", L"文件视图", L"定位到第一项", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptFocus},
    {CmdId::FileFocusBottom, L"file.focus.bottom", L"File", L"文件视图", L"定位到最后一项", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptFocus},
    {CmdId::CmdRefresh, L"file.refresh", L"File", L"文件视图", L"刷新", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasTabs}, nullptr, "activePath=$unchanged"},
    // 复制/移动到指定文件夹（IFileOperation + 目录选择器），与 mod 的同名命令对齐。
    {CmdId::CopyToFolder, L"file.copy.to", L"File", L"文件操作", L"复制到文件夹", true, kEffectCopyTo, kNoArgs, false, true},
    {CmdId::MoveToFolder, L"file.move.to", L"File", L"文件操作", L"移动到文件夹", true, kEffectMoveTo, kNoArgs, false, true},
    // 批量重命名交给 PowerRename，按模式选择需要"选中集"抽象，本轮不造。
    // 批量重命名：destructive=true（可能不可逆），但 undoable 走的是 shell 的撤销栈
    // （每个改名都是 FOF_ALLOWUNDO），所以 Ctrl+Z 能退回 —— 见 filetools::PlanBatchRename。
    {CmdId::BatchRename, L"file.rename.batch", L"File", L"文件操作", L"批量重命名", true, Effects{0, 1, 0, 0, 1, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"命名模式：{name} 原名（不含扩展名）、{ext} 扩展名、{old} 原名全称、{n}/{nn}/{nnn} 从 1 起的序号（补零）\"},\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"要改名的完整路径；省略则用当前选中项（必须在当前目录内）\"}},\"required\":[\"pattern\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"renamed\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"from\",\"to\"]}},\"failed\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"}},\"required\":[\"from\",\"reason\"]},\"description\":\"改失败的项与具体原因（被占用/权限不足/重名/不在视图）\"},\"skippedSame\":{\"type\":\"integer\",\"description\":\"算出的名字与原名相同、被跳过的项数\"}},\"required\":[\"pattern\",\"renamed\",\"failed\"]}", nullptr, kExemptNoReadback},
    {CmdId::SelectByPattern, L"file.select.pattern", L"File", L"文件视图", L"按模式选择", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"匹配模式，分号分隔多个，如 *.txt;*.md；省略则弹输入框\"},\"folders\":{\"type\":\"boolean\",\"description\":\"是否也匹配文件夹（默认 false）\"},\"caseSensitive\":{\"type\":\"boolean\",\"description\":\"区分大小写（默认 false）\"}},\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "selectionCount!=0"},
    // ---- 确定式文件操作（P5：目标由参数点名，绝不弹框、绝不读选中）----
    // 与上面的 CmdRename/CmdDelete/CopyToFolder/MoveToFolder 是同一个业务的两种入口。
    // 之所以要单独一组而不是给旧命令加参数：旧命令的语义里"弹框问用户"是不可分割的
    // 一部分（F2 重命名的体验就是"弹出输入框"），加参数会变成"传了参数就不弹框"
    // 的隐式分叉。拆成两条命令后，人和机器各走各的入口，共用同一份实现。
    {CmdId::FsRename, L"fs.rename", L"File", L"文件操作", L"重命名（指定目标）", true, kEffectRename, "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"description\":\"要重命名的项目完整路径\"},\"newName\":{\"type\":\"string\",\"description\":\"新名字（只给名字，不含路径）\"}},\"required\":[\"target\",\"newName\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::FsDelete, L"fs.delete", L"File", L"文件操作", L"删除（指定目标）", true, kEffectDelete, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"要删除的项目完整路径列表\"}},\"required\":[\"targets\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::FsCopyTo, L"fs.copy", L"File", L"文件操作", L"复制到（指定目标）", true, kEffectCopyTo, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"to\":{\"type\":\"string\",\"description\":\"目标文件夹完整路径\"}},\"required\":[\"targets\",\"to\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::FsMoveTo, L"fs.move", L"File", L"文件操作", L"移动到（指定目标）", true, kEffectMoveTo, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"to\":{\"type\":\"string\",\"description\":\"目标文件夹完整路径\"}},\"required\":[\"targets\",\"to\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::FsOpen, L"fs.open", L"File", L"文件操作", L"打开（指定路径）", true, kEffectOpen, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要打开的完整路径（文件夹会在当前标签内进入）\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::FsOpenInNewTab, L"fs.open.tab", L"File", L"文件操作", L"打开（新标签）", true, kEffectOpenTab, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"目标的完整路径\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptAsyncNav},
    {CmdId::FsOpenInNewWindow, L"fs.open.window", L"File", L"文件操作", L"打开（新窗口）", true, kEffectOpenWindow, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"目标的完整路径\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::FsCopy, L"fs.copy.toclip", L"File", L"文件操作", L"复制到剪贴板（指定路径）", true, kEffectCopyToClipboard, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"一组完整路径\"}},\"required\":[\"targets\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::FsCut, L"fs.cut.toclip", L"File", L"文件操作", L"剪切到剪贴板（指定路径）", true, kEffectCutToClipboard, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"一组完整路径\"}},\"required\":[\"targets\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::FsProperties, L"fs.properties", L"File", L"文件操作", L"属性（指定路径）", true, kEffectProperties, "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"description\":\"目标的完整路径\"}},\"required\":[\"target\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    // ---- View ----
    {CmdId::ViewMode, L"view.mode", L"View", L"文件视图", L"视图模式", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"preset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7,\"description\":\"0=超大图标 1=大图标 2=中图标 3=小图标 4=列表 5=详细信息 6=平铺 7=内容\"}},\"required\":[\"preset\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "viewMode=$arg:preset"},
    {CmdId::ViewSort, L"view.sort", L"View", L"文件视图", L"排序方式", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"by\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3,\"description\":\"0=名称 1=日期 2=类型 3=大小\"}},\"required\":[\"by\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "sortMode=$arg:by"},
    {CmdId::ViewGroup, L"view.group", L"View", L"文件视图", L"分组方式", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"by\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3,\"description\":\"0=不分组 1=类型 2=日期 3=大小\"}},\"required\":[\"by\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "groupBy=$arg:by"},
    // P3-4：放开到任意 PROPERTYKEY。与 0..3 档位并存，不互相推导 ——
    // 任意列（作者/标签/创建日期…）不在四档里，档位无法表达它。
    // key 用规范格式 "{GUID},pid"（PSPropertyKeyFromString 接受的形式），
    // ascending 省略 = 同列再点翻转（与资源管理器一致）。
    {CmdId::ViewSortByKey, L"view.sort.bykey", L"View", L"文件视图", L"按属性排序", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"pattern\":\"^\\\\{[0-9A-Fa-f-]+\\\\}:\\\\d+$\",\"description\":\"PROPERTYKEY，形如 {B725F130-47EF-101A-A5F1-02608C9EEBAC}:10（名称）/ {E3E0584C-B788-4A5A-BB20-7F45E13135E9}:1（作者）等；可用 view.props.list 查全部\"},\"ascending\":{\"type\":\"boolean\",\"description\":\"省略=同列再次排序则反向\"}},\"required\":[\"key\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"ascending\":{\"type\":\"boolean\"}},\"required\":[\"key\",\"ascending\"]}", "sortKey=$arg:key"},
    {CmdId::ViewGroupByKey, L"view.group.bykey", L"View", L"文件视图", L"按属性分组", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"pattern\":\"^\\\\{[0-9A-Fa-f-]+\\\\}:\\\\d+$\",\"description\":\"PROPERTYKEY，形如 {B725F130-47EF-101A-A5F1-02608C9EEBAC}:10\"}},\"required\":[\"key\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}", "groupKey=$arg:key"},
    {CmdId::ViewPropsList, L"view.props.list", L"View", L"文件视图", L"列出可排序属性", true, Effects{1, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"properties\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"}},\"required\":[\"id\",\"title\"]}}},\"required\":[\"properties\"]}", nullptr, kExemptNoReadback},
    {CmdId::ToggleHidden, L"view.toggle.hidden", L"View", L"文件视图", L"显示隐藏文件", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "showHiddenFiles=$changed"},
    {CmdId::ToggleSystemFiles, L"view.toggle.system", L"View", L"文件视图", L"显示系统文件", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "showSystemFiles=$changed"},
    {CmdId::ToggleExtensions, L"view.toggle.extensions", L"View", L"文件视图", L"显示扩展名", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "showExtensions=$changed"},
    {CmdId::CycleHiddenSeg, L"view.cycle.hidden", L"View", L"文件视图", L"切换隐藏文件三态", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "shellHiddenState=$changed"},
    {CmdId::TogglePreviewPane, L"view.toggle.preview", L"View", L"文件视图", L"预览窗格", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "previewPaneVisible=$changed"},
    {CmdId::ToggleNavPane, L"view.toggle.nav", L"View", L"文件视图", L"导航窗格", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "navPaneVisible=$changed"},
    // Explorer 的 Ctrl+Shift+E：把导航窗格整棵树展开。纯 UI 便利操作，不暴露给 AI；
    // 状态里没有"每节点展开与否"的回读面，所以不编造断言。
    {CmdId::NavTreeExpandAll, L"nav.tree.expand.all", L"View", L"文件视图", L"展开导航树", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    // ---- 详细信息列（P3）----
    // list 是只读且无参：AI 可先拿稳定列 id，再决定是否调用 set。
    // set/choose 不暴露给 AI：前者要人从菜单里勾选，后者本身就是 UI 动作。
    {CmdId::ViewColumnsList, L"view.columns.list", L"View", L"文件视图", L"列出详情列", true, Effects{1, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"columns\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"visible\":{\"type\":\"boolean\"},\"canHide\":{\"type\":\"boolean\"},\"width\":{\"type\":\"integer\"}},\"required\":[\"id\",\"name\",\"visible\",\"canHide\",\"width\"]}}},\"required\":[\"columns\"]}", nullptr, kExemptNoReadback},
    {CmdId::ViewColumnsSet, L"view.columns.set", L"View", L"文件视图", L"设置详情列", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"visible\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"要显示的列 id；来自 view.columns.list\"}},\"required\":[\"visible\"],\"additionalProperties\":false}", false, false},
    {CmdId::ViewColumnsChoose, L"view.columns.choose", L"View", L"文件视图", L"选择详细信息列", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, false, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    // Ctrl+滚轮：步进 ±1，handler 内部换算成 16px 并夹在 16..256。
    // choose 是人用的离散“滑块”（预设档位菜单），set 由菜单转投，不暴露给 AI。
    {CmdId::ViewZoom, L"view.zoom", L"View", L"文件视图", L"缩放图标", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"delta\":{\"type\":\"integer\",\"minimum\":-8,\"maximum\":8,\"description\":\"滚轮步数：正=放大，负=缩小\"}},\"required\":[\"delta\"],\"additionalProperties\":false}", false, false},
    {CmdId::ViewZoomSet, L"view.zoom.set", L"View", L"文件视图", L"设置图标大小", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"iconPx\":{\"type\":\"integer\",\"minimum\":16,\"maximum\":256,\"description\":\"图标像素\"}},\"required\":[\"iconPx\"],\"additionalProperties\":false}", false, false},
    {CmdId::ViewZoomChoose, L"view.zoom.choose", L"View", L"文件视图", L"图标大小", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, false, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::ToggleCheckboxes, L"view.toggle.checkboxes", L"View", L"文件视图", L"复选框", false, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    {CmdId::ViewNext, L"view.next", L"View", L"文件视图", L"下一个视图模式", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "viewMode=$changed"},
    {CmdId::ViewPrev, L"view.prev", L"View", L"文件视图", L"上一个视图模式", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{}, nullptr, "viewMode=$changed"},
    {CmdId::ViewToggleDualPane, L"view.dualpane.toggle", L"View", L"文件视图", L"切换双栏", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, false, false},
    {CmdId::ViewNextPane, L"view.pane.next", L"View", L"文件视图", L"切换到另一个窗格", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    {CmdId::ViewPaneSplitSet, L"view.pane.split", L"View", L"文件视图", L"设置分栏比例", true, Effects{0, 0, 1, 0, 0, Scope::App}, kPaneSplitSchema, false, false},
    // 垂直双栏：把双栏从"左右并排"切成"上下堆叠"（或切回来）。
    // 与 view.dualpane.toggle 分开：开合与方向是两件事，方向在单栏下也有意义
    // （先切方向、再开双栏，得到的就是上下两栏）。
    {CmdId::ViewTogglePaneVertical, L"view.pane.vertical", L"View", L"文件视图", L"切换双栏方向（左右/上下）", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, false, false},
    // ---- AI 面板 ----
    // 右侧的聊天面板。断言 aiPanelVisible=$changed：开关必须真的改了状态，
    // 否则"按了没反应"会被当成成功（面板创建失败时正是这个症状）。
    {CmdId::AiPanelToggle, L"ai.panel.toggle", L"Ai", L"AI 助手", L"开合 AI 助手面板", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "aiPanelVisible=$changed"},
    // 用系统浏览器打开面板服务（WebView2 不可用时的退路）。
    // 不声明断言：打开浏览器没有可回读的状态（我们无法判断用户是否真看到了页面），
    // 而"编一个断言"比没有断言更糟 —— 那会让 verdict 说谎。kExemptNoReadback 就是这个意思。
    {CmdId::AiPanelWeb, L"ai.panel.web", L"Ai", L"AI 助手", L"在浏览器打开 AI 面板", true, Effects{0, 0, 0, 1, 0, Scope::System}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    // ---- Window ----
    {CmdId::NewWindow, L"win.new", L"Window", L"窗口", L"新窗口", true, Effects{0, 0, 0, 0, 0, Scope::System}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::QuitExplorer, L"win.quit", L"Window", L"全局", L"退出程序", true, Effects{0, 1, 0, 0, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::FolderOptions, L"win.folderoptions", L"Window", L"系统对话框", L"文件夹选项", true, Effects{0, 0, 1, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::MapDrive, L"win.mapdrive", L"Window", L"系统对话框", L"映射网络驱动器", true, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::UnmapDrive, L"win.unmapdrive", L"Window", L"系统对话框", L"断开网络驱动器", true, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::CommandPalette, L"win.palette", L"Window", L"窗口", L"命令面板", true, Effects{1, 0, 0, 0, 0, Scope::App}, kNoArgs, false, false},
    {CmdId::HotkeyManager, L"win.hotkeys", L"Window", L"窗口", L"快捷键管理器", true, Effects{0, 1, 0, 0, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::FocusAddress, L"win.focus.address", L"Window", L"导航栏", L"聚焦地址栏", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "focus=address_bar"},
    {CmdId::FocusSearch, L"win.focus.search", L"Window", L"导航栏", L"聚焦搜索框", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "focus=search_box"},
    // 原声明是 implemented=false + destructive=1 + Scope::System，三条都不对：
    // 面板早就有（导航栏右键「设置…」），开的是一扇窗、不改任何不可逆状态，
    // 也不碰进程外。声明错了的直接后果是命令面板里搜不到它、键位绑不上。
    {CmdId::Preferences, L"win.preferences", L"Window", L"窗口", L"首选项", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, false, false},
    // ---- External ----
    {CmdId::OpenTerminal, L"ext.terminal", L"External", L"导航栏", L"在此打开终端", true, Effects{0, 0, 0, 1, 0, Scope::System}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::OpenTerminalAdmin, L"ext.terminal.admin", L"External", L"导航栏", L"管理员终端", true, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptNoReadback},
    {CmdId::RunCommand, L"ext.run", L"External", L"全局", L"运行命令", false, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::SendKeys, L"ext.sendkeys", L"External", L"全局", L"发送按键", false, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    // ---- Bookmark ----
    {CmdId::BmkAddCurrent, L"bmk.add.current", L"Bookmark", L"书签栏", L"收藏当前目录", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"新条目 id\"}},\"required\":[\"id\"],\"additionalProperties\":false}", "bookmarkTotalCount=$changed"},
    {CmdId::BmkShowBar, L"bmk.show", L"Bookmark", L"书签栏", L"显示书签栏", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "bookmarkBarVisible=true"},
    {CmdId::BmkHideBar, L"bmk.hide", L"Bookmark", L"书签栏", L"隐藏书签栏", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "bookmarkBarVisible=false"},
    {CmdId::BmkToggleBar, L"bmk.toggle", L"Bookmark", L"书签栏", L"切换书签栏", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "bookmarkBarVisible=$changed"},
    // 书签编辑族：原来只有右键菜单可达，补成命令后键盘/面板/AI 与人等价。
    // 删除/清空/排序都进撤销栈，所以 destructive=0（可回退）；undoable=1。
    {CmdId::BmkEdit, L"bmk.edit", L"Bookmark", L"书签栏", L"编辑书签", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"书签 id\"},\"name\":{\"type\":\"string\",\"description\":\"新名称\"},\"path\":{\"type\":\"string\",\"description\":\"新路径；空串表示改成纯分类\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "bookmarkTotalCount=$unchanged"},
    {CmdId::BmkRemove, L"bmk.remove", L"Bookmark", L"书签栏", L"删除书签", true, Effects{0, 1, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"书签 id\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "bookmarkTotalCount=$changed"},
    {CmdId::BmkNewFolder, L"bmk.newfolder", L"Bookmark", L"书签栏", L"新建分类", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"分类名称\"},\"parentId\":{\"type\":\"string\",\"description\":\"父分类 id；省略=顶层\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"新分类 id\"}},\"required\":[\"id\"],\"additionalProperties\":false}", "bookmarkTotalCount=$changed"},
    {CmdId::BmkUndo, L"bmk.undo", L"Bookmark", L"书签栏", L"撤销书签操作", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptUndo},
    {CmdId::BmkClearAll, L"bmk.clear", L"Bookmark", L"书签栏", L"清除全部书签", true, Effects{0, 1, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "bookmarkTotalCount=0"},
    {CmdId::BmkSort, L"bmk.sort", L"Bookmark", L"书签栏", L"按名称排序", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"recursive\":{\"type\":\"boolean\",\"description\":\"是否连分类内部一起排，默认 true\"},\"ascending\":{\"type\":\"boolean\",\"description\":\"升序（默认 true），false 为降序\"},\"parentId\":{\"type\":\"string\",\"description\":\"只排这个分类内的直接子项；省略=按 recursive 排整棵树\"}},\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "bookmarkTotalCount=$unchanged"},
    {CmdId::BmkDedupe, L"bmk.dedupe", L"Bookmark", L"书签栏", L"清理重复书签", true, Effects{0, 1, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, nullptr, kExemptLegalNoChange},
    // ---- 补齐三处"只有鼠标能做"的动作（拖放添加 / 拖拽重排 / 读取树）----
    // 前两条与拖放走**同一份实现**：菜单、拖放、脚本、AI 都调它们。
    {CmdId::BmkAdd, L"bmk.add.path", L"Bookmark", L"书签栏", L"添加书签（指定路径）", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要收藏的目录完整路径\"},\"name\":{\"type\":\"string\",\"description\":\"显示名；省略则取末段\"},\"parentId\":{\"type\":\"string\",\"description\":\"父分类 id；省略=顶层\"},\"toIndex\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"在目标容器内的插入位置；省略=末尾\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"新条目 id；路径已存在且给了 parentId 或 toIndex 时为移动后的已存在条目\"},\"moved\":{\"type\":\"boolean\",\"description\":\"true=路径已存在，本次是移动而非新增\"}},\"required\":[\"id\"],\"additionalProperties\":false}", "bookmarkTotalCount=$changed"},
    // 收藏**选中的**文件夹（与 bmk.add.current 的"当前目录"是两个意图：选中≠当前）。
    // needsSelection + HasSelection 前置：无选中时前置拒绝，不猜用户意图。
    {CmdId::BmkAddSelection, L"bmk.add.selection", L"Bookmark", L"书签栏", L"收藏选中的文件夹", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, true, Requirements{Req::HasSelection}, nullptr, "bookmarkTotalCount=$changed"},
    // 收藏**所有打开标签页**的目录（与 add.current 只收当前目录互补）。
    // 返回 added/skipped/ids —— 批量动作必须可验证：调用方从响应直接拿到
    // 新条目 id，不再需要按名字回查（那是重名场景下会失能的临时做法）。
    {CmdId::BmkAddTabs, L"bmk.add.tabs", L"Bookmark", L"书签栏", L"收藏所有打开标签页", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"added\":{\"type\":\"integer\",\"description\":\"新增条数\"},\"skipped\":{\"type\":\"integer\",\"description\":\"跳过条数（已收藏/空路径）\"},\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"新增条目的 id，顺序同标签顺序\"}},\"required\":[\"added\",\"skipped\",\"ids\"]}", nullptr, kExemptLegalNoChange},
    // 复制书签（分类连子树）。返回新 id —— 复制的下一步（编辑副本）必须有 id 才能做。
    {CmdId::BmkCopy, L"bmk.copy", L"Bookmark", L"书签栏", L"复制书签", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"要复制的书签 id\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"新条目 id，紧挨原条目之后\"}},\"required\":[\"id\"]}", "bookmarkTotalCount=$changed"},
    {CmdId::BmkMove, L"bmk.move", L"Bookmark", L"书签栏", L"移动书签", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"要移动的书签 id\"},\"toId\":{\"type\":\"string\",\"description\":\"目标分类 id（移入它内部）\"},\"toIndex\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"在目标内的位置；省略=末尾\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "bookmarkTotalCount=$unchanged"},
    // 解散分类：分类里的**直接子项**提到它所在的上一层（顶层分类 → 书签栏），
    // 分类本身消失。节点总数一定减一（少了分类自己），所以断言是 $changed。
    // 可撤销：store 在改树前拍了快照（与删除同一档）。
    {CmdId::BmkDissolve, L"bmk.dissolve", L"Bookmark", L"书签栏", L"解散分类", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"要解散的分类 id\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"被提升到上一层的条目 id，顺序同新位置\"},\"count\":{\"type\":\"integer\",\"description\":\"被提升的条目数\"}},\"required\":[\"ids\",\"count\"]}", "bookmarkTotalCount=$changed"},
    // bmk.list 是**读**：其余 bmk.* 要的 id 由它给出。没有它，
    // AI 拿到"删除书签"却无从知道有哪些 id —— 命令表就是断的。
    {CmdId::BmkList, L"bmk.list", L"Bookmark", L"书签栏", L"列出书签", true, Effects{1, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"parentId\":{\"type\":\"string\",\"description\":\"只列该分类下；省略=整棵树\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"entries\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"isFolder\":{\"type\":\"boolean\"},\"parentId\":{\"type\":\"string\"}},\"required\":[\"id\",\"name\",\"isFolder\"]}}}}", "bookmarkTotalCount=$unchanged"},
    // ---- 设置域 ----
    // config.list 是「能枚举」的落地：AI 先问有哪些项，再按 key 读/写，
    // 全程不依赖界面文案或坐标。
    {CmdId::ConfigList, L"config.list", L"Config", L"设置", L"列出全部设置项", true, Effects{1, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"items\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"settings.json 里的键名，读写都用它\"},\"kind\":{\"type\":\"string\",\"description\":\"bool / int / string / string[] / int[] / object(readonly)\"},\"doc\":{\"type\":\"string\",\"description\":\"中文名与说明\"},\"writable\":{\"type\":\"boolean\",\"description\":\"false = 只读，doc 会说明该用哪个入口\"},\"value\":{},\"min\":{\"type\":\"integer\"},\"max\":{\"type\":\"integer\"}},\"required\":[\"key\",\"kind\",\"writable\"]}}}}", "sortMode=$unchanged"},
    {CmdId::ConfigGet, L"config.get", L"Config", L"设置", L"读取设置项", true, Effects{1, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"设置项的键名（settings.json 里的名字），见 config.list\"}},\"required\":[\"key\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"value\":{}},\"required\":[\"key\"]}", "sortMode=$unchanged"},
    // config.set 不是 destructive（不改磁盘文件），但**不可撤销**：
    // 覆盖了用户原来的值就找不回来了。所以调用方应当先 get 再 set，
    // 并在需要时自行记下旧值 —— 这也是断言要读回来的原因之一。
    {CmdId::ConfigSet, L"config.set", L"Config", L"设置", L"修改设置项", true, Effects{0, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"设置项的键名（settings.json 里的名字）\"},\"value\":{\"description\":\"新值；类型须与 config.list 里的 kind 一致\"}},\"required\":[\"key\",\"value\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, nullptr, kExemptConfig},
    };
}

const std::vector<CommandDef>& CommandTable() {
    // 拼一次、全程复用：命令表是只读事实，派发时只查不改。
    static const std::vector<CommandDef> kTable = [] {
        std::vector<CommandDef> all = UserCommandRows();
        std::vector<CommandDef> tools = ToolCommandRows();
        all.insert(all.end(), tools.begin(), tools.end());
        return all;
    }();
    return kTable;
}

const CommandDef* FindCommand(const std::wstring& name) {
    // 规则 C3（docs/NAMING.md §4.3）：所有读命令名的地方都经这里，两个方向都认。
    //
    // 为什么不是"先精确匹配、再试点分"的两段式：那要求调用方知道自己手里是哪种名字。
    // keymap.json 写的是旧名（TabCloseOther），AI 客户端用点分名（tab.close.others），
    // 而 d.name 将来会从旧名迁到点分名 —— 迁移期间三种组合都会出现。所以这里对
    // **任何输入**都试三条路，与 d.name 用哪种写法无关。
    for (const auto& d : CommandTable())
        if (str::IEquals(d.name, name.c_str())) return &d;

    // 输入是旧名 → 换成点分名再匹配（d.name 已是点分名的情形）
    const std::wstring dotted = cmdnaming::AliasOf(name);
    if (!dotted.empty()) {
        for (const auto& d : CommandTable())
            if (str::IEquals(d.name, dotted.c_str())) return &d;
    }

    // 输入是点分名 → 换成旧名再匹配（d.name 仍是旧名的情形，即当前主流）
    if (cmdnaming::LooksDotted(name)) {
        const std::wstring legacy = cmdnaming::LegacyOf(name);
        if (!legacy.empty()) {
            for (const auto& d : CommandTable())
                if (str::IEquals(d.name, legacy.c_str())) return &d;
        }
    }
    return nullptr;
}

const CommandDef* FindCommand(CmdId id) {
    for (const auto& d : CommandTable())
        if (d.id == id) return &d;
    return nullptr;
}

