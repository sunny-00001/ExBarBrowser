// 命令的命名空间别名表（说明见 cmd_naming.h）。
//
// 命名规则：
//   · 全小写，点分，两段（域.动作）。两段够用：131 条命令按域分组后每组最多 28 条，
//     再加一段会变成 tab.close.other.left 这种读起来吃力的名字。
//   · 域取自 CommandDef::group（小写）—— 不另造分类，否则两套"分组"迟早不一致。
//   · 动作优先用现有名字去掉历史前缀后的部分（NavBack -> back），
//     而不是重新起名 —— 减少"同一个概念两个词"的混乱。
//
// 少数偏离 group 的例外在注释里说明理由。
#include "cmd_naming.h"
#include "str.h"

#include <unordered_map>

namespace cmdnaming {
namespace {

struct Alias {
    const wchar_t* legacy;
    const wchar_t* dotted;
};

// ---- 别名表 ----
// 未列出的命令走推导规则（group 小写 + 原名小写），见 Derive()。
const Alias kAliases[] = {
    // --- Tab（group=Tab）---
    {L"TabNew",          L"tab.new"},
    {L"TabClose",        L"tab.close"},
    {L"TabCloseOther",   L"tab.close.others"},
    {L"TabCloseLeft",    L"tab.close.left"},
    {L"TabCloseRight",   L"tab.close.right"},
    {L"TabClone",        L"tab.clone"},
    {L"TabReopenClosed", L"tab.reopen"},
    {L"TabMoveLeft",     L"tab.move.left"},
    {L"TabMoveRight",    L"tab.move.right"},
    {L"TabMoveFirst",    L"tab.move.first"},
    {L"TabMoveLast",     L"tab.move.last"},
    {L"TabPrevTab",      L"tab.activate.mru"},   // MRU 上一个：与位置切换区分
    {L"TabLast",         L"tab.activate.last"},
    {L"TabActivate",     L"tab.activate"},
    {L"TabNext",         L"tab.next"},
    {L"TabPrev",         L"tab.prev"},
    {L"TabMergeAll",     L"tab.merge"},
    {L"CloseLeftOne",    L"tab.close.prev"},     // 关掉左边"一个"：与 close.left 不同
    {L"CloseRightOne",   L"tab.close.next"},
    {L"GoTab1",          L"tab.goto.1"},
    {L"GoTab2",          L"tab.goto.2"},
    {L"GoTab3",          L"tab.goto.3"},
    {L"GoTab4",          L"tab.goto.4"},
    {L"GoTab5",          L"tab.goto.5"},
    {L"GoTab6",          L"tab.goto.6"},
    {L"GoTab7",          L"tab.goto.7"},
    {L"GoTab8",          L"tab.goto.8"},
    {L"GoTab9",          L"tab.goto.9"},

    // --- Tab 个性化：改名/颜色/去重 ---
    // 这三条不是"标签开关"，但仍属标签域，别名表里写明以免走推导规则
    // （推导出的是 tab.rename —— 恰好一致，但仍显式登记：名字是契约，不靠巧合）。
    {L"TabRename",          L"tab.rename"},
    {L"TabSetColor",        L"tab.color"},
    {L"TabCloseDuplicates", L"tab.close.duplicates"},
    {L"TabLock",           L"tab.lock"},
    {L"TabCopyTitle",      L"tab.copy.title"},
    // 关闭历史 / 固定 / 切换器：同一域的新命令，显式登记 legacy 名，
    // 让 keymap 与旧脚本能用 CamelCase 引用（FindCommand 两个方向都认）。
    {L"TabClosedList",     L"tab.closed.list"},
    {L"TabReopenAt",       L"tab.reopen.index"},
    {L"TabPin",            L"tab.pin"},
    {L"TabSwitcher",       L"tab.switcher"},
    {L"TabCycleNext",      L"tab.cycle.next"},
    {L"TabCyclePrev",      L"tab.cycle.prev"},
    {L"TabMoveNewWindow",  L"tab.move.window"},
    {L"NamedSessionSave",    L"session.named.save"},
    {L"NamedSessionList",    L"session.named.list"},
    {L"NamedSessionRestore", L"session.named.restore"},
    {L"NamedSessionDelete",  L"session.named.delete"},

    // --- Go（group=Go）：跳转到已知位置 ---
    {L"GoHome",          L"go.home"},
    {L"GoDesktop",       L"go.desktop"},
    {L"GoDownloads",     L"go.downloads"},
    {L"GoDocuments",     L"go.documents"},
    {L"GoConfigPath",    L"go.config"},
    {L"GoLocalAppData",  L"go.localappdata"},
    {L"GoControlPanel",  L"go.controlpanel"},
    {L"GoStartFolder",   L"go.startfolder"},
    {L"GoThisPC",        L"go.thispc"},
    {L"GoPowerShell",    L"go.powershell"},

    // --- Nav（group=Nav）---
    {L"NavBack",         L"nav.back"},
    {L"NavForward",      L"nav.forward"},
    {L"NavUp",           L"nav.up"},
    {L"NavRefresh",      L"nav.refresh"},
    {L"NavHome",         L"nav.home"},
    {L"NavViewMenu",     L"nav.viewmenu"},

    // --- Path（group=Path）：写剪贴板 ---
    {L"PathCopy",        L"path.copy"},
    {L"PathCopyName",    L"path.copy.name"},
    {L"PathCopyDir",     L"path.copy.dir"},
    {L"PathCopyUnc",     L"path.copy.unc"},
    {L"PathCopyExt",     L"path.copy.ext"},
    {L"PathCopyTab",     L"path.copy.tab"},
    {L"PathCopyLink",    L"path.copy.link"},

    // --- File（group=File）：交互式沿用 file.*，确定式用 fs.* ---
    // 为什么确定式单开 fs.*：它们的语义是"对指定路径做文件系统操作"，
    // 与"对当前选中做点什么"的交互式命令不是同一件事。名字上就该分开。
    {L"CmdNewTextFile",    L"file.new.text"},
    {L"CmdOpenWith",       L"file.open.with"},
    {L"CmdNewFolder",      L"file.new.folder"},
    {L"CmdCopy",           L"file.copy"},
    {L"CmdCut",            L"file.cut"},
    {L"CmdPaste",          L"file.paste"},
    {L"CmdDelete",         L"file.delete"},
    {L"CmdDeletePermanent", L"file.delete.permanent"},
    {L"CmdRename",         L"file.rename"},
    {L"CmdProps",          L"file.properties"},
    {L"CmdUndo",           L"file.undo"},
    {L"CmdSelectAll",      L"file.select.all"},
    {L"CmdOpen",           L"file.open"},
    {L"CmdOpenInNewTab",   L"file.open.tab"},
    {L"CmdOpenInNewWindow", L"file.open.window"},
    {L"CmdRefresh",        L"file.refresh"},
    {L"SelectInvert",      L"file.select.invert"},
    {L"SelectFiles",       L"file.select.files"},
    {L"SelectFolders",     L"file.select.folders"},
    {L"SelectByType",      L"file.select.type"},
    {L"SelectByPattern",   L"file.select.pattern"},
    {L"CopyToFolder",      L"file.copy.to"},
    {L"MoveToFolder",      L"file.move.to"},
    {L"BatchRename",       L"file.rename.batch"},
    {L"FileFocusTop",      L"file.focus.top"},
    {L"FileFocusBottom",   L"file.focus.bottom"},
    // 确定式（第 4 步）：目标由参数点名
    {L"FsRename",          L"fs.rename"},
    {L"FsDelete",          L"fs.delete"},
    {L"FsCopyTo",          L"fs.copy"},
    {L"FsMoveTo",          L"fs.move"},
    {L"FsOpen",            L"fs.open"},
    {L"FsOpenInNewTab",    L"fs.open.tab"},
    {L"FsOpenInNewWindow", L"fs.open.window"},
    {L"FsCopy",            L"fs.copy.toclip"},
    {L"FsCut",             L"fs.cut.toclip"},
    {L"FsProperties",      L"fs.properties"},

    // --- View（group=View）---
    {L"ViewMode",          L"view.mode"},
    {L"ViewSort",          L"view.sort"},
    {L"ViewGroup",         L"view.group"},
    {L"ViewSortByKey",     L"view.sort.bykey"},
    {L"ViewGroupByKey",    L"view.group.bykey"},
    {L"ViewPropsList",     L"view.props.list"},
    {L"ViewNext",          L"view.next"},
    {L"ViewPrev",          L"view.prev"},
    {L"ToggleHidden",      L"view.toggle.hidden"},
    {L"ToggleSystemFiles", L"view.toggle.system"},
    {L"ToggleExtensions",  L"view.toggle.extensions"},
    {L"TogglePreviewPane", L"view.toggle.preview"},
    {L"ToggleNavPane",     L"view.toggle.nav"},
    {L"NavTreeExpandAll",  L"nav.tree.expand.all"},
    {L"ViewColumnsList",   L"view.columns.list"},
    {L"ViewColumnsSet",    L"view.columns.set"},
    {L"ViewColumnsChoose", L"view.columns.choose"},
    {L"ViewZoom",          L"view.zoom"},
    {L"ViewZoomSet",       L"view.zoom.set"},
    {L"ViewZoomChoose",    L"view.zoom.choose"},
    {L"ToggleCheckboxes",  L"view.toggle.checkboxes"},
    {L"CycleHiddenSeg",    L"view.cycle.hidden"},

    // --- Window（group=Window）---
    {L"NewWindow",         L"win.new"},
    {L"QuitExplorer",      L"win.quit"},
    {L"FolderOptions",     L"win.folderoptions"},
    {L"MapDrive",          L"win.mapdrive"},
    {L"UnmapDrive",        L"win.unmapdrive"},
    {L"CommandPalette",    L"win.palette"},
    {L"HotkeyManager",     L"win.hotkeys"},
    {L"FocusAddress",      L"win.focus.address"},
    {L"FocusSearch",       L"win.focus.search"},
    {L"Preferences",       L"win.preferences"},

    // --- External（group=External）---
    {L"OpenTerminal",      L"ext.terminal"},
    {L"OpenTerminalAdmin", L"ext.terminal.admin"},
    {L"RunCommand",        L"ext.run"},
    {L"SendKeys",          L"ext.sendkeys"},

    // --- Bookmark（group=Bookmark）---
    {L"BmkAddCurrent",     L"bmk.add.current"},   // 加**当前目录**（无参）
    {L"BmkAdd",            L"bmk.add.path"},     // 加**指定路径**（带参）
    {L"BmkShowBar",        L"bmk.show"},
    {L"BmkHideBar",        L"bmk.hide"},
    {L"BmkToggleBar",      L"bmk.toggle"},
    // 书签编辑族：右键菜单动作补成命令（R20 反向要求：人工可达就必须有结构化入口）。
    {L"BmkEdit",           L"bmk.edit"},
    {L"BmkRemove",         L"bmk.remove"},
    {L"BmkNewFolder",      L"bmk.newfolder"},
    {L"BmkUndo",           L"bmk.undo"},
    {L"BmkClearAll",       L"bmk.clear"},
    {L"BmkSort",           L"bmk.sort"},
    {L"BmkDedupe",         L"bmk.dedupe"},
    {L"BmkMove",           L"bmk.move"},
    {L"BmkList",           L"bmk.list"},
    {L"BmkFocus",          L"bmk.focus"},
    // 全量覆盖 Batch 2：书签文件 IO 与带参外部/剪贴板入口。
    {L"BmkImport",         L"bmk.import"},
    {L"BmkExport",         L"bmk.export"},
    {L"BmkReload",         L"bmk.reload"},
    {L"ClipboardCopy",     L"clipboard.copy"},
    {L"ExtTerminalAt",     L"ext.terminal.at"},

    // --- Session（group=Session）---
    {L"SessionSave",       L"session.save"},
    {L"SessionRestore",    L"session.restore"},
    {L"SessionDelete",     L"session.delete"},
    {L"SessionList",       L"session.list"},

    // --- Tool（group=Tool）---
    {L"DupFinder",         L"tool.dupfinder"},
    {L"FileHash",          L"tool.hash"},
    {L"FolderCompare",     L"tool.compare"},
    {L"OpListDir",         L"tool.list"},
    {L"OpStat",            L"tool.stat"},
    {L"OpTopN",            L"tool.topn"},
    {L"OpDiskUsage",       L"tool.diskusage"},
    // 作业域：长任务命令（tool.*）的异步侧。三段点分名推导规则给不出来
    // （推导出来会是 tool.jobget），所以显式登记 —— 名字是契约，不靠巧合。
    {L"ToolJobGet",        L"tool.job.get"},
    {L"ToolJobCancel",     L"tool.job.cancel"},
    {L"ToolJobList",       L"tool.job.list"},
    // AI 面板是窗口级 UI（group=Window），不是"工具"——它开/关的是窗口里的一块区域。
    {L"AiTogglePanel",     L"win.ai.toggle"},
    {L"AiStop",            L"win.ai.stop"},

    // --- Search（group=Search）---
    {L"SearchEverything",  L"search.everything"},
    {L"OpSearch",          L"search.find"},
};

const std::unordered_map<std::wstring, std::wstring>& LegacyToDotted() {
    static const std::unordered_map<std::wstring, std::wstring> m = [] {
        std::unordered_map<std::wstring, std::wstring> out;
        for (const auto& a : kAliases) out.emplace(a.legacy, a.dotted);
        return out;
    }();
    return m;
}

const std::unordered_map<std::wstring, std::wstring>& DottedToLegacy() {
    static const std::unordered_map<std::wstring, std::wstring> m = [] {
        std::unordered_map<std::wstring, std::wstring> out;
        for (const auto& a : kAliases) out.emplace(a.dotted, a.legacy);
        return out;
    }();
    return m;
}

}  // namespace

// 注：本文件曾有一个局部 Lower()（把自己转小写再查表）。两个查表方向都改成
// _wcsicmp 遍历比对后它不再需要；同类工具已统一在 str::Lower（str.h）。

std::wstring AliasOf(const std::wstring& legacyName) {
    const auto& m = LegacyToDotted();
    // 查大小写不敏感：与 FindCommand 的 _wcsicmp 口径一致。
    // 以前这里用 find（区分大小写），而 LegacyOf 用 Lower —— 两个方向不对称，
    // 表现为"nav.back 认、NavBack 认、navback 不认"这种半吊子行为。
    for (const auto& kv : m)
        if (str::IEquals(kv.first, legacyName)) return kv.second;
    return std::wstring();
}

std::wstring LegacyOf(const std::wstring& dottedName) {
    const auto& m = DottedToLegacy();
    // 同样不区分大小写（原先靠 Lower 归一，遇到非 ASCII 或奇怪大小写仍会漏）
    for (const auto& kv : m)
        if (str::IEquals(kv.first, dottedName)) return kv.second;
    return std::wstring();
}

bool LooksDotted(const std::wstring& name) { return name.find(L'.') != std::wstring::npos; }

std::wstring CanonicalName(const std::wstring& anyName) {
    // 点分名原样返回（它已经是规范形式）；旧名转成点分名。
    if (LooksDotted(anyName)) return anyName;
    const std::wstring a = AliasOf(anyName);
    return a.empty() ? anyName : a;
}

}  // namespace cmdnaming
