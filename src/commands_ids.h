// 命令身份（CmdId）：命令的稳定编号。
//
// 【为什么单独成文件】命令的**身份**与命令的**契约**是两件事：
//   · 身份：这是第几号命令（CmdId）—— 决定 id 稳定性、追加规则、legacy 保留义务。
//   · 契约：这条命令要什么参数、有什么效果（CommandDef / Effects / Args / Actor）。
// 把两者放一个头里，改一条命令的契约会在两百行编号里翻；
// 而**读编号表的人**（命令表生成、检查器、AI 通道）根本不需要契约那一大堆。
//
// 【顺序纪律，只加不删】enum class 无显式赋值 —— 在中间插一项会让后面所有 id 整体位移，
// 而 id 已经写进了 tests/baselines/commands-baseline.json、keymap.json 与用户的脚本。
// 所以：**只能在末尾追加**（见末尾注释与 C1b 规则）。删掉的命令要留占位。
#pragma once

enum class CmdId : int {
    None = 0,
    // Tab
    TabNew, TabClose, TabCloseOther, TabCloseLeft, TabCloseRight, TabClone,
    TabReopenClosed, TabMoveLeft, TabMoveRight, TabMoveFirst, TabMoveLast,
    TabPrevTab, TabLast, CloseLeftOne, CloseRightOne,
    TabActivate,  // arg = 0-based index
    GoTab1, GoTab2, GoTab3, GoTab4, GoTab5, GoTab6, GoTab7, GoTab8, GoTab9,
    TabNext, TabPrev, TabMergeAll,
    // Go
    GoHome, GoDesktop, GoDownloads, GoDocuments, GoConfigPath, GoLocalAppData,
    GoControlPanel, GoStartFolder, GoThisPC, GoPowerShell,
    // Nav
    NavBack, NavForward, NavUp, NavRefresh, NavHome, NavViewMenu,
    // Path
    PathCopy, PathCopyName, PathCopyDir, PathCopyUnc, PathCopyExt, PathCopyTab, PathCopyLink,
    // File
    CmdNewTextFile, CmdOpenWith, CmdNewFolder, CmdCopy, CmdCut, CmdPaste, CmdDelete,
    CmdRename, CmdProps, CmdUndo, CmdSelectAll, CmdOpen, CmdOpenInNewTab,
    CmdOpenInNewWindow, SelectInvert, SelectFiles, SelectFolders, SelectByType,
    CopyToFolder, MoveToFolder, BatchRename, SelectByPattern,
    FileFocusTop, FileFocusBottom, CmdRefresh,
    // ---- 确定式文件操作（P5：交互与确定分离）----
    // 上面那批 Cmd* 是"交互式"：要么弹输入框/对话框，要么作用于"用户碰巧选中"的项。
    // 下面这批是同一个业务的**确定式**版本：目标由参数点名，绝不弹框、绝不读选中。
    // 两者共用 shell_ops 里的同一份实现（唯一实现原则），只是入口不同。
    FsRename,    // {target, newName}  —— 对应 CmdRename 的确定式版本
    FsDelete,    // {targets[]}        —— 对应 CmdDelete
    FsCopyTo,    // {targets[], to}    —— 对应 CopyToFolder
    FsMoveTo,    // {targets[], to}    —— 对应 MoveToFolder
    FsOpen,      // {path}             —— 对应 CmdOpen
    // FsOpen 的语义与 CmdOpen 完全一致：文件夹在当前标签内进入，文件交给 shell。
    // 不设 mode 参数是刻意的 —— VS Code 把"打开"拆成 open / openWith / revealInExplorer
    // 三条独立命令，而不是一条带 mode 的命令。带 mode 会让 schema 变成枚举，
    // 调用方要先查"有哪些 mode"，而它们其实是三种不同的意图，值得各占一个名字。
    FsOpenInNewTab,     // {path}         —— 对应 CmdOpenInNewTab
    FsOpenInNewWindow,  // {path}         —— 对应 CmdOpenInNewWindow
    FsCopy,             // {targets[]}    —— 对应 CmdCopy（写剪贴板，不产生副本）
    FsCut,              // {targets[]}    —— 对应 CmdCut（写剪贴板 + 标记移动）
    FsProperties,       // {target}       —— 对应 CmdProps
    // View
    ViewMode,  // arg = 0..7（超大/大/中/小/列表/详细信息/平铺/内容）
    ViewSort,  // arg = 0..3（名称/日期/类型/大小）
    ViewGroup, // arg = 0..3（无/类型/日期/大小）
    ToggleHidden, ToggleSystemFiles, ToggleExtensions, CycleHiddenSeg,
    TogglePreviewPane, ToggleNavPane, ToggleCheckboxes, ViewNext, ViewPrev,
    // Window
    // 注：QuitExplorer 是 mod 期的名字（那时行为是"重启 explorer.exe"）。本工程里
    // 它的行为是"退出本程序"，点分名 win.quit 才是准确描述。枚举名保留是因为
    // 规则 C1（CmdId 值一经分配不再改，废弃则保留占位）—— 改枚举名会打断
    // keymap.json / 审计日志 / 脚本里的引用。表里的 name 已改为 win.quit。
    NewWindow, QuitExplorer, FolderOptions, MapDrive, UnmapDrive,
    CommandPalette, HotkeyManager, FocusAddress, FocusSearch, Preferences,
    // External
    OpenTerminal, OpenTerminalAdmin, RunCommand, SendKeys,
    // Bookmark
    BmkAddCurrent, BmkShowBar, BmkHideBar, BmkToggleBar,
    // Session
    SessionSave, SessionRestore, SessionDelete, SessionList,
    // 目录占位（部分已实现）。以下 Op* 六项已废弃：
    // 它们从未有 handler，且是 fs.open / tool.list 的语义重复。命令表条目已删除
    // （见 commands.cpp），但**枚举项必须保留** —— C1b：enum class 无显式赋值，
    // 删中间项会让后续所有 id 位移；legacy 名也必须留（C2：用户 keymap 只增不减）。
    DupFinder, FileHash, FolderCompare, SearchEverything, OpListDir, OpStat,
    OpTopN, OpDiskUsage, OpSearch,
    // 废弃，勿用（仅占位维持 id 顺序）：
    OpReadText, OpNavigate, OpReveal, OpSelect, OpOpen, OpListCurrentView,
    AiTogglePanel, AiStop,
    // ---- 标签个性化（只允许追加在末尾，见 C1 与 commands-baseline.json）----
    // 这三条原本只是右键菜单项；登记成命令是为了满足 R20（人机等价）：
    // 右键弹出来的每个动作都必须有键盘/面板可达路径，否则只有鼠标能做。
    TabRename,         // {name}     改名（空 = 取消改名）
    TabSetColor,       // {color, index?}  color 0..7（0 = 无颜色），index 省略=活动标签
    TabCloseDuplicates,// 关掉当前标签同路径的其他标签
    TabLock,           // {locked}   锁定/解锁（锁定后关标签族会跳过它）
    TabCopyTitle,      // 复制当前显示名（含改名后的别名）到剪贴板

    // ---- 书签编辑族（只有右键菜单能做 → 补成命令，满足 R20 反向要求）----
    // 这几条原先是书签栏右键菜单里的动作，但是**没有对应命令**：
    //   · 检查器把它们报成"人工可达但无结构化入口"；
    //   · 也就意味着只有鼠标能做 —— 键盘用户、命令面板、AI 都没有等价路径。
    // 语义与菜单项一一对应，参数用书签 id（不是下标：重排后下标会变）。
    BmkEdit,           // {id, name, path}  改名称/路径（path 空 = 变成纯分类）
    BmkRemove,         // {id}              删除一条（可撤销）
    BmkNewFolder,      // {name, parentId?} 新建分类
    BmkUndo,           // 撤销上一次删除/移动/排序
    BmkClearAll,       // 清除全部（可撤销）
    BmkSort,           // {recursive?}   按名称排序
    BmkDedupe,         // 清理重复路径书签
    BmkAdd,            // {path, name?, parentId?} 添加一条（拖放/脚本/AI 的共同入口）
    BmkMove,           // {id, toId} 或 {id, toIndex}  移动（拖拽重排的结构化版本）
    BmkList,           // {parentId?}  返回书签树（**读**：其余 bmk.* 要的 id 从这里来）
    // ---- 设置域（操作软件自身：可枚举 / 可读 / 可写 / 可回读）----
    // 这三条是「AI 调试某个设置项」那条路的入口。没有它们，设置只能是
    // 编译期 struct，机器侧既不知道有哪些项，也没有稳定名字去定位。
    ConfigList,        // 列出全部设置项（key/类型/范围/当前值）
    ConfigGet,         // {key}        读一项
    ConfigSet,         // {key, value} 写一项（带类型与范围校验）
    BmkFocus,          // 焦点进入书签栏（->/<- 选，Enter 开，Shift+F10 菜单，Esc 退回视图）
    // ---- 开发面（操作代码本身：快照/编译/测试/回滚）----
    // 与配置域同一范式：可枚举、可调用、可回读。build/test 异步跑完发事件，
    // 不占着 UI 线程等；rollback 是破坏性写，必须 approve。
    DevSnapshot, DevBuild, DevTest, DevJob, DevRollback,
    // 新增命令一律加在**枚举末尾**（check_commands.py 的 C1b 规则）：
    // enum class 无显式赋值，往中间插会让后面所有 id 整体位移，
    // 基线（tests/baselines/commands-baseline.json）与既有持久化引用全部失配。
    BmkAddSelection,   // 无参，needsSelection：收藏文件视图当前选中的文件夹（可多选）
    BmkAddTabs,        // 无参：收藏当前所有打开标签页的目录（已收藏/空路径跳过），返回 added/skipped/ids
    BmkCopy,           // {id}：复制书签条目（分类连子树深拷贝），插到原条目后，返回新 id
    // ---- 标签关闭历史（P0：关闭栈已有 20 条，但过去只能"重开最近一个"）----
    TabClosedList,     // 无参，只读：列出关闭历史（0 = 最近关闭）
    TabReopenAt,       // {index}：重开第 index 条关闭记录（先建后弹栈）
    TabPin,            // {index?, pinned?}：固定/取消固定（省略 pinned = 切换）
    // ---- 导航栏历史（能力缺口补齐）----
    // 下拉历史、"跳到第 N 条"、"清除历史" 过去只有鼠标一条路：快捷键绑不上、
    // 命令面板里也没有，AI 更是连"后退会退到哪"都看不见。
    NavHistoryList,    // {backward?}        只读：列出可后退/可前进的条目（最近的在前）
    NavHistoryJump,    // {depth, backward?} 跳到第 depth 条（0 = 最近一条）
    NavHistoryClear,   // 无参，破坏性：清空当前标签历史 + 全局""最近访问"
    NavRecentList,     // 无参，只读：列出地址栏下拉里的"最近访问"
    TabSwitcher,       // 无参：打开标签切换器（可键入前缀定位）。UI 模态，不暴露给 AI
    TabCycleNext,      // 无参：按 MRU 循环到下一个（Ctrl+Tab）
    TabCyclePrev,      // 无参：按 MRU 循环到上一个（Ctrl+Shift+Tab）
    TabMoveNewWindow,  // {index?}：在新窗口打开该标签路径并关闭源标签（不迁移历史）
    NamedSessionSave,    // {name}：把当前标签组存成命名会话（同名覆盖）
    NamedSessionList,    // 无参，只读：列出全部命名会话
    NamedSessionRestore, // {name}：整组重开一个命名会话（追加开新标签）
    NamedSessionDelete,  // {name}：删除一个命名会话

    // ---- Explorer 键位补齐（只允许追加在末尾，见 C1b）----
    CmdDeletePermanent,  // 无参，needsSelection：Shift+Delete，永久删除（不经回收站、不进撤销栈）
    NavTreeExpandAll,    // 无参：Ctrl+Shift+E，展开导航窗格整棵树
    // ---- 长任务作业域（只允许追加在末尾，见 C1b）----
    ToolJobGet,     // {id}：查作业状态与结果（完成后带 result）
    ToolJobCancel,  // {id}：请求取消一个还在跑的作业
    ToolJobList,    // 无参：列出全部作业（新的在前，只给摘要）

    // ---- 详细信息列（P3：能力缺口）----
    ViewColumnsList,     // 无参，只读：列出详细信息视图可用列及可见性
    ViewColumnsSet,      // {visible[]}：设置可见详情列（列 id 来自 view.columns.list）
    ViewColumnsChoose,   // 无参，UI：打开“选择详细信息”菜单
    ViewZoom,            // {delta}：Ctrl+滚轮步进缩放图标（delta = ±1）
    ViewZoomSet,         // {iconPx}：设置精确图标像素（16..256）
    ViewZoomChoose,      // 无参，UI：打开图标大小档位菜单
    // ---- P0-9：改写可见性（只读观测，不落盘）----
    DevDiff,             // 无参，只读：返回工作树的结构化 diff（文件+SHA-256+mtime+patch），供审阅"agent 改了什么"
    // ---- P3-4：任意属性排序/分组（只允许追加在末尾，见 C1b）----
    ViewSortByKey,       // {key[,ascending]}：按任意 PROPERTYKEY 排序
    ViewGroupByKey,      // {key}：按任意 PROPERTYKEY 分组
    ViewPropsList,       // 无参，只读：列出可排序/分组的常用属性键（id 可直接给 bykey）

    // ---- 全量覆盖 Batch 2：补齐菜单里有、命令表里没有的带参入口 ----
    // 前三条是书签文件 IO 的确定式后端：AI 给完整路径，人走文件对话框；
    // 后者只是前者的 UI 适配器，两条路共用同一份 Bmk*HtmlFrom/To 实现。
    BmkImport,         // {path, mode}  mode=0 追加 / 1 覆盖（破坏性、可撤销）
    BmkExport,         // {path}        导出到指定文件
    BmkReload,         // 无参：从 bookmarks.html 重新载入并重建书签栏
    // 任意文本写剪贴板与"在指定目录开终端"：面包屑/书签右键此前直接调底层函数。
    ClipboardCopy,     // {text}  写剪贴板（与 fs.copy.toclip 同源，但内容是文本而非路径集）
    ExtTerminalAt,     // {path}  在指定目录启动终端（ext.terminal 的带参版）
    // ---- 第 2 档 序 7：接受器 + archive + funnel（只允许追加在末尾，见 C1b）----
    DevArchiveDecide,  // {trials[]}：纯计算，配对比较 + e-process 判定（只读）
    DevArchiveRecord,  // {trajectory, repeats[]?, baselineId?, ...}：落盘候选 + 判定
    DevArchiveList,    // {limit?}：只读列出 archive 候选
    // ---- 窗口管理命令体系 ----
    // 单窗口：作用于本进程主窗口（App::hwnd()）。都是用户/命令触发的产品行为，
    // 走 CommandBus；R21 禁的是脚本自动化抢焦点，不是产品代码里的窗口 API。
    WinMinimize,       // 无参：最小化主窗口（ShowWindow SW_MINIMIZE）
    WinMaximize,       // 无参：最大化主窗口（SW_MAXIMIZE，幂等）
    WinRestore,        // 无参：还原主窗口（SW_RESTORE）
    WinTopmost,        // {on}：置顶开关（SetWindowPos HWND_TOPMOST/NOTOPMOST，NOACTIVATE）
    WinActivate,       // 无参：把主窗口切到前台（抢焦点 → 不暴露给 AI，仅人工可达）
    WinCenter,         // 无参：居中到当前显示器工作区
    WinMove,           // {x, y}：把主窗口左上角移到屏幕坐标
    WinResize,         // {width, height}：调整主窗口客户区外框尺寸
    // 多窗口：枚举所有 ExBarBrowser_Main 顶层窗口后布局。跨进程只 SetWindowPos，
    // 不搬 ShellView（进程内 COM），也不发明新的 IPC 协议。
    WinList,           // 无参，只读：列出所有本程序窗口的 hwnd/pid/标题/矩形/最大化状态
    WinTile,           // {orientation}：把本程序窗口并排（horizontal=横排 / vertical=纵排）
    WinCascade,        // 无参：把本程序窗口级联错位层叠
    // ---- 双栏（只允许追加在末尾，见 C1b）----
    ViewToggleDualPane, // 无参：开合双栏（Ctrl+F6）
    ViewNextPane,       // 无参：切换活动窗格（F6）
    ViewPaneSplitSet,   // {percent}：设置左窗格宽度百分比（20..80）
    // ---- 搜索域（只允许追加在末尾，见 C1b）----
    // 搜索框左侧的「搜索范围」菜单与右侧的「搜索历史」下拉过去只有鼠标一条路：
    // AI 看不见当前用的是哪一档范围，也读不出这台机器上搜过什么。
    SearchScopeList,    // 无参，只读：列出范围档位与当前值
    SearchScopeSet,     // {scope, path?}：设置范围（path 只对 custom 有意义）
    SearchHistoryList,  // 无参，只读：列出搜索历史（最近的在前）
    SearchHistoryClear, // 无参，破坏性：清空搜索历史
    SearchQuerySet,     // {query}：把搜索词填进搜索框（**不执行搜索**）
    // ---- 垂直双栏（只允许追加在末尾，见 C1b）----
    ViewTogglePaneVertical,  // 无参：双栏方向切换（左右并排 <-> 上下堆叠）
    // ---- AI 面板（只允许追加在末尾，见 C1b）----
    AiPanelToggle,     // 无参：开合右侧 AI 面板
    AiPanelWeb,        // 无参：用系统浏览器打开面板服务（WebView2 不可用时的退路）
    // ---- 书签层级重组（只允许追加在末尾，见 C1b）----
    BmkDissolve,       // {id}：解散分类（子项提到上一层，分类本身消失）
    // ---- 运行期命令自省（只允许追加在末尾，见 C1b）----
    CmdHelp,           // 无参，只读：运行期列出全部命令的用法（schema/risk/必填参数/断言）
    // ---- 书签右键「打开」族（只允许追加在末尾，见 C1b）----
    FsOpenInPane,      // {path}：在另一窗格打开（单栏时先自动开双栏）
    // ---- 文件操作重做（只允许追加在末尾，见 C1b）----
    CmdRedo,           // 无参：重做一步被撤销的文件操作（与 file.undo 对称）
    // ---- 属性写入（只允许追加在末尾，见 C1b）----
    FileAttrSet,       // {attrs}：改当前选中项的属性位（未传的位保持原值）
    FsAttrSet,         // {targets, attrs}：改指定路径的属性位
    // ---- 撤销历史浏览（只允许追加在末尾，见 C1b）----
    FileUndoList,      // 无参，只读：列出撤销/重做栈里每一步的标签
    FileUndoTo,        // {index}：一次撤到指定那一步（0 = 栈顶）
    // ---- 配置导出/还原（只允许追加在末尾，见 C1b）----
    ConfigExport,      // {path?}：导出全部可写设置（省略 path 只回显，不落盘）
    ConfigImport,      // {path}：从导出文件还原设置（逐项报告成败）
    // ---- 文件列表区上下文菜单（只允许追加在末尾，见 C1b）----
    FileContextMenu,   // 无参：弹 shell 的上下文菜单（Shift+F10），纯人机 UI
    // ---- F6 焦点循环（只允许追加在末尾，见 C1b）----
    FocusNextElement,  // 无参：焦点移到下一个界面元素（F6）
    FocusPrevElement,  // 无参：焦点移到上一个界面元素（Shift+F6）
    // ---- 标签绝对位置重排（只允许追加在末尾，见 C1b）----
    // 拖拽重排的结构化入口：tab.move.* 只有左移/右移/最前/最后，
    // "把标签移到第 N 位"此前没有任何命令能表达 —— AI 无法复现拖拽。
    TabMoveTo,         // {index?, to}：把标签移到指定位置（index 省略=当前标签）
    // ---- 键位改绑（只允许追加在末尾，见 C1b）----
    // 热键管理器窗口的机器等价：改绑/解绑/全部还原。此前改键是人独占操作
    // （config.set 的 hotkeys 项只读），AI/脚本无法表达"把这个命令绑到 X"。
    HotkeySet,         // {command, key}：把命令绑到指定组合键（覆盖默认）
    HotkeyClear,       // {command}：显式解绑（回落到默认绑定）
    HotkeyReset,       // 无参：清掉全部覆盖，回到 keymap.json 的默认表
    // ---- 双栏协同（P0-2 / P1，2026-11-26；只允许追加在末尾，见 C1b）----
    // 对侧即目标（TC F5/F6 范式）：无参命令，源=活动窗格选中项，目标=另一窗格
    // 当前目录。与 fs.copy/fs.move（显式点名 to）共用 TransferTargets 的同一份
    // 底层（shellops::TransferToFolder + fileundo），只是目标来源不同。
    //
    // 【枚举名与命令名不一致：有意保留】枚举项叫 FsCopyToPane，命令名却是
    // file.copy.pane —— 因为这两条**作用于当前选中**（无参），属于 file.* 组
    // 的语义；而 fs.* 组的约定是"目标由参数点名、绝不读选中"（check_ai_meta
    // 第 9 项强制）。曾用 fs.copy.pane 时被那条判据判失败，改名才是正解。
    // 枚举名不改：CmdId 的**值**一经分配不可变，重命名枚举项会让
    // tests/baselines/commands-baseline.json 的按名比对失效 —— 值没变，
    // 名字变更是白付成本。
    FsCopyToPane,      // 无参：复制活动窗格选中项到另一窗格当前目录
    FsMoveToPane,      // 无参：移动活动窗格选中项到另一窗格当前目录（destructive，人工确认门）
    ViewPaneSwap,      // 无参：两窗格目录互换（TC Ctrl+U / cm_Exchange）
    ViewPaneMirror,    // 无参：另一窗格导航到活动窗格当前目录（TC cm_MatchSrc 目录版）
    // ---- 弹层入口命令化（2026-11-26 §2b B1/A5；只允许追加在末尾，见 C1b）----
    // "打开一个弹层"也是动作，也要有机器等价：撤销历史弹层（chip 点击人独占）
    // 与 AI peek（A5 分支级盲区）。弹层内的选中/派发本来就走总线，
    // 缺的只是"把它打开"这一步的入口。
    WinUndoHistory,    // 无参：打开撤销历史弹层（点行 = file.undo.to / file.redo）
    WinAiPeek,         // 无参：翻转 AI peek 面板（状态栏 AI chip 同义）
};
