---
status: current
baseline: 2026-11-26 11:00
---

# 能力对照：ExBar mod vs ExBarBrowser

> 回答一个问题：**独立进程路线现在能替代 explorer 到什么程度，缺口在哪，哪些缺口根本不该补。**
>
> 数据来源：`src/commands_table.cpp` + `src/commands_table_tools.cpp` 的命令目录
> （逐条对照 `implemented` 字段，不按 README 宣称），
> mod 侧对照其 127 条命令目录（`data/capabilities.json`）。本工程在其上补了确定式操作、
> 菜单动作、长任务/开发面等扩展命令；总数以 `tools/check_commands.py` 实测为准
> （2026-11-26：**241 条**）。
> 数字不写死在这里 —— 加一条命令就变，见 §2 口径说明。

---

## 1. 图例

| 标记 | 含义 |
|---|---|
| **已实现** | 本工程有命令，且能由用户直接操作（按钮 / 快捷键 / 序列 / 鼠标 / 面板） |
| **部分** | 能力存在但受限（参数不全 / 只覆盖文件系统 / 依赖系统版本） |
| **未实现** | 命令在目录里但 `implemented=false`；管理器显示"未实现" |
| **不适用** | 独立进程模型下没有意义，或与本工程设计前提冲突 |

---

## 2. 总览（2026-11-26 脚本对账）

| 组 | 条数 | 已实现 | 未实现 |
|---|---:|---:|---|
| Tab | 41 | 40 | 1（`tab.merge`） |
| Go | 10 | 10 | 0 |
| Nav | 10 | 10 | 0 |
| Path | 8 | 8 | 0 |
| File | 45 | 45 | 0 |
| View | 28 | 27 | 1（`view.toggle.checkboxes`） |
| Ai | 2 | 2 | 0 |
| Window | 30 | 28 | 2（`win.ai.toggle` / `win.ai.stop`） |
| External | 5 | 3 | 2（`ext.run` / `ext.sendkeys`） |
| Bookmark | 22 | 22 | 0 |
| Config | 5 | 5 | 0 |
| Dev | 10 | 10 | 0 |
| Session | 8 | 8 | 0 |
| Tool | 10 | 10 | 0 |
| Search | 7 | 6 | 1（`search.everything`） |
| **合计** | **241** | **234** | **7** |

> 口径：脚本按 `src/commands_table.cpp` + `src/commands_table_tools.cpp` 里
> `{CmdId::X, …}` 字面形式逐条统计分组与 `implemented`，
> 不按 README 宣称。命令行：见 `tools/check_commands.py`（同时校验 keymap × 处理器三方一致）。
>
> `search.find` 于 2026-11-21 从「已实现」降级为 `implemented=false`（宿主
> `IExplorerBrowser` 没有搜索带，`search-ms:` 被 `ShellView::DoNavigate` 明确拒绝）；
> **2026-11-22 已修好并恢复为已实现**：改走**原生搜索文件夹**
> （`ISearchFolderItemFactory` 造 PIDL → 以 PIDL 形式导航），搜索真的能执行，
> 结果视图默认详细信息。详见 `docs/EXPLORER-GAP-AUDIT.md` §2.1 F1。
>
> Search 组现在 7 条：只剩 `search.everything` 一条未实现（要接 Everything 的 IPC）；
> 另 6 条里 5 条（`search.scope.*` / `search.history.*` / `search.query.set`）是
> 2026-11-22 搜索框分裂改造带来的**搜索框配置面**（不执行搜索），`search.find`
> 才是执行搜索的那条，详见 §3.12.1。
>
> **「此电脑」这一档是已知的能力边界，不是假绿**：原生搜索文件夹的作用域只能是
> 一个普通文件系统目录（整机范围要 Explorer 进程内的 `search-ms` 宿主），
> 所以 `search.scope.set {scope:"thispc"}` **直接拒绝**，范围菜单里那一行摆成灰字
> 「（本宿主暂不可用）」。详见 EXPLORER-GAP-AUDIT §2.1 F1 的遗留小节。
>
> 总数 241 里，10 条确定式命令
> （`fs.open` / `fs.open.tab` / `fs.open.window` / `fs.copy.toclip` / `fs.cut.toclip` /
> `fs.properties` / `fs.rename` / `fs.delete` / `fs.copy` / `fs.move`）不是"mod 有而我们没有"，
> 而是"机器需要按路径点名操作"的补充 —— 详见 §3.5 与 `docs/OPERATION-MODEL.md` 第 4 步。
> 另 3 条（`tab.rename` / `tab.color` / `tab.close.duplicates`）是标签右键菜单的补齐，
> 每条都同时登记为命令（R20 人机等价：菜单动作必须有键盘/面板可达路径）。
>
> 2026-11-22 的覆盖率批次又补了 5 条：`bmk.import` / `bmk.export` / `bmk.reload`
> （书签文件 IO，人走对话框、机器给完整路径）、`clipboard.copy` / `ext.terminal.at`
> （面包屑与书签右键此前直连底层函数）。它们补齐的不是新能力，而是已有的
> UI 动作缺少结构化入口这一缺口；现在菜单、面板、快捷键与 AI 走同一条命令路径。
>
> Dev 组从 5 条涨到 9 条，来自同一天的开发面审计（diff / archive 等），与本次
> 人机等价补齐是两条独立的工作线；表格按命令表实测统一对账。
>
> Tool 组已实现 10 条：`OpListDir` / `OpStat` / `OpTopN` / `OpDiskUsage` 是**当前视图**的结构化查询；
> `tool.hash` / `tool.dupfinder` / `tool.compare` 是 2026-11-21 新增的文件集工具
> （`file_hash.cpp` + `file_tools.cpp`），它们回答的是"两份是不是同一份"；
> `tool.job.get` / `tool.job.cancel` / `tool.job.list` 是 2026-11-22 新增的**作业查询与控制**
> （前五条长任务命令改跑工作线程后配套的读面）。

> 对账口径：`src/commands_table*.cpp` 的 `implemented` 字段 × `app_commands*.cpp` 的 `reg(CmdId::X)`
> × `data/keymap.json` 的引用。一条命令"已实现"必须三者都在，`python tools/check_commands.py`
> 会同时校验；`tests/commands_test.cpp` 校验 keymap 里的每个命令 id 都存在。
> （旧版本手写的 90/36 与代码不一致：`GoTab1..9` 在 keymap 里绑了但目录里没有，
> `CopyToFolder`/`MoveToFolder` 也一直缺，已补齐。）

一句话结论：**File / Bookmark / Config / Dev / Session / Tool 六组全部落地；剩下的 7 条里搜索占 1
（`search.everything` 要接它的 IPC；`search.find` 已于 2026-11-22 走原生搜索文件夹修好），
另有 AI 面板（2，缺窗口内对话 UI），以及少量"本工程模型下没有意义/没有把握"的项
（合并窗口、复选框、RunCommand/SendKeys 等）。**

---

## 3. 逐组明细

### 3.1 Tab（33）—— 已实现 32

| 能力 | 状态 | 说明 |
|---|---|---|
| 新建/关闭/下一个/上一个/激活第 N 个 | 已实现 | 标签栏、`Ctrl+T/W/Tab`、命令面板 |
| **切换到第 1..9 个（`GoTab1..9`）** | 已实现 | 序列 `g 1`…`g 9`；此前目录里没有这 9 个 id，序列是空动作 |
| 关闭其他/左侧/右侧、关闭左/右一个 | 已实现 | 锁定标签会被跳过 |
| 复制标签、重新打开关闭的标签（栈深 20） | 已实现 | `core::TabSet::closed` |
| 左移/右移/移到首/尾、回到上一个激活（MRU） | 已实现 | `core::TabSet` 的 `order` / `mru` |
| 锁定标签（无关闭钮、不可关、持久化） | 已实现 | 右键菜单 → 锁定；`settings.json` 的 `lockedPaths` |
| 拖拽排序 / 中键关闭 / 滚轮切换 / 双击空白新建 | 已实现 | `tab_strip.cpp` |
| 溢出滚动 + `»` 全部标签菜单 | 已实现 | 标签宽度不足时自动进入 |
| 标签右键菜单（关闭族/复制/锁定/复制路径/重开） | 已实现 | 自绘弹层 |
| 与 explorer 标签合并（`TabMergeAll`） | 不适用 | 单窗口模型 |

### 3.2 Go（10）—— 全实现

桌面 / 下载 / 文档 / 配置目录 / LocalAppData / 控制面板 / 开始菜单 / 此电脑 /
PowerShell（在当前目录开终端）/ 主文件夹。

### 3.3 Nav（6）—— 全实现

后退 / 前进 / 向上 / 刷新 / 主文件夹 / 视图菜单。
面包屑（分段、ghost、每级 chevron、分段子目录下拉、折叠）、地址编辑、`Tab` 补全、
系统自动补全（`SHAutoComplete`，目录优先）、方向历史、最近访问下拉。

### 3.4 Path（8）—— 全实现

完整路径 / 文件名 / 当前目录 / UNC（`\\localhost\C$\…`）/ 扩展名 / 所有标签路径 → 剪贴板。
`PathCopyLink`（复制为快捷方式）已于 2026-11-25（0.3.2）实现：`path.copy.link` 把选中项
做成 **.lnk 虚拟文件**放进剪贴板（CFSTR_FILEDESCRIPTOR + CFSTR_FILECONTENTS），
粘贴得到真快捷方式而不是文本路径；不落盘。

### 3.5 File（36）—— 全实现

mod 的 25 条 + 本工程新增的 11 条命令（10 条确定式 `Fs*` + `Shift+Delete` 的永久删除入口）。

| 已实现 | 说明 |
|---|---|
| 复制 / 剪切 / 粘贴 / 删除 / 永久删除（`Shift+Delete`） | 走 shell 的 `IDataObject` + `IContextMenu` + `IFileOperation`（回收站/冲突/撤销交给系统）；永久删除仅去掉 `FOF_ALLOWUNDO`，仍保留 `FOF_WANTNUKEWARNING` 警告 |
| 重命名 / 属性 / 打开 | shell 右键菜单动词 |
| 新建文件夹 / 新建文本文件 | `IFileOperation::NewItem` / `CreateFileW`（自动去重名） |
| 全选 / 定位首项 / 定位末项 | `IShellView::SelectItem` |
| 在新标签 / 新窗口打开、打开方式 | 选中项或当前目录 |
| 刷新 | `IShellView::Refresh` |
| 复制到 / 移动到文件夹（`CopyToFolder` / `MoveToFolder`） | `IFileOpenDialog(FOS_PICKFOLDERS)` + `IFileOperation::CopyItems/MoveItems`，是**交互式**入口（弹框选目标） |
| **`FsRename` / `FsDelete` / `FsCopyTo` / `FsMoveTo`** | **确定式**：目标由参数点名，绝不弹框、绝不读选中。实现方式不是另写一份文件操作，而是 `shell_ops::SelectByPath` 先选中再复用既有 Selection 路径（否则回收站/冲突/权限语义会有第二份） |
| **`FsOpen` / `FsOpenInNewTab` / `FsOpenInNewWindow` / `FsProperties`** | 确定式版本，补掉"只能操作选中项"的债务 |
| **`FsCopy` / `FsCut`** | 与 `Ctrl+C/X` 同义：只准备剪贴板，不产生副本/移动。想真复制到某处用 `FsCopyTo` |

**全实现。** 2026-11-21 补齐最后两条：`SelectByType`（`file.select.type`，以首个选中项为
样板选同类扩展名）与 `BatchRename`（`file.rename.batch`，`Ctrl+Shift+R`）。

> 批量重命名**没有**接 PowerRename，而是做了最小可解释集合：模式串
> （`{name}` / `{ext}` / `{old}` / `{n}` / `{nn}` / `{nnn}`）+ 全部先校验再动手
> （重名 / 撞已有文件 / 非法字符 / 空名）+ 预览确认 + 逐项 `RenameTo`（`FOF_ALLOWUNDO`，
> `Ctrl+Z` 可退）。理由：PowerRename 不保证存在，而"按了没反应"正是要解决的问题本身。
> 纯函数 `filetools::PlanBatchRename` 可单测（`tests/file_tools_test.cpp`）。
>
> 选择族与撤销已于 2026-11-21 补齐：`SelectInvert`（`Ctrl+I`）/ `SelectFiles` /
> `SelectFolders`（`Ctrl+Alt+D`）/ `SelectByPattern`（`Ctrl+Shift+F`）走
> `shellops::SelectWhere` + `shellops::SelectByPattern`，匹配语义交给系统
> `PathMatchSpecExW`（不自创通配语法）；`CmdUndo`（`Ctrl+Z`）转发给 shell 视图自己的撤销栈。
> `SelectByType` 与它们共用 `SelectWhere`，缺的只是"取首项扩展名"这一步。
>
> 既没有单独的 `SelectionModel` 也不需要：`IFolderView::Item` + `SelectItem`
> 已经够用，先前的判断（"缺选择集抽象"）过于保守，实测直接做即可。
>
> 交互式与确定式**并列存在**不是重复：`F2` 的体验就是"弹输入框问名字"，
> 给 `CmdRename` 加参数会让"传了参数就不弹框"变成隐式分叉。拆开后人机各走入口、逻辑共用，
> 这条口径见 `docs/OPERATION-MODEL.md` 第 4 步与 `ARCHITECTURE-RULES.md` R6。

### 3.6 View（26）—— 已实现 25

8 档视图模式（按钮 / `Ctrl+Shift+1..8` / `ViewNext/ViewPrev` / 面板）、
排序（名称/日期/类型/大小）、分组（无/类型/日期/大小）、
隐藏文件 / 系统文件 / 扩展名、隐藏三态（状态栏眼睛段）、导航窗格开关、
**展开导航树（`Ctrl+Shift+E`，原生 `SysTreeView32` 逐节点 `TVM_EXPAND`）**、
**选择详细信息列（视图菜单 →「选择详细信息…」，`IColumnManager` 封装在 `shell_columns`）**、
**图标缩放（`Ctrl+滚轮` → `view.zoom`，视图菜单 →「图标大小…」 → `view.zoom.choose`）**、
预览窗格开关（`FWF_SHOWPREVIEWPANE`，用户意图跨目录保持）。

**最小双栏（2026-11-22，+3 条）**：一个标签持两个窗格，各有独立 `ShellView`、
历史、搜索来源和状态；右窗格按需创建，隐藏也保留目录/历史。
`view.dualpane.toggle`（`Ctrl+F6`）开合双栏，`view.pane.next`（`F6`）切活动窗格，
`view.pane.split {percent}`（20..80）设左栏宽度；配置键为 `dualPane` / `paneSplit`，
右栏路径与历史入会话快照。导航/命令/状态一律走活动窗格；切换窗格时标签标题、
地址栏、视图档位指示与 AI 快照都切到该窗格，两个视图本身保持原样。
会话落盘固定按左/右窗格写，切焦点不会把两边目录对调。原生导航窗格只给活动视图显示，
视觉上是一套共享侧栏。实现在 `src/app_panes.cpp` / `app_state.cpp` /
`app_commands_panes.cpp`，布局在 `src/app_layout.cpp`。

**垂直双栏（2026-11-23，+1 条）**：`view.pane.vertical`（`Ctrl+Shift+F6`）把两个窗格
从左右并排切成**上下堆叠**；配置键 `paneVertical`，AI 快照带 `paneVertical`。
**槽位与方向是两回事**：模型里始终是槽位 0/1，左右时 0=左 1=右，上下时 0=上 1=下；
路径/历史/选中/视图档位挂在**槽位**上，**方向只决定这两个槽位怎么摆**，
所以切方向不丢任何状态。分栏比例复用 `paneSplit` —— 左右表示左栏**宽度**百分比，
上下表示上栏**高度**百分比（同一把尺子）。导航树**不搬家**：始终占内容区最左侧、
占满整个高度，两个窗格都在它右边。活动窗格的真源仍是 `GetFocus()`，与方向无关。
中间是主题尺寸的缝（`Theme::splitter()`，96 DPI 下 6px，缝本身是独立分割条窗口
`ExBarBrowser_PaneSplitter`，可拖动改比例），每栏最小边长 80px。
措辞上全局改用"**主窗格 / 第二窗格**"（槽位 0/1），AI 字段名 `leftPanePath` /
`rightPanePath` **不改**（契约），只补注释说明是槽位语义。

实测（只读观测，不发合成输入）：启动即上下堆叠时 `TabView0 1038x317` /
`TabView1 1038x281`、分割条 `1038x6` 横贯、导航树仍 `176x604` 占满左侧全高；
运行时经 AI 通道 `config.set paneVertical` 热切，分割条由 `6x463` 变 `690x6`，
两栏由左右并排改为上下堆叠，导航树位置宽度不变。

**三条可拖边界（2026-11-23）**：内容区里"两块内容之间"的线现在都能拖：
导航树 | 文件区（`navPaneWidth`）、文件区 | AI 面板（`aiPanelWidth`）、
双栏之间（`paneSplit`）。共用 `src/split_bar.cpp` 的拖拽状态机，
只是落点口径不同（侧栏是像素、双栏是百分比）；边界条压在交界线上，
不额外占宽 —— `navPaneWidth=220` 仍等于"文件区从 x=220 开始"。
宽度上下限与"窗口变窄先收谁"收在 `src/layout_limits.h` / `side_layout.cpp`：
AI 先收、导航后收、文件区至少留 160px。侧栏边界没有时（面板收起/导航关闭）
对应的真窗口会隐藏，不留拖不动的把手。拖动只在松手时写盘，
`config.set` 也能改这两个宽度并立即重排。

**导航树组间分隔线（2026-11-23）**：原生导航树的根节点按 Explorer 习惯分三组，
组间画 1px 横线，色 `0x383838`（与 Explorer 实测一致），左端内缩 10px。
画法是**子类化 NSTC 内部的 `SysTreeView32`，在它的 `WM_PAINT` 走完之后补画**；
线位每次绘制现算（`GetItemRect` → `ScreenToClient`），滚动/展开/收起天然跟随。
另两条路已实测否决并记录在 `src/nav_pane_sep.h`：往 NSTC 插外来空行树项会让它崩；
本机创建不出 `WS_EX_LAYERED` 子窗口（`CreateWindowExW` 返回 NULL）。
两条线的**组界与 Explorer 相同**（`TAN-个人 | 桌面`、`tools | 123云盘`）。
实测像素：树客户区 159x604（隔离副本那轮）/ 159x463（当前树那轮），背景 `0x191919`，
两条线在 `y=89` / `y=299`，色 `0x383838`，`x=10..158`；两线相隔 210px = 7 行 x 30px，
与根清单里组界落在第 3 行（`桌面`）、第 10 行（`123云盘`）逐项对齐
（根清单 25~27 项，Nextcloud 客户端在跑时多 2 项，组界不变）。
已知差异：Explorer 组间有 30px 空行、线画在空行正中，我们只能在行边界画线；
Explorer 顶部有 15px 留白、我们没有。

列 id 由 `view.columns.list` 给出稳定 `{GUID}:pid` 字符串；菜单里的每次勾选都转投
`view.columns.set`，不直接改 shell 状态。`view.columns.choose` 只作为人工入口，不暴露给 AI。
缩放同理：`view.zoom` / `view.zoom.set` 经命令总线调 `ShellView::ZoomIcon`，
`view.zoom.choose` 只是视图菜单里的人工入口，不暴露给 AI。

未实现：`ToggleCheckboxes`。

### 3.7 Window（23）—— 已实现 21

新窗口、退出程序（`QuitExplorer` 重定义为关闭本程序，点分名 `win.quit`）、文件夹选项、
映射/断开网络驱动器、命令面板、快捷键管理器、聚焦地址栏、**聚焦搜索框（`Ctrl+F`，
2026-11-21 落地：`search-ms:` 协议）**、**首选项（2026-11-21 接上命令总线）**。

首选项这条原来记的是"面板已存在且视图菜单能打开，但没有走命令"——**两句都错**：
面板的真实入口是导航栏右键 →「设置…」（动作号 900），视图菜单里从来没有这一项；
命令目录里那条 `CmdId::Preferences` 则一直是 `implemented=false`，于是命令面板搜不到、
键位绑不上、agent 看不见。现在 `reg(CmdId::Preferences, …)` 打开的就是同一个面板，
锚点复用 `NavBar::SettingsAnchor()`，鼠标与命令两条路径弹在同一处。同时改掉那条声明里
另外两个错字段：`destructive=1`（开一扇窗不改任何不可逆状态）、`Scope::System`（不碰进程外）。

未实现：`AiTogglePanel` / `AiStop`
（缺窗口内对话 UI，接口层 `ai_api` / `ai_server` 已就绪）。

**窗口管理命令体系（2026-11-22，+11 条）**，全部经 `CommandBus`，实现在
`src/app_commands_window.cpp`：

- 单窗口几何/状态：`win.minimize` / `win.maximize` / `win.restore`（`ShowWindow`）、
  `win.topmost {on}`（`SetWindowPos` HWND_TOPMOST，省略 on = 切换）、
  `win.center`（居中到当前显示器工作区）、`win.move {x,y}` / `win.resize {width,height}`。
- 多窗口布局：`win.list`（只读列出所有 `ExBarBrowser_Main` 顶层窗口的 hwnd/pid/标题/矩形/状态）、
  `win.tile {orientation}`（横排/纵排）、`win.cascade`（级联错位）。枚举用 `EnumWindows`，
  布局一律带 `SWP_NOACTIVATE`，不抢前台焦点；跨进程只改窗口位置，不搬 ShellView。
- `win.activate` 把主窗口切到前台——**抢前台焦点，故 `aiExposed=false`**，与 `win.quit`
  同理：焦点归属不该由机器决定，只有快捷键/命令面板等人侧路径可达。
- 拆分标签已有 `tab.move.window`（有损：新窗口按路径开、源标签关，历史/视图状态不迁移——
  ShellView 是进程内 COM，跨进程搬不了）；**合并窗口未做**：需要跨进程标签传输协议，
  属另一件事，不在本次命令体系范围。
- 这些命令的断言统一豁免：窗口矩形/z 序/前台窗口不在 `exbar/state` 快照里，
  回读面是 `win.list` 的返回值，写不出 state 级判据。

> 会计口径：R21 禁的是**自动化脚本**模拟输入/抢焦点（`tools`/`scripts`/`tests` 零例外），
> 不是产品代码里由命令触发的窗口 API——`AGENTS.md` §R21 明确豁免 `src/` 里的
> `SetForegroundWindow` 等调用，判据是触发源而非 API 名字。

> `QuitExplorer` 是本组唯一的命名债：mod 期它的行为是"重启 explorer.exe"，本工程里是"退出本程序"。
> 2026-11-20 已把表里的主名改成 `win.quit`，枚举名 `CmdId::QuitExplorer` 按规则 C1 保留
> （改枚举名会打断 keymap / 审计日志 / 脚本）。见 [NAMING.md](NAMING.md) §4.5。

### 3.8 External（4）—— 已实现 2

在当前目录打开 Windows Terminal（普通 / 管理员，失败回退 PowerShell）。
`RunCommand` / `SendKeys` 不实现（安全风险 > 收益）。

### 3.9 Bookmark（15）—— 全实现

- 数据：`data\bookmarks.html`（Netscape + `EXBAR_ID/ICON/FLAGS`）。
  不再与 mod 共用同一份（便携模式各写各的），需要共用请手工拷贝。
- 书签栏：图标 + 名称、分类下拉（逐级下钻）、溢出 `»`、右键菜单
  （打开/复制路径/重命名/删除/新建分类/添加当前目录）、拖入文件夹添加、双击空白切换显隐。
- 分类内部也是可组织的（2026-11-25 补齐，原先只能读不能改）：
  - **拖动重排** —— 分类内容弹层里按住条目拖动即改顺序，与顶层那条栏同一套
    `bmk.move` 落点语义（插到第几行之前）。
  - **行内右键菜单** —— 打开 / 复制路径 / 编辑 / 复制书签 / 上移 / 下移 /
    移到书签栏 / 删除；子分类另有"解散分类"。
  - **解散分类**（`bmk.dissolve`，超出 mod 目录的新增能力）—— 分类右键菜单里的
    "解散分类"把该分类的**直接子项**提到它所在的上一层（顶层分类 → 书签栏），
    分类本身消失；嵌套分类解散只降一层。子项带着自己的子树一起走 ——
    散掉的是"这一层壳"，不是把整棵树拍平。改树前拍快照，可撤销。
  - **排序本分类**（`bmk.sort` 带 `parentId`）—— 只排这个分类的直接子项，
    不动别的层；与"按名称排序（含分类内）"的递归排序是两条不同的路。
- 显示/隐藏/切换：命令 + `Ctrl+Shift+B` + 鼠标规则。

### 3.10 Session（4）—— 全实现

`SessionSave`（保存当前标签组到 `settings.json`）、`SessionRestore`（重新打开）。
启动时若命令行没给路径，自动恢复上次会话（`sessionPaths` / `sessionActive`）。

`SessionList` / `SessionDelete`（2026-11-21）：**语义按真实模型收窄** —— 本工程只有
**一条**已保存的标签组（`save` 覆盖它，`restore` 整组重开），所以
`session.list` = 读出那一条（`{paths, count, active}`，人侧进只读报告窗口）、
`session.delete` = 清空那一条（`{cleared, count}`，破坏性 → agent 需 `approve`，
人侧先弹确认）。名字里的复数来自 mod 期能力；真要做多命名会话是另一件事
（要有名字、重名与覆盖策略），不该用这两条命令名去冒充 —— 否则"能列出 / 能删"
会在只有一条会话时变成假能力。

### 3.11 Tool（10）—— 全实现

| 命令 | 状态 | 说明 |
|---|---|---|
| `OpListDir` | 已实现 | 列出当前视图的项目（`shellops::ListView`），带 `limit` 截断标记；返回结构化 JSON |
| `OpStat` | 已实现 | 查单项属性（`shellops::StatItem`），必须在本目录内 |
| `OpTopN` | 已实现 | 谁最占空间：`entries[]` + `totalBytes` + `truncated`/`timedOut`/`cancelled`/`elapsedMs` |
| `OpDiskUsage` | 已实现 | 这目录占多少：`totalBytes` + 完整性标记 |
| `FileHash` | 已实现 | MD5/SHA-1/SHA-256，BCrypt 流式读（64 KB 块），`paths` 省略则用选中项 |
| `DupFinder` | 已实现 | 先按大小分组再比 SHA-256；`Ctrl+Shift+Y` / 命令面板；返回组 + `wastedBytes` |
| `FolderCompare` | 已实现 | 相对路径配对（大小写不敏感），同大小再比内容；`byContent=false` 只比大小 |
| `ToolJobGet` | 已实现（2026-11-22 新增） | 查作业状态；完成后带 `result` |
| `ToolJobCancel` | 已实现（2026-11-22 新增） | 请求取消；返回 `cancelRequested`（**已受理**，不等于已停下） |
| `ToolJobList` | 已实现（2026-11-22 新增） | 列出全部作业（新的在前，只给摘要） |
| ~~`OpReadText` / `OpNavigate` / `OpReveal` / `OpSelect` / `OpOpen` / `OpListCurrentView`~~ | **已删除命令表条目** | 六条是 `fs.open` / `tool.list` 的语义重复，且无 handler、无键位、无 agent 暴露。保留声明只是在目录里挂着永远不会实现的假能力，故删除。**枚举项与 legacy 名仍保留**（C1b/C2：删中间枚举会让后续 id 位移，删 legacy 名会让用户已有 keymap 失效），仅标废弃。 |

> `OpTopN` 与 `OpDiskUsage` 是**新写**的（mod 没有）：`src/disk_scan.cpp` 纯文件系统遍历、
> 不碰 COM，因此能脱离窗口单测。三条硬约束写在文件头：必须能中途停（deadline + 部分结果）、
> 必须报告完整性（`truncated`/`timedOut`）、不跟随重解析点（否则 junction 无限递归）。
> 单测 `tests/disk_scan_test.cpp` 26 项。
>
> 文件集三条（2026-11-21）**没有**按原计划做 helper 进程 + IPC：预算是显式的
> （`timedOut` / `complete` / `deniedDirs` 如实上报），而单文件摘要本来就必须读完整文件，
> helper 换来的只是"扫描时界面不卡"，代价是一整套 IPC 与生命周期管理。
> 代价是预算耗尽会少扫一部分 —— 报告里写清楚了，比假死或假数字好。
> 端到端验证：`obj/e2e_tools.py`（真实管道 + 真实文件，含改名副本、同大小不同内容）。
>
> **2026-11-22：长任务作业化。** 上面"预算够用"的判断只解决了"不会假死到没完"，
> 没解决"中途不能停"与"界面不冻结"。现在五条长任务命令全部搬到**工作线程**
> （`src/job.{h,cpp}`），配**进度窗口**（`src/job_ui.{h,cpp}`，带「取消」按钮）；
> 仍然**不用 helper 进程**（helper 要把结果整份搬回主进程，
> 而这里要的正是"边跑边看进度、随时能停"）。新增 `async` 参数与三条作业命令，
> 细节见 `docs/AI-INTERFACE.md` §9。顺带补上一个人侧的空白：
> `tool.topn` / `tool.diskusage` 过去**没有任何人可见的输出**（命令面板只调 `Invoke`
> 不调 `InvokeResult`，返回值被丢弃），现在跑完给一份可读报告。

### 3.12 Search（7）—— 已实现 6

**六条已实现、一条未实现。** 未实现的那条是 `search.everything`（要接 Everything 的 IPC）。

`OpSearch`（`search.find`）的来历值得留着：它曾标为已实现，走 shell 的 `search-ms:` 协议，
文档当时声称"结果落在同一个 `IShellView` 里，所以排序 / 右键 / 拖放照旧可用"。
**实测证明这句话是假的**：宿主 `IExplorerBrowser` 没有搜索带，`ShellView::DoNavigate`
对 `search-ms:` 前缀明确 `return false`，`App::RunSearch` 必然失败。于是 2026-11-21
把它降级为 `implemented=false`，让失败显形 —— **宁可少暴露，不可假绿**。

**2026-11-22 真搜索落地**（`src/search_folder.{h,cpp}` + `src/shell_view_search.cpp`）：
改走**原生搜索文件夹** —— `ISearchFolderItemFactory` 造 PIDL，再以 **PIDL** 形式
`BrowseToObject`。`DoNavigate` 拒绝的是**字符串前缀**、不是文件夹类型，所以同一个
文件夹以 PIDL 交出去 shell 就能绑定，导航过去就是**真的搜索结果视图**（排序 / 右键 /
拖放照旧可用）。同一套做法见 `D:\Windhawk\exbar.cpp:26215 RunSystemSearchFolder`。
结果视图默认**详细信息**（原生搜索文件夹的视图是异步创建的，所以定时等它就位再补设）。

实测（`profiles/multi` 通道）：

```
call search.find '{"query":"a"}' → ok=true      断言 searchQuery=$arg:query.trim 通过
scope=subtree  → 1371 条（含子目录）
scope=current  →   19 条（仅本层）
path=搜索: a · E:\ExBarBrowser（含子目录）
[search] 详情视图 hr=0x00000000 now=4 icon=16
```

`searchurl::Build` / `Trim`（`src/search_url.*`）与 `tests/search_url_test.cpp` 保留：
`Trim` 仍在用（搜索词规范化），`Build`（URL 形式）留作纯函数与单测参考 ——
**程序执行搜索不走 URL**（本宿主导航不了那个前缀）。

`SearchEverything`：需要接 Everything 的 IPC（mod 的 `evipc`）。

#### 3.12.1 已实现的五条：搜索框的配置面（2026-11-22）

导航栏搜索框按 exbar 的形态改成**分裂按钮**（左带=范围菜单，右带=历史下拉）后，
新增五条命令。它们**不执行搜索** —— 只管"搜索框长什么样、装什么词、搜哪个范围"，
每条都是本宿主真能兑现的能力：

| 命令 | 作用 | 回读面 |
|---|---|---|
| `search.scope.list` | 列出四档范围与当前档（带 `supported`：哪一档本宿主真能搜） | `searchRange` / `searchRangePath` |
| `search.scope.set {scope,path}` | 设范围档（`current`/`subtree`/`custom`；`thispc` 明确拒绝） | `searchRange`（= `$arg:scope`） |
| `search.history.list` | 列出提交过的搜索词（最近在前） | `searchHistoryCount` |
| `search.history.clear` | 清空搜索历史 | `searchHistoryCount`（= 0） |
| `search.query.set {query}` | 把词填进搜索框（**不代按回车**） | `searchInput`（= `$arg:query.trim`） |

四档范围真的进搜索后端（不再是 URL crumb）：`current` = 仅本层（作用域收窄到当前目录，
再用条件圈"父目录恰好等于它"）、`subtree` = 当前目录及其子目录、`custom` = 自定义路径
及其子目录、`thispc` = **本宿主做不到，明确报错**。范围的唯一真源是
`src/search_scope.{h,cpp}`（`searchscope::Resolve`），作用域的构造与约束见
`src/search_folder.h` 的头注释。

搜索历史记的是"**提交过的词**"（与是否成功无关）：宿主拒绝 `search-ms` 时，
若只在成功后记录，历史永远是空的，右带下拉就成了死功能。历史去重、置顶、上限 20，
随 `settings.json` 落盘。

**已知缺口不变**：`search-ms:` 提交仍被宿主拒绝 —— 范围/历史/回填三项真实可用，
但"按下回车看到结果"仍要换宿主或接 `ISearchFolderItemFactory`（`docs/EXPLORER-GAP-AUDIT.md` §2.1 F1）。

---

## 4. 差异的根因

| 根因 | 影响的命令 | 处理原则 |
|---|---|---|
| 不在 explorer 进程内 | `TabMergeAll`、`SendKeys` | 不适用 / 不实现 |
| 视图不自绘 | `ToggleCheckboxes` | shell 视图未暴露接口；预览窗格已改用 `IFolderView2` 的 folder flag，无需自绘 |
| 需要独立进程/协议 | Search 未实现的 1 条（`search.everything` 的 IPC） | `search.find` 已改走原生搜索文件夹（`ISearchFolderItemFactory`），不依赖独立进程；Everything 走 mod 的 `evipc` 协议。文件集三条**不需要**：显式预算 + 完整性上报已够（见 §3.11）。Search 组已实现的 6 条（搜索执行 + 范围/历史/回填）**不依赖**这条 |
| 缺窗口内对话 UI | `AiTogglePanel` / `AiStop` | 接口层已完成，只缺面板与历史（ROADMAP P4） |
| 语义与既有命令重叠 | 6 条 `tool.*` 包装命令 | 先确认没有第二种语义，再接线；重复一套实现比"未实现"更糟 |
| 安全边界 | `RunCommand`、`SendKeys` | 不做；AI 侧也不暴露 |

---

## 5. 快捷键对照（默认键）

| 动作 | mod 默认 | 本工程 | 一致性 |
|---|---|---|---|
| 新建 / 关闭标签 | `Ctrl+T` / `Ctrl+W` | 同 | 一致 |
| 下一个 / 上一个标签 | `Ctrl+Tab` / `Ctrl+Shift+Tab` | 同 | 一致 |
| 重新打开关闭的标签 | `Ctrl+Shift+T` | 同 | 一致 |
| 复制标签 | `Ctrl+K` | 同 | 一致 |
| 关闭其他 / 左 / 右 | `Ctrl+Shift+W` / `Ctrl+Shift+←/→` | 同 | 一致 |
| 聚焦地址栏 | `F4`（mod）/ `Ctrl+L` | `F4` + `Ctrl+L` | 超集 |
| 后退 / 前进 / 上级 | `Alt+←/→/↑` | 同 | 一致 |
| 刷新 | `F5` | 同 | 一致 |
| 复制路径 / 文件名 / 目录 | `Ctrl+Shift+C` / `Ctrl+Alt+C` / `Ctrl+Alt+Shift+C` | 同 | 一致 |
| 打开终端 / 管理员 | `Ctrl+M` / `Ctrl+Shift+M` | 同 | 一致 |
| 隐藏文件 / 系统 / 扩展名 | `Ctrl+H` / `Ctrl+Shift+H` / `Ctrl+Alt+E` | 同 | 一致 |
| 收藏当前目录 / 切换书签栏 | `Ctrl+D` / `Ctrl+Shift+B` | 同 | 一致 |
| 命令面板 / 快捷键管理器 | `Ctrl+Shift+P` / `Alt+K` | 同 | 一致 |
| 视图模式 | `Ctrl+Shift+1..8` | 同 | 一致 |
| 序列键 | 38 条（`g g`、`g t`…） | 169 条（全命令覆盖，见 `data/keymap.json`），另有模式总览弹窗 | 本工程增强 |
| 鼠标规则 | `empty` 双击上一级、`bookmark_empty` 双击切换书签栏 | 同 + 状态栏双击复制路径；Shell 视图目标由 UIA/ListView 探测接入 | 超集 |

完整键位表：`data/keymap.json`；输入系统细节：[INPUT.md](INPUT.md)。

---

## 6. 结论

- **日常浏览**（开标签、走路径、复制路径、开终端、切视图、收藏）已经不需要回到 explorer。
- **机器可调**（AI/脚本）：对 AI 暴露的命令都带 JSON Schema 与前置条件，走同一条 `CommandBus`，
  审计记 `actor=ai` —— 这一层 mod 没有，是本工程 0.2.1 的主要增量。
- **文件工具与搜索**是下一阶段的主战场，且都要先有 helper 通道。
- 不建议为了"127 条全绿"而硬造：`TabMergeAll`、`SendKeys`、`BatchRename`、
  `ToggleCheckboxes` 的投入产出比都低。
