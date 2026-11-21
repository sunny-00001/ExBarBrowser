#include "commands.h"

#include "str.h"

#include "cmd_naming.h"
#include "common.h"
#include "config.h"
#include "text_file.h"

#include <algorithm>

namespace {

namespace {

// 无参命令的 schema（119 条命令都用它）。
//
// 以前这一行 41 字符的字面量在表里抄了 119 遍，占全表体积的一半以上，
// 也让"哪些命令真的带参数"变得看不出来 —— 扫一眼全是同一个串。
// 提成常量后，带参数的条目会自然凸显。
//
// 语义：显式声明"不接受任何参数"（additionalProperties:false），
// 这样 AI 多传一个字段会被 schema 校验拒绝，而不是被静默忽略。
constexpr const char* kNoArgs = R"({"type":"object","additionalProperties":false})";

}  // namespace

const std::vector<CommandDef> kTable = {
    // ---- Tab ----
    {CmdId::TabNew, L"tab.new", L"Tab", L"标签栏", L"新建标签页", true, Effects{0, 0, 0, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要打开的目录；省略则打开此电脑（与 + 按钮一致）\"}},\"additionalProperties\":false}", true, false},
    // 【为什么要求 HasMultipleTabs】关掉最后一个标签 = 关闭程序（CloseTab 里
    // Count()==0 会 DestroyWindow）。而 win.quit 特意设了 aiExposed=false，
    // 理由是"退出程序不该由机器替人决定"。如果 tab.close 只要 HasTabs，
    // AI 就能用 tab.close 达成同一件事，把那条设计整个绕过去。
    // 单标签时关标签没有"关标签"的意义（等于退出），所以拒绝是语义正确的。
    {CmdId::TabClose, L"tab.close", L"Tab", L"标签栏", L"关闭标签页", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs, Req::HasMultipleTabs}},
    {CmdId::TabCloseOther, L"tab.close.others", L"Tab", L"标签栏", L"关闭其他标签", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::TabCloseLeft, L"tab.close.left", L"Tab", L"标签栏", L"关闭左侧标签", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::TabCloseRight, L"tab.close.right", L"Tab", L"标签栏", L"关闭右侧标签", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::TabClone, L"tab.clone", L"Tab", L"标签栏", L"复制当前标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::TabReopenClosed, L"tab.reopen", L"Tab", L"标签栏", L"重新打开关闭的标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasClosedTabs}},
    {CmdId::TabMoveLeft, L"tab.move.left", L"Tab", L"标签栏", L"标签左移", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{Req::CanMoveTabLeft}},
    {CmdId::TabMoveRight, L"tab.move.right", L"Tab", L"标签栏", L"标签右移", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{Req::CanMoveTabRight}},
    {CmdId::TabMoveFirst, L"tab.move.first", L"Tab", L"标签栏", L"标签移到最前", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{Req::CanMoveTabLeft}},
    {CmdId::TabMoveLast, L"tab.move.last", L"Tab", L"标签栏", L"标签移到最后", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{Req::CanMoveTabRight}},
    {CmdId::TabPrevTab, L"tab.activate.mru", L"Tab", L"标签栏", L"回到上一个激活的标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::TabLast, L"tab.activate.last", L"Tab", L"标签栏", L"最后一个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::CloseLeftOne, L"tab.close.prev", L"Tab", L"标签栏", L"关闭左侧一个", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs, Req::CanMoveTabLeft}},
    {CmdId::CloseRightOne, L"tab.close.next", L"Tab", L"标签栏", L"关闭右侧一个", true, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs, Req::CanMoveTabRight}},
    {CmdId::TabActivate, L"tab.activate", L"Tab", L"标签栏", L"切换到第 N 个标签", true, Effects{0, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"0 起的标签序号\"}},\"required\":[\"index\"],\"additionalProperties\":false}", true, false, Requirements{Req::HasTabs}, nullptr, "activeTab=$arg:index"},
    {CmdId::GoTab1, L"tab.goto.1", L"Tab", L"标签栏", L"切换到第 1 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::GoTab2, L"tab.goto.2", L"Tab", L"标签栏", L"切换到第 2 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::GoTab3, L"tab.goto.3", L"Tab", L"标签栏", L"切换到第 3 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::GoTab4, L"tab.goto.4", L"Tab", L"标签栏", L"切换到第 4 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::GoTab5, L"tab.goto.5", L"Tab", L"标签栏", L"切换到第 5 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::GoTab6, L"tab.goto.6", L"Tab", L"标签栏", L"切换到第 6 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::GoTab7, L"tab.goto.7", L"Tab", L"标签栏", L"切换到第 7 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::GoTab8, L"tab.goto.8", L"Tab", L"标签栏", L"切换到第 8 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::GoTab9, L"tab.goto.9", L"Tab", L"标签栏", L"切换到第 9 个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::TabNext, L"tab.next", L"Tab", L"标签栏", L"下一个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::TabPrev, L"tab.prev", L"Tab", L"标签栏", L"上一个标签", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    // 单窗口模型下没有"合并 explorer 标签"这回事，保留在目录里只为让 keymap 不静默丢键。
    {CmdId::TabMergeAll, L"tab.merge", L"Tab", L"标签栏", L"合并所有窗口标签（不适用）", false, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, false, false},
    // ---- Go ----
    {CmdId::GoHome, L"go.home", L"Go", L"导航栏", L"回到主文件夹", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoDesktop, L"go.desktop", L"Go", L"导航栏", L"桌面", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoDownloads, L"go.downloads", L"Go", L"导航栏", L"下载", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoDocuments, L"go.documents", L"Go", L"导航栏", L"文档", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoConfigPath, L"go.config", L"Go", L"导航栏", L"配置目录", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoLocalAppData, L"go.localappdata", L"Go", L"导航栏", L"LocalAppData", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoControlPanel, L"go.controlpanel", L"Go", L"导航栏", L"控制面板", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoStartFolder, L"go.startfolder", L"Go", L"导航栏", L"开始菜单文件夹", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoThisPC, L"go.thispc", L"Go", L"导航栏", L"此电脑", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::GoPowerShell, L"go.powershell", L"Go", L"导航栏", L"PowerShell 目录", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    // ---- Nav ----
    {CmdId::NavBack, L"nav.back", L"Nav", L"导航栏", L"后退", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasHistoryBack}},
    {CmdId::NavForward, L"nav.forward", L"Nav", L"导航栏", L"前进", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasHistoryForward}},
    {CmdId::NavUp, L"nav.up", L"Nav", L"导航栏", L"向上一级", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::CanGoUp}},
    {CmdId::NavRefresh, L"nav.refresh", L"Nav", L"导航栏", L"刷新", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::NavHome, L"nav.home", L"Nav", L"导航栏", L"回到主文件夹", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::NavViewMenu, L"nav.viewmenu", L"Nav", L"导航栏", L"视图菜单", true, Effects{0, 0, 0, 1, 0, Scope::App}, kNoArgs, false, false},
    // ---- Path ----
    {CmdId::PathCopy, L"path.copy", L"Path", L"导航栏", L"复制完整路径", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::PathCopyName, L"path.copy.name", L"Path", L"导航栏", L"复制文件名", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::PathCopyDir, L"path.copy.dir", L"Path", L"导航栏", L"复制当前目录", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::PathCopyUnc, L"path.copy.unc", L"Path", L"导航栏", L"复制 UNC 路径", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, true},
    {CmdId::PathCopyExt, L"path.copy.ext", L"Path", L"导航栏", L"复制扩展名", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, true},
    {CmdId::PathCopyTab, L"path.copy.tab", L"Path", L"导航栏", L"复制所有标签路径", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::PathCopyLink, L"path.copy.link", L"Path", L"导航栏", L"复制为快捷方式", false, Effects{0, 1, 0, 1, 0, Scope::CurrentTab}, kNoArgs, false, false},
    // ---- File ----
    {CmdId::CmdNewTextFile, L"file.new.text", L"File", L"文件视图", L"新建文本文件", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::CmdOpenWith, L"file.open.with", L"File", L"文件视图", L"打开方式", true, Effects{0, 0, 0, 1, 0, Scope::System}, kNoArgs, false, true, Requirements{Req::HasSelectionSingle}},
    {CmdId::CmdNewFolder, L"file.new.folder", L"File", L"文件操作", L"新建文件夹", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::CmdCopy, L"file.copy", L"File", L"文件操作", L"复制", true, Effects{0, 0, 1, 1, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::CmdCut, L"file.cut", L"File", L"文件操作", L"剪切", true, Effects{0, 1, 0, 1, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::CmdPaste, L"file.paste", L"File", L"文件操作", L"粘贴", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasClipboard}},
    {CmdId::CmdDelete, L"file.delete", L"File", L"文件操作", L"删除", true, Effects{0, 1, 0, 0, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::CmdRename, L"file.rename", L"File", L"文件操作", L"重命名", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    // 【Effects 必须按物理后果声明，不按"这个命令看起来像什么"】
    // 打开属性对话框**能改东西**：只读位、隐藏属性、时间戳。所以 readOnly 必须是 0，
    // 与同义的 fs.properties 保持同一口径（那一行本来就是 0）。
    // 以前这里标 readOnly=1，而 AI 客户端正是按 annotations 决定"要不要先弹确认" ——
    // 两套口径等于契约不可信。
    {CmdId::CmdProps, L"file.properties", L"File", L"文件操作", L"属性", true, Effects{0, 0, 1, 1, 0, Scope::System}, kNoArgs, false, true, Requirements{Req::HasSelectionSingle}},
    {CmdId::CmdUndo, L"file.undo", L"File", L"文件操作", L"撤销", true, Effects{0, 0, 0, 0, 1, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::CmdSelectAll, L"file.select.all", L"File", L"文件操作", L"全选", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::CmdOpen, L"file.open", L"File", L"文件操作", L"打开", true, Effects{0, 0, 0, 1, 0, Scope::System}, kNoArgs, false, true},
    {CmdId::CmdOpenInNewTab, L"file.open.tab", L"File", L"文件操作", L"在新标签打开", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, false, true},
    {CmdId::CmdOpenInNewWindow, L"file.open.window", L"File", L"文件操作", L"在新窗口打开", true, Effects{0, 0, 0, 1, 0, Scope::System}, kNoArgs, false, true},
    {CmdId::SelectInvert, L"file.select.invert", L"File", L"文件视图", L"反向选择", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::SelectFiles, L"file.select.files", L"File", L"文件视图", L"仅选文件", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::SelectFolders, L"file.select.folders", L"File", L"文件视图", L"仅选文件夹", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::SelectByType, L"file.select.type", L"File", L"文件视图", L"按类型选择", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, false, true, Requirements{Req::HasSelection}},
    {CmdId::FileFocusTop, L"file.focus.top", L"File", L"文件视图", L"定位到第一项", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::FileFocusBottom, L"file.focus.bottom", L"File", L"文件视图", L"定位到最后一项", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::CmdRefresh, L"file.refresh", L"File", L"文件视图", L"刷新", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    // 复制/移动到指定文件夹（IFileOperation + 目录选择器），与 mod 的同名命令对齐。
    {CmdId::CopyToFolder, L"file.copy.to", L"File", L"文件操作", L"复制到文件夹", true, Effects{0, 1, 0, 0, 0, Scope::CurrentTab}, kNoArgs, false, true},
    {CmdId::MoveToFolder, L"file.move.to", L"File", L"文件操作", L"移动到文件夹", true, Effects{0, 1, 0, 0, 0, Scope::CurrentTab}, kNoArgs, false, true},
    // 批量重命名交给 PowerRename，按模式选择需要"选中集"抽象，本轮不造。
    // 批量重命名：destructive=true（可能不可逆），但 undoable 走的是 shell 的撤销栈
    // （每个改名都是 FOF_ALLOWUNDO），所以 Ctrl+Z 能退回 —— 见 filetools::PlanBatchRename。
    {CmdId::BatchRename, L"file.rename.batch", L"File", L"文件操作", L"批量重命名", true, Effects{0, 1, 0, 0, 1, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"命名模式：{name} 原名（不含扩展名）、{ext} 扩展名、{old} 原名全称、{n}/{nn}/{nnn} 从 1 起的序号（补零）\"},\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"要改名的完整路径；省略则用当前选中项（必须在当前目录内）\"}},\"required\":[\"pattern\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"renamed\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"from\",\"to\"]}},\"failed\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"改失败的原名（被占用/权限不足）\"},\"skippedSame\":{\"type\":\"integer\",\"description\":\"算出的名字与原名相同、被跳过的项数\"}},\"required\":[\"pattern\",\"renamed\",\"failed\"]}"},
    {CmdId::SelectByPattern, L"file.select.pattern", L"File", L"文件视图", L"按模式选择", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"匹配模式，分号分隔多个，如 *.txt;*.md；省略则弹输入框\"},\"folders\":{\"type\":\"boolean\",\"description\":\"是否也匹配文件夹（默认 false）\"},\"caseSensitive\":{\"type\":\"boolean\",\"description\":\"区分大小写（默认 false）\"}},\"additionalProperties\":false}", true, false},
    // ---- 确定式文件操作（P5：目标由参数点名，绝不弹框、绝不读选中）----
    // 与上面的 CmdRename/CmdDelete/CopyToFolder/MoveToFolder 是同一个业务的两种入口。
    // 之所以要单独一组而不是给旧命令加参数：旧命令的语义里"弹框问用户"是不可分割的
    // 一部分（F2 重命名的体验就是"弹出输入框"），加参数会变成"传了参数就不弹框"
    // 的隐式分叉。拆成两条命令后，人和机器各走各的入口，共用同一份实现。
    {CmdId::FsRename, L"fs.rename", L"File", L"文件操作", L"重命名（指定目标）", true, Effects{0, 0, 0, 0, 1, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"description\":\"要重命名的项目完整路径\"},\"newName\":{\"type\":\"string\",\"description\":\"新名字（只给名字，不含路径）\"}},\"required\":[\"target\",\"newName\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsDelete, L"fs.delete", L"File", L"文件操作", L"删除（指定目标）", true, Effects{0, 1, 0, 0, 1, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"要删除的项目完整路径列表\"}},\"required\":[\"targets\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsCopyTo, L"fs.copy", L"File", L"文件操作", L"复制到（指定目标）", true, Effects{0, 0, 0, 0, 1, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"to\":{\"type\":\"string\",\"description\":\"目标文件夹完整路径\"}},\"required\":[\"targets\",\"to\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsMoveTo, L"fs.move", L"File", L"文件操作", L"移动到（指定目标）", true, Effects{0, 0, 0, 0, 1, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"to\":{\"type\":\"string\",\"description\":\"目标文件夹完整路径\"}},\"required\":[\"targets\",\"to\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsOpen, L"fs.open", L"File", L"文件操作", L"打开（指定路径）", true, Effects{0, 0, 0, 1, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要打开的完整路径（文件夹会在当前标签内进入）\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsOpenInNewTab, L"fs.open.tab", L"File", L"文件操作", L"打开（新标签）", true, Effects{0, 0, 0, 1, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"目标的完整路径\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsOpenInNewWindow, L"fs.open.window", L"File", L"文件操作", L"打开（新窗口）", true, Effects{0, 0, 0, 1, 0, Scope::System}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"目标的完整路径\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsCopy, L"fs.copy.toclip", L"File", L"文件操作", L"复制到剪贴板（指定路径）", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"一组完整路径\"}},\"required\":[\"targets\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsCut, L"fs.cut.toclip", L"File", L"文件操作", L"剪切到剪贴板（指定路径）", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"一组完整路径\"}},\"required\":[\"targets\"],\"additionalProperties\":false}", true, false},
    {CmdId::FsProperties, L"fs.properties", L"File", L"文件操作", L"属性（指定路径）", true, Effects{0, 0, 0, 1, 0, Scope::System}, "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"description\":\"目标的完整路径\"}},\"required\":[\"target\"],\"additionalProperties\":false}", true, false},
    // ---- View ----
    {CmdId::ViewMode, L"view.mode", L"View", L"文件视图", L"视图模式", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"preset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7,\"description\":\"0=超大图标 1=大图标 2=中图标 3=小图标 4=列表 5=详细信息 6=平铺 7=内容\"}},\"required\":[\"preset\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "viewMode=$arg:preset"},
    {CmdId::ViewSort, L"view.sort", L"View", L"文件视图", L"排序方式", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"by\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3,\"description\":\"0=名称 1=日期 2=类型 3=大小\"}},\"required\":[\"by\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "sortMode=$arg:by"},
    {CmdId::ViewGroup, L"view.group", L"View", L"文件视图", L"分组方式", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"by\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3,\"description\":\"0=不分组 1=类型 2=日期 3=大小\"}},\"required\":[\"by\"],\"additionalProperties\":false}", true, false, Requirements{}, nullptr, "groupBy=$arg:by"},
    {CmdId::ToggleHidden, L"view.toggle.hidden", L"View", L"文件视图", L"显示隐藏文件", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::ToggleSystemFiles, L"view.toggle.system", L"View", L"文件视图", L"显示系统文件", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::ToggleExtensions, L"view.toggle.extensions", L"View", L"文件视图", L"显示扩展名", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::CycleHiddenSeg, L"view.cycle.hidden", L"View", L"文件视图", L"切换隐藏文件三态", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::TogglePreviewPane, L"view.toggle.preview", L"View", L"文件视图", L"预览窗格", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::ToggleNavPane, L"view.toggle.nav", L"View", L"文件视图", L"导航窗格", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::ToggleCheckboxes, L"view.toggle.checkboxes", L"View", L"文件视图", L"复选框", false, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    {CmdId::ViewNext, L"view.next", L"View", L"文件视图", L"下一个视图模式", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    {CmdId::ViewPrev, L"view.prev", L"View", L"文件视图", L"上一个视图模式", true, Effects{0, 0, 0, 0, 0, Scope::CurrentTab}, kNoArgs, true, false},
    // ---- Window ----
    {CmdId::NewWindow, L"win.new", L"Window", L"窗口", L"新窗口", true, Effects{0, 0, 0, 0, 0, Scope::System}, kNoArgs, true, false},
    {CmdId::QuitExplorer, L"win.quit", L"Window", L"全局", L"退出程序", true, Effects{0, 1, 0, 0, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::FolderOptions, L"win.folderoptions", L"Window", L"系统对话框", L"文件夹选项", true, Effects{0, 0, 1, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::MapDrive, L"win.mapdrive", L"Window", L"系统对话框", L"映射网络驱动器", true, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::UnmapDrive, L"win.unmapdrive", L"Window", L"系统对话框", L"断开网络驱动器", true, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::CommandPalette, L"win.palette", L"Window", L"窗口", L"命令面板", true, Effects{1, 0, 0, 0, 0, Scope::App}, kNoArgs, false, false},
    {CmdId::HotkeyManager, L"win.hotkeys", L"Window", L"窗口", L"快捷键管理器", true, Effects{0, 1, 0, 0, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::FocusAddress, L"win.focus.address", L"Window", L"导航栏", L"聚焦地址栏", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false},
    {CmdId::FocusSearch, L"win.focus.search", L"Window", L"导航栏", L"聚焦搜索框", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false},
    // 原声明是 implemented=false + destructive=1 + Scope::System，三条都不对：
    // 面板早就有（导航栏右键「设置…」），开的是一扇窗、不改任何不可逆状态，
    // 也不碰进程外。声明错了的直接后果是命令面板里搜不到它、键位绑不上。
    {CmdId::Preferences, L"win.preferences", L"Window", L"窗口", L"首选项", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, false, false},
    // ---- External ----
    {CmdId::OpenTerminal, L"ext.terminal", L"External", L"导航栏", L"在此打开终端", true, Effects{0, 0, 0, 1, 0, Scope::System}, kNoArgs, true, false},
    {CmdId::OpenTerminalAdmin, L"ext.terminal.admin", L"External", L"导航栏", L"管理员终端", true, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, true, false},
    {CmdId::RunCommand, L"ext.run", L"External", L"全局", L"运行命令", false, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::SendKeys, L"ext.sendkeys", L"External", L"全局", L"发送按键", false, Effects{0, 1, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    // ---- Bookmark ----
    {CmdId::BmkAddCurrent, L"bmk.add.current", L"Bookmark", L"书签栏", L"收藏当前目录", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true, false},
    {CmdId::BmkShowBar, L"bmk.show", L"Bookmark", L"书签栏", L"显示书签栏", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "bookmarkBarVisible=true"},
    {CmdId::BmkHideBar, L"bmk.hide", L"Bookmark", L"书签栏", L"隐藏书签栏", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, nullptr, "bookmarkBarVisible=false"},
    {CmdId::BmkToggleBar, L"bmk.toggle", L"Bookmark", L"书签栏", L"切换书签栏", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false},
    // 书签编辑族：原来只有右键菜单可达，补成命令后键盘/面板/AI 与人等价。
    // 删除/清空/排序都进撤销栈，所以 destructive=0（可回退）；undoable=1。
    {CmdId::BmkEdit, L"bmk.edit", L"Bookmark", L"书签栏", L"编辑书签", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"书签 id\"},\"name\":{\"type\":\"string\",\"description\":\"新名称\"},\"path\":{\"type\":\"string\",\"description\":\"新路径；空串表示改成纯分类\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true , false},
    {CmdId::BmkRemove, L"bmk.remove", L"Bookmark", L"书签栏", L"删除书签", true, Effects{0, 1, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"书签 id\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true , false},
    {CmdId::BmkNewFolder, L"bmk.newfolder", L"Bookmark", L"书签栏", L"新建分类", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"分类名称\"},\"parentId\":{\"type\":\"string\",\"description\":\"父分类 id；省略=顶层\"}},\"additionalProperties\":false}", true , false},
    {CmdId::BmkUndo, L"bmk.undo", L"Bookmark", L"书签栏", L"撤销书签操作", true, Effects{0, 0, 0, 0, 1, Scope::App}, kNoArgs, true , false},
    {CmdId::BmkClearAll, L"bmk.clear", L"Bookmark", L"书签栏", L"清除全部书签", true, Effects{0, 1, 0, 0, 1, Scope::App}, kNoArgs, true , false},
    {CmdId::BmkSort, L"bmk.sort", L"Bookmark", L"书签栏", L"按名称排序", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"recursive\":{\"type\":\"boolean\",\"description\":\"是否连分类内部一起排，默认 true\"},\"ascending\":{\"type\":\"boolean\",\"description\":\"升序（默认 true），false 为降序\"}},\"additionalProperties\":false}", true , false},
    {CmdId::BmkDedupe, L"bmk.dedupe", L"Bookmark", L"书签栏", L"清理重复书签", true, Effects{0, 1, 0, 0, 1, Scope::App}, kNoArgs, true , false},
    // ---- 补齐三处"只有鼠标能做"的动作（拖放添加 / 拖拽重排 / 读取树）----
    // 前两条与拖放走**同一份实现**：菜单、拖放、脚本、AI 都调它们。
    {CmdId::BmkAdd, L"bmk.add.path", L"Bookmark", L"书签栏", L"添加书签（指定路径）", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要收藏的目录完整路径\"},\"name\":{\"type\":\"string\",\"description\":\"显示名；省略则取末段\"},\"parentId\":{\"type\":\"string\",\"description\":\"父分类 id；省略=顶层\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false},
    {CmdId::BmkMove, L"bmk.move", L"Bookmark", L"书签栏", L"移动书签", true, Effects{0, 0, 0, 0, 1, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"要移动的书签 id\"},\"toId\":{\"type\":\"string\",\"description\":\"目标分类 id（移入它内部）\"},\"toIndex\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"在目标内的位置；省略=末尾\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true, false},
    // bmk.list 是**读**：其余 bmk.* 要的 id 由它给出。没有它，
    // AI 拿到"删除书签"却无从知道有哪些 id —— 命令表就是断的。
    {CmdId::BmkList, L"bmk.list", L"Bookmark", L"书签栏", L"列出书签", true, Effects{1, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"parentId\":{\"type\":\"string\",\"description\":\"只列该分类下；省略=整棵树\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"entries\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"isFolder\":{\"type\":\"boolean\"},\"parentId\":{\"type\":\"string\"}},\"required\":[\"id\",\"name\",\"isFolder\"]}}}}"},
    // ---- 设置域 ----
    // config.list 是「能枚举」的落地：AI 先问有哪些项，再按 key 读/写，
    // 全程不依赖界面文案或坐标。
    {CmdId::ConfigList, L"config.list", L"Config", L"设置", L"列出全部设置项", true, Effects{1, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"items\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"settings.json 里的键名，读写都用它\"},\"kind\":{\"type\":\"string\",\"description\":\"bool / int / string / string[] / int[] / object(readonly)\"},\"doc\":{\"type\":\"string\",\"description\":\"中文名与说明\"},\"writable\":{\"type\":\"boolean\",\"description\":\"false = 只读，doc 会说明该用哪个入口\"},\"value\":{},\"min\":{\"type\":\"integer\"},\"max\":{\"type\":\"integer\"}},\"required\":[\"key\",\"kind\",\"writable\"]}}}}"},
    {CmdId::ConfigGet, L"config.get", L"Config", L"设置", L"读取设置项", true, Effects{1, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"设置项的键名（settings.json 里的名字），见 config.list\"}},\"required\":[\"key\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"value\":{}},\"required\":[\"key\"]}"},
    // config.set 不是 destructive（不改磁盘文件），但**不可撤销**：
    // 覆盖了用户原来的值就找不回来了。所以调用方应当先 get 再 set，
    // 并在需要时自行记下旧值 —— 这也是断言要读回来的原因之一。
    {CmdId::ConfigSet, L"config.set", L"Config", L"设置", L"修改设置项", true, Effects{0, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"设置项的键名（settings.json 里的名字）\"},\"value\":{\"description\":\"新值；类型须与 config.list 里的 kind 一致\"}},\"required\":[\"key\",\"value\"],\"additionalProperties\":false}", true, false},

    // ---- 开发面：让 Agent 能自己完成「读现状 → 改 → 编译 → 测 → 回滚」----
    // 这五条不新增业务逻辑，只是把仓库里已有的 build.sh / tests/run_all.sh / git
    // 收进同一条命令总线。人从命令面板可达（R20），机器从 AI 通道可达。
    {CmdId::DevSnapshot, L"dev.snapshot", L"Dev", L"开发", L"读取代码工作树快照", true, Effects{1, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"head\":{\"type\":\"string\"},\"status\":{\"type\":\"string\"},\"diffStat\":{\"type\":\"string\"},\"repo\":{\"type\":\"string\"},\"profile\":{\"type\":\"string\"},\"bash\":{\"type\":\"string\"},\"python\":{\"type\":\"string\"},\"error\":{\"type\":\"string\"}},\"required\":[\"head\",\"status\"]}"},
    {CmdId::DevBuild, L"dev.build", L"Dev", L"开发", L"异步编译当前工作树", true, Effects{0, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"description\":\"可选：编译目标，默认整个工程\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"jobId\":{\"type\":\"string\"}},\"required\":[\"jobId\"]}"},
    {CmdId::DevTest, L"dev.test", L"Dev", L"开发", L"异步运行测试", true, Effects{0, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\",\"description\":\"只跑名字含该串的测试；省略=全量\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"jobId\":{\"type\":\"string\"}},\"required\":[\"jobId\"]}"},
    {CmdId::DevJob, L"dev.job", L"Dev", L"开发", L"查询异步作业结果", true, Effects{1, 0, 1, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"dev.build/dev.test 返回的 jobId\"}},\"required\":[\"id\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\"},\"done\":{\"type\":\"boolean\"},\"exitCode\":{\"type\":\"integer\"},\"output\":{\"type\":\"string\"},\"error\":{\"type\":\"string\"}},\"required\":[\"id\",\"done\"]}"},
    // 不可撤销：回滚会丢掉工作树里对该文件的改动。调用方必须先 dev.snapshot
    // 记下 before，并在需要时自行备份；approve=true 表示人已确认这一次覆盖。
    {CmdId::DevRollback, L"dev.rollback", L"Dev", L"开发", L"把仓库内文件恢复到 HEAD", true, Effects{0, 1, 0, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"仓库内相对路径，如 src/commands.cpp\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"before\":{\"type\":\"string\"},\"after\":{\"type\":\"string\"}},\"required\":[\"before\",\"after\"]}"},
    {CmdId::BmkFocus, L"bmk.focus", L"Bookmark", L"书签栏", L"聚焦书签栏", true, Effects{0, 0, 1, 0, 0, Scope::App}, kNoArgs, true, false},
    // ---- Session ----
    {CmdId::SessionSave, L"session.save", L"Session", L"标签栏", L"保存标签组", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false},
    {CmdId::SessionRestore, L"session.restore", L"Session", L"标签栏", L"恢复标签组", true, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, true, false},
    {CmdId::SessionDelete, L"session.delete", L"Session", L"标签栏", L"删除标签组", false, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, false, false},
    {CmdId::SessionList, L"session.list", L"Session", L"标签栏", L"列出标签组", false, Effects{1, 0, 1, 0, 0, Scope::App}, kNoArgs, false, false},
    // ---- 占位（未实现）----
    // 文件集工具已实现（2026-11-21）：见 app_commands_files.cpp 末尾三条 regR。
    // 它们的"人工路径"是**不带参数**调用（目标取界面现状），agent 路径带参数返回 JSON。
    {CmdId::DupFinder, L"tool.dupfinder", L"Tool", L"文件视图", L"查找重复文件", true, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要扫描的目录；省略则用当前目录\"},\"budgetMs\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":30000,\"description\":\"时间预算（毫秒）；默认 1500，超时返回已找到的部分并置 timedOut\"},\"minBytes\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"小于此字节数的文件不参与；默认 1（排除 0 字节占位文件）\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"groups\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"bytes\":{\"type\":\"integer\",\"description\":\"组内每份的大小\"},\"hash\":{\"type\":\"string\",\"description\":\"SHA-256（小写十六进制）\"},\"files\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"bytes\":{\"type\":\"integer\"}},\"required\":[\"path\"]}}},\"required\":[\"bytes\",\"hash\",\"files\"]}},\"wastedBytes\":{\"type\":\"integer\",\"description\":\"保留一份后可回收的字节数\"},\"scannedFiles\":{\"type\":\"integer\"},\"hashedFiles\":{\"type\":\"integer\",\"description\":\"实际计算摘要的文件数（含未进结果的候选）\"},\"deniedDirs\":{\"type\":\"integer\",\"description\":\"打不开的目录数（>0 说明结果偏少）\"},\"timedOut\":{\"type\":\"boolean\"},\"complete\":{\"type\":\"boolean\",\"description\":\"是否扫完整个子树\"},\"elapsedMs\":{\"type\":\"integer\"}},\"required\":[\"path\",\"groups\",\"wastedBytes\",\"complete\"]}"},
    {CmdId::FileHash, L"tool.hash", L"Tool", L"文件视图", L"计算哈希", true, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"要计算摘要的文件路径；省略则用当前选中项\"},\"algorithm\":{\"type\":\"string\",\"enum\":[\"md5\",\"sha1\",\"sha256\"],\"description\":\"摘要算法；默认 sha256。认不出的名字会失败，不会退回默认值\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"algorithm\":{\"type\":\"string\"},\"entries\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"ok\":{\"type\":\"boolean\"},\"hash\":{\"type\":\"string\",\"description\":\"小写十六进制；ok=false 时缺省\"},\"bytes\":{\"type\":\"integer\"},\"error\":{\"type\":\"string\",\"description\":\"ok=false 时的原因\"}},\"required\":[\"path\",\"ok\"]}}},\"required\":[\"algorithm\",\"entries\"]}"},
    {CmdId::FolderCompare, L"tool.compare", L"Tool", L"文件视图", L"比较文件夹", true, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"left\":{\"type\":\"string\",\"description\":\"左侧目录\"},\"right\":{\"type\":\"string\",\"description\":\"右侧目录\"},\"byContent\":{\"type\":\"boolean\",\"description\":\"true=同大小再比 SHA-256（默认）；false=只比大小（快，但同大小不同内容会算作相同）\"},\"budgetMs\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":30000,\"description\":\"时间预算（毫秒）；默认 1500\"}},\"required\":[\"left\",\"right\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"left\":{\"type\":\"string\"},\"right\":{\"type\":\"string\"},\"entries\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"rel\":{\"type\":\"string\",\"description\":\"相对路径\"},\"leftBytes\":{\"type\":\"integer\"},\"rightBytes\":{\"type\":\"integer\"},\"state\":{\"type\":\"string\",\"enum\":[\"onlyLeft\",\"onlyRight\",\"differ\"]}},\"required\":[\"rel\",\"state\"]}},\"sameCount\":{\"type\":\"integer\"},\"differCount\":{\"type\":\"integer\"},\"onlyLeftCount\":{\"type\":\"integer\"},\"onlyRightCount\":{\"type\":\"integer\"},\"timedOut\":{\"type\":\"boolean\"},\"complete\":{\"type\":\"boolean\"},\"elapsedMs\":{\"type\":\"integer\"}},\"required\":[\"left\",\"right\",\"entries\",\"complete\"]}"},
    {CmdId::SearchEverything, L"search.everything", L"Search", L"导航栏", L"搜索", false, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    // ---- 只读查询（P6：读操作返回结构化结果）----
    // 这两条是第一批带 outputSchema 的命令，用来验证返回值设计。
    // 它们列/查的是**当前视图**，而不是按路径另扫盘 —— 与用户看到的一致。
    {CmdId::OpListDir, L"tool.list", L"Tool", L"文件视图", L"列出当前目录", true, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000,\"description\":\"最多返回多少项；省略则不限\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"被列出的目录\"},\"truncated\":{\"type\":\"boolean\"},\"entries\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"isFolder\":{\"type\":\"boolean\"},\"size\":{\"type\":\"integer\"},\"hidden\":{\"type\":\"boolean\"}},\"required\":[\"name\",\"isFolder\"]}}},\"required\":[\"path\",\"entries\"]}"},
    {CmdId::OpStat, L"tool.stat", L"Tool", L"文件视图", L"查看单项属性", true, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"项目的完整路径（必须在当前目录内）\"}},\"required\":[\"path\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"isFolder\":{\"type\":\"boolean\"},\"hidden\":{\"type\":\"boolean\"},\"readonly\":{\"type\":\"boolean\"},\"size\":{\"type\":\"integer\"},\"typeName\":{\"type\":\"string\"}},\"required\":[\"name\",\"isFolder\"]}"},
    {CmdId::OpTopN, L"tool.topn", L"Tool", L"文件视图", L"最大的 N 项", true, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要统计的目录；省略则用当前目录\"},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000,\"description\":\"返回前 N 项；默认 50\"},\"budgetMs\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":30000,\"description\":\"时间预算（毫秒），超时则返回已统计部分；默认 1500\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"被统计的目录\"},\"totalBytes\":{\"type\":\"integer\",\"description\":\"目录内容总大小（含被 limit 截断的部分）\"},\"truncated\":{\"type\":\"boolean\",\"description\":\"结果被 limit 截断\"},\"timedOut\":{\"type\":\"boolean\",\"description\":\"到时间预算提前结束，数值不完整\"},\"elapsedMs\":{\"type\":\"integer\"},\"scannedFiles\":{\"type\":\"integer\"},\"scannedDirs\":{\"type\":\"integer\"},\"deniedDirs\":{\"type\":\"integer\",\"description\":\"打不开的目录数（>0 表示结果偏小）\"},\"complete\":{\"type\":\"boolean\",\"description\":\"整次统计是否完整\"},\"entries\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"bytes\":{\"type\":\"integer\"},\"isFolder\":{\"type\":\"boolean\"},\"items\":{\"type\":\"integer\"},\"complete\":{\"type\":\"boolean\"}},\"required\":[\"name\",\"bytes\"]}}},\"required\":[\"path\",\"totalBytes\",\"entries\"]}"},
    {CmdId::OpDiskUsage, L"tool.diskusage", L"Tool", L"文件视图", L"目录占用", true, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"要统计的目录；省略则用当前目录\"},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000,\"description\":\"返回前 N 项；默认 50\"},\"budgetMs\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":30000,\"description\":\"时间预算（毫秒），超时则返回已统计部分；默认 1500\"}},\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"被统计的目录\"},\"totalBytes\":{\"type\":\"integer\",\"description\":\"目录内容总大小（含被 limit 截断的部分）\"},\"truncated\":{\"type\":\"boolean\",\"description\":\"结果被 limit 截断\"},\"timedOut\":{\"type\":\"boolean\",\"description\":\"到时间预算提前结束，数值不完整\"},\"elapsedMs\":{\"type\":\"integer\"},\"scannedFiles\":{\"type\":\"integer\"},\"scannedDirs\":{\"type\":\"integer\"},\"deniedDirs\":{\"type\":\"integer\",\"description\":\"打不开的目录数（>0 表示结果偏小）\"},\"complete\":{\"type\":\"boolean\",\"description\":\"整次统计是否完整\"},\"entries\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"bytes\":{\"type\":\"integer\"},\"isFolder\":{\"type\":\"boolean\"},\"items\":{\"type\":\"integer\"},\"complete\":{\"type\":\"boolean\"}},\"required\":[\"name\",\"bytes\"]}}},\"required\":[\"path\",\"totalBytes\",\"entries\"]}"},
    // 搜索是**导航**（结果落在同一个视图里），不是只读查询 —— 原声明 readOnly=1 就错在这里。
    // 人侧：搜索框直接输入、或命令面板追问搜索词（留空＝退出搜索）；agent 侧：{"query":"..."} → 结构化结果。
    {CmdId::OpSearch, L"search.find", L"Search", L"文件视图", L"搜索", true, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"搜索词；空串＝退出搜索回到来源目录\"}},\"required\":[\"query\"],\"additionalProperties\":false}", true, false, Requirements{}, "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"scope\":{\"type\":\"string\"},\"url\":{\"type\":\"string\"},\"searching\":{\"type\":\"boolean\"}},\"required\":[\"query\",\"scope\",\"searching\"]}"},

    {CmdId::OpReadText, L"tool.readtext", L"Tool", L"文件视图", L"读取文本", false, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    {CmdId::OpNavigate, L"tool.navigate", L"Tool", L"导航栏", L"导航", false, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    {CmdId::OpReveal, L"tool.reveal", L"Tool", L"文件视图", L"定位", false, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    {CmdId::OpSelect, L"tool.select", L"Tool", L"文件视图", L"选择", false, Effects{0, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    {CmdId::OpOpen, L"tool.open", L"Tool", L"文件视图", L"打开", false, Effects{0, 0, 0, 1, 0, Scope::System}, kNoArgs, false, false},
    {CmdId::OpListCurrentView, L"tool.listview", L"Tool", L"文件视图", L"列出当前视图", false, Effects{1, 0, 1, 0, 0, Scope::CurrentTab}, kNoArgs, false, false},
    {CmdId::AiTogglePanel, L"win.ai.toggle", L"Window", L"窗口", L"AI 助手", false, Effects{0, 0, 0, 0, 0, Scope::App}, kNoArgs, false, false},
    {CmdId::AiStop, L"win.ai.stop", L"Window", L"窗口", L"停止 AI", false, Effects{0, 1, 0, 0, 0, Scope::App}, kNoArgs, false, false},

    // ---- 标签个性化 ----
    // 说明：这三条的命令名用 tab.* 域（与右键菜单里的动作一一对应），
    // 但**归类为 Tab 组**而不是新开 "TabMeta" 组 —— 分组是给面板列菜单用的，
    // 三条自成一体会让命令面板多出一个只有三行的组。
    //
    // aiExposed = true 的理由：改名/上色都是可逆、无破坏的窗口内状态变更（Write），
    // 而且 schema 完整点名目标值，机器不需要猜用户选了什么。这正是 R20 想要的形态。
    // 但**不加** HasTabs 之外的前置：改名对任意标签都成立（不像 move 有左右边界）。
    {CmdId::TabRename, L"tab.rename", L"Tab", L"标签栏", L"重命名标签页", true, Effects{0, 0, 0, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"新的标签名；空串表示恢复为跟随目录的默认名\"}},\"required\":[\"name\"],\"additionalProperties\":false}", true, false, Requirements{Req::HasTabs}},
    {CmdId::TabSetColor, L"tab.color", L"Tab", L"标签栏", L"设置标签页颜色", true, Effects{0, 0, 0, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"color\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7,\"description\":\"0=无颜色，1 红 2 橙 3 黄 4 绿 5 蓝 6 紫 7 灰\"}},\"required\":[\"color\"],\"additionalProperties\":false}", true, false, Requirements{Req::HasTabs}},
    {CmdId::TabCloseDuplicates, L"tab.close.duplicates", L"Tab", L"标签栏", L"关闭重复标签页", true, Effects{0, 1, 0, 0, 1, Scope::App}, kNoArgs, true, false, Requirements{Req::HasTabs}},
    {CmdId::TabLock, L"tab.lock", L"Tab", L"标签栏", L"锁定标签页", true, Effects{0, 0, 0, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"locked\":{\"type\":\"boolean\",\"description\":\"true=锁定，false=解锁；省略=切换\"},\"index\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"标签序号；省略=当前标签\"}},\"additionalProperties\":false}", true, false},
    {CmdId::TabCopyTitle, L"tab.copy.title", L"Tab", L"标签栏", L"复制标签名", true, Effects{0, 0, 0, 0, 0, Scope::App}, "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"标签序号；省略=当前标签\"}},\"additionalProperties\":false}", true , false},
};

}  // namespace

const std::vector<CommandDef>& CommandTable() { return kTable; }

const CommandDef* FindCommand(const std::wstring& name) {
    // 规则 C3（docs/NAMING.md §4.3）：所有读命令名的地方都经这里，两个方向都认。
    //
    // 为什么不是"先精确匹配、再试点分"的两段式：那要求调用方知道自己手里是哪种名字。
    // keymap.json 写的是旧名（TabCloseOther），AI 客户端用点分名（tab.close.others），
    // 而 d.name 将来会从旧名迁到点分名 —— 迁移期间三种组合都会出现。所以这里对
    // **任何输入**都试三条路，与 d.name 用哪种写法无关。
    for (const auto& d : kTable)
        if (str::IEquals(d.name, name.c_str())) return &d;

    // 输入是旧名 → 换成点分名再匹配（d.name 已是点分名的情形）
    const std::wstring dotted = cmdnaming::AliasOf(name);
    if (!dotted.empty()) {
        for (const auto& d : kTable)
            if (str::IEquals(d.name, dotted.c_str())) return &d;
    }

    // 输入是点分名 → 换成旧名再匹配（d.name 仍是旧名的情形，即当前主流）
    if (cmdnaming::LooksDotted(name)) {
        const std::wstring legacy = cmdnaming::LegacyOf(name);
        if (!legacy.empty()) {
            for (const auto& d : kTable)
                if (str::IEquals(d.name, legacy.c_str())) return &d;
        }
    }
    return nullptr;
}

const CommandDef* FindCommand(CmdId id) {
    for (const auto& d : kTable)
        if (d.id == id) return &d;
    return nullptr;
}

bool SplitCommandSpec(const std::wstring& spec, std::wstring* name, std::wstring* arg) {
    size_t colon = spec.find(L':');
    if (name) *name = colon == std::wstring::npos ? spec : spec.substr(0, colon);
    if (arg) *arg = colon == std::wstring::npos ? std::wstring() : spec.substr(colon + 1);
    return !spec.empty();
}

// ---------------------------------------------------------------- 参数适配

namespace {

// argsSchema 存的是 UTF-8 的 const char*；转换唯一实现在 str.cpp。
// 空 schema 的业务语义（"{}" = 无参数对象）留在这里。
std::wstring AnsiToWide(const char* s) {
    std::wstring w = str::FromUtf8(s);
    return w.empty() ? std::wstring(L"{}") : w;
}

// 审计里记的参数摘要。用紧凑 JSON，过长时截断 —— 审计是给人查的，
// 不该因为一条超长路径把日志行撑爆。
std::wstring DescribeArgs(const Args& args) {
    if (args.IsEmpty()) return std::wstring();
    std::wstring s = json::Stringify(args.raw(), 0);
    if (s.size() > 160) s = s.substr(0, 157) + L"...";
    return s;
}

// 审计里的参数摘要：失败且 handler 报了原因时，把原因附在同一行。
// 不另开字段，是为了让"一条命令一行"这个性质保持 —— 事后 grep 一条命令
// 就能看到为什么失败，不用在几行之间拼时间戳。
std::wstring ArgsDescForAudit(const Args& args, bool ok, const std::wstring& reason) {
    const std::wstring desc = DescribeArgs(args);
    if (ok || reason.empty()) return desc;
    return desc.empty() ? (L"(failed: " + reason + L")") : (desc + L" (failed: " + reason + L")");
}

// 从 schema 里挑出"旧字符串参数该落到哪个键"。
// 规则：优先 required[0]，否则 properties 的第一个键。这是唯一合理的默认 ——
// 旧规格 "ViewMode:3" 里那个 3 只可能对应命令声明的那个必填档位。
bool PickSingleKey(const json::Value& schema, std::wstring* key, std::wstring* type) {
    const json::Value* req = schema.Find(L"required");
    const json::Value* props = schema.Find(L"properties");
    if (!props || !props->IsObject() || props->obj.empty()) return false;

    if (req && req->IsArray() && !req->arr.empty()) {
        const json::Value& first = req->arr[0];
        if (first.IsString()) {
            const json::Value* prop = props->Find(first.str.c_str());
            if (prop) {
                *key = first.str;
                *type = prop->GetStr(L"type");
                return true;
            }
        }
    }
    *key = props->obj[0].first;
    *type = props->obj[0].second.GetStr(L"type");
    return true;
}

}  // namespace

Args ArgsFromLegacyArg(const CommandDef& def, const std::wstring& legacyArg) {
    if (legacyArg.empty()) return Args::Empty();

    json::Value schema;
    std::wstring perr;
    if (!def.argsSchema || !json::Parse(std::wstring(AnsiToWide(def.argsSchema)), schema, &perr) ||
        !schema.IsObject())
        return Args::Empty();

    std::wstring key, type;
    if (!PickSingleKey(schema, &key, &type)) return Args::Empty();

    json::Value out = json::Value::MakeObject();
    // 按 schema 声明的类型决定存数字还是字符串。以前所有参数都是字符串，
    // handler 各自 _wtoi；现在类型由 schema 决定，handler 直接取 Int()。
    if (type == L"integer" || type == L"number") {
        out.Set(key, json::Value::MakeInt(_wtoi(legacyArg.c_str())));
    } else if (type == L"boolean") {
        out.Set(key, json::Value::MakeBool(legacyArg == L"1" || str::IEquals(legacyArg.c_str(), L"true")));
    } else {
        out.Set(key, json::Value::MakeStr(legacyArg));
    }
    return Args::FromJson(out);
}

// ---------------------------------------------------------------- 可否执行

namespace {

StateProbe g_probe;

}  // namespace

void SetStateProbe(StateProbe fn) { g_probe = std::move(fn); }

const wchar_t* ReqReason(Req r) {
    switch (r) {
    case Req::None:              return L"";
    case Req::HasHistoryBack:    return L"已经是第一条历史";
    case Req::HasHistoryForward: return L"已经是最后一条历史";
    case Req::HasSelection:      return L"未选中任何项目";
    case Req::HasTabs:           return L"没有打开的标签";
    case Req::HasClipboard:      return L"剪贴板没有可粘贴的内容";
    case Req::CanGoUp:           return L"已经是顶层（此电脑或网络根）";
    case Req::StateUnavailable:  return L"界面尚未就绪";
    case Req::CanMoveTabLeft:    return L"当前标签已在最左";
    case Req::CanMoveTabRight:   return L"当前标签已在最右";
    case Req::HasClosedTabs:     return L"没有可重新打开的标签";
    case Req::HasMultipleTabs:   return L"只有一个标签";
    case Req::HasSelectionSingle: return L"需要恰好选中一个项目";
    }
    return L"";
}

bool CanExecute(const CommandDef& def, std::wstring* reason) {
    auto deny = [reason](Req r) {
        if (reason) *reason = ReqReason(r);
        return false;
    };

    if (def.requires_.Empty()) {
        if (reason) reason->clear();
        return true;
    }

    // 未注册探测 = 不知道状态 = 不能声称可执行（失败关闭）。
    // 返回一个明确原因而不是空串，否则 UI 会显示"灰按钮但没有解释"。
    if (!g_probe) return deny(Req::StateUnavailable);

    const CommandState st = g_probe();
    for (Req r : def.requires_.items) {
        if (r == Req::None) continue;
        switch (r) {
        case Req::HasHistoryBack:    if (st.historyBack <= 0) return deny(r); break;
        case Req::HasHistoryForward: if (st.historyForward <= 0) return deny(r); break;
        case Req::HasSelection:      if (!st.hasSelection) return deny(r); break;
        case Req::HasTabs:           if (st.tabCount <= 0) return deny(r); break;
        case Req::HasClipboard:      if (!st.hasClipboard) return deny(r); break;
        case Req::CanGoUp:           if (!st.canGoUp) return deny(r); break;
        case Req::CanMoveTabLeft:    if (st.activeTabIndex <= 0) return deny(r); break;
        case Req::CanMoveTabRight:
            if (st.activeTabIndex >= st.tabCount - 1) return deny(r);
            break;
        case Req::HasClosedTabs:     if (!st.hasClosedTabs) return deny(r); break;
        case Req::HasMultipleTabs:   if (st.tabCount < 2) return deny(r); break;
        case Req::HasSelectionSingle: if (st.selectionCount != 1) return deny(r); break;
        case Req::StateUnavailable:  return deny(r);   // 走到这里说明 probe 刚被清空
        case Req::None:              break;
        }
    }
    if (reason) reason->clear();
    return true;
}

CommandBus& CommandBus::Get() {
    static CommandBus bus;
    return bus;
}

void CommandBus::Register(CmdId id, Handler fn) {
    // 与 RegisterWithResult 互斥检查：同一命令在两个通道都注册时，
    // Dispatch 会让带结果的胜出，这里的注册就被静默忽略。
    // 那不是崩溃，而是"我明明注册了却没跑"—— 所以当场警告。
    for (const auto& kv : resultHandlers_) {
        if (kv.first == id) {
            const CommandDef* d = FindCommand(id);
            LogDbg(L"[cmd] 冲突：%s 已用 RegisterWithResult 注册，"
                   L"这次的 Register 会被忽略", d ? d->name : L"?");
            break;
        }
    }
    for (auto& kv : handlers_) {
        if (kv.first == id) {
            kv.second = std::move(fn);
            return;
        }
    }
    handlers_.emplace_back(id, std::move(fn));
}

void CommandBus::RegisterByName(const wchar_t* name, Handler fn) {
    if (const CommandDef* d = FindCommand(name)) Register(d->id, std::move(fn));
}

void CommandBus::RegisterWithResult(CmdId id, ResultHandler fn) {
    for (auto& kv : resultHandlers_) {
        if (kv.first == id) {
            kv.second = std::move(fn);
            return;
        }
    }
    resultHandlers_.emplace_back(id, std::move(fn));
}

bool CommandBus::HasResult(CmdId id) const {
    for (const auto& kv : resultHandlers_)
        if (kv.first == id) return true;
    return false;
}

bool CommandBus::HasResult(const std::wstring& name) const {
    const CommandDef* d = FindCommand(name);
    return d && HasResult(d->id);
}

bool CommandBus::Invoke(CmdId id, const Args& args, const wchar_t* actor) {
    return Dispatch(id, args, nullptr, actor);
}

bool CommandBus::InvokeResult(CmdId id, const Args& args, json::Value* result,
                             const wchar_t* actor) {
    return Dispatch(id, args, result, actor);
}

bool CommandBus::Dispatch(CmdId id, const Args& args, json::Value* result, const wchar_t* actor) {
    const CommandDef* def = FindCommand(id);
    if (!def) return false;

    // 每次都先清空：否则上一次的拒绝原因会被下一次成功调用带出去，
    // 调用方读到"成功了但原因还是旧的"，比没有原因更误导。
    lastBlockReason_.clear();
    pendingReason_.clear();

    // 前置检查：不满足就不调 handler。以前这里没有这一层，结果是
    // "无历史时点后退"——handler 里静默 return false，界面毫无反馈。
    // 现在拒绝时给出明确原因（审计里也能看到），并把原因留给调用方：
    // 对外报"已经是顶层"远比笼统的"命令执行失败"有用。
    std::wstring reason;
    if (!CanExecute(*def, &reason)) {
        last_ = def->name;
        lastBlockReason_ = reason;
        Audit(*def, DescribeArgs(args) + L" (blocked: " + reason + L")", actor, false);
        LogDbg(L"[cmd] 前置不满足：%s（%s）", def->name, reason.c_str());
        if (result) *result = json::Value::MakeObject();
        return false;
    }

    // 先找带返回值的 handler。
    //
    // 注意：同一命令如果两边都注册过，这里带结果的那个会胜出、另一个被忽略。
    // 这是刻意的（带结果的是"更强的实现"），但如果不提醒就会变成
    // "我注册了 handler 却没跑"这种极难查的问题 —— 所以注册时就报警，
    // 而不是等到运行时表现为"命令没反应"。
    for (auto& kv : resultHandlers_) {
        if (kv.first != id) continue;
        json::Value out = json::Value::MakeObject();
        bool ok = kv.second ? kv.second(args, &out) : false;
        last_ = def->name;
        // handler 自己报的原因优先于笼统的"失败"：它知道是名字错还是值超范围。
        if (!ok && !pendingReason_.empty()) lastBlockReason_ = pendingReason_;
        Audit(*def, ArgsDescForAudit(args, ok, pendingReason_), actor, ok);
        if (result) *result = std::move(out);
        return ok;
    }

    for (auto& kv : handlers_) {
        if (kv.first != id) continue;
        bool ok = kv.second ? kv.second(args) : false;
        last_ = def->name;
        if (!ok && !pendingReason_.empty()) lastBlockReason_ = pendingReason_;
        Audit(*def, ArgsDescForAudit(args, ok, pendingReason_), actor, ok);
        // 无返回值命令：给空对象而非旧数据（不编造）。
        if (result) *result = json::Value::MakeObject();
        return ok;
    }
    Audit(*def, DescribeArgs(args), actor, false);
    LogDbg(L"[cmd] 未实现：%s（actor=%s）", def->name, actor ? actor : L"?");
    if (result) *result = json::Value::MakeObject();
    return false;
}

bool CommandBus::InvokeByNameResult(const std::wstring& name, const Args& args,
                                    json::Value* result, const wchar_t* actor) {
    const CommandDef* def = FindCommand(name);
    if (!def) {
        LogDbg(L"[cmd] 未知命令：%s（actor=%s）", name.c_str(), actor ? actor : L"?");
        if (result) *result = json::Value::MakeObject();
        return false;
    }
    return Dispatch(def->id, args, result, actor);
}

bool CommandBus::InvokeByName(const std::wstring& name, const Args& args, const wchar_t* actor) {
    const CommandDef* def = FindCommand(name);
    if (!def) {
        LogDbg(L"[cmd] 未知命令：%s（actor=%s）", name.c_str(), actor ? actor : L"?");
        return false;
    }
    return Dispatch(def->id, args, nullptr, actor);
}

bool CommandBus::InvokeLegacy(CmdId id, const std::wstring& arg, const wchar_t* actor) {
    const CommandDef* def = FindCommand(id);
    if (!def) return false;
    return Invoke(id, ArgsFromLegacyArg(*def, arg), actor);
}

bool CommandBus::InvokeByNameLegacy(const std::wstring& name, const std::wstring& arg,
                                    const wchar_t* actor) {
    // 支持 "ViewMode:3" 这种内联参数（keymap / 序列 / 鼠标规则同源）：
    // 显式传入的 arg 优先，没有才用规格里的。
    std::wstring cmdName, inlineArg;
    SplitCommandSpec(name, &cmdName, &inlineArg);
    const CommandDef* def = FindCommand(cmdName);
    if (!def) {
        LogDbg(L"[cmd] 未知命令：%s（actor=%s）", name.c_str(), actor ? actor : L"?");
        return false;
    }
    return Invoke(def->id, ArgsFromLegacyArg(*def, arg.empty() ? inlineArg : arg), actor);
}

void CommandBus::Audit(const CommandDef& def, const std::wstring& argDesc, const wchar_t* actor,
                       bool ok) {
    // 审计只记录"谁触发了什么命令"；文件不轮转，单行 <=200 字节，量级与人工操作同阶。
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[512];
    swprintf(line, 512, L"%04d-%02d-%02dT%02d:%02d:%02d %ls %ls arg=%ls ok=%d\r\n", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond, actor ? actor : L"?", def.name,
             argDesc.empty() ? L"-" : argDesc.c_str(), ok ? 1 : 0);
    const std::wstring path = tf::Join(Config::Get().Dir(), L"audit.log");

    // 轮转（每 200 条查一次、超 2 MB 改名 .1）与调试日志共用 tf::AppendLine；
    // 这里只负责编码 —— 审计用 UTF-8（脚本 grep 方便），调试日志用 UTF-16 LE。
    tf::AppendLine(path, tf::ToUtf8(line), 2ull << 20, 200);
}
