---
status: current
baseline: 2026-11-21 21:00
---

# 能力对照：ExBar mod vs ExBarBrowser

> 回答一个问题：**独立进程路线现在能替代 explorer 到什么程度，缺口在哪，哪些缺口根本不该补。**
>
> 数据来源：`src/commands.cpp` 的命令目录（逐条对照 `implemented` 字段，不按 README 宣称），
> mod 侧对照其 127 条命令目录（`data/capabilities.json`）。本工程在 mod 的 127 条之外自加 13 条
> 命令（10 条确定式 `Fs*` + 3 条标签个性化 `tab.rename` / `tab.color` / `tab.close.duplicates`），
> 因此总数是 140。

---

## 1. 图例

| 标记 | 含义 |
|---|---|
| **已实现** | 本工程有命令，且能由用户直接操作（按钮 / 快捷键 / 序列 / 鼠标 / 面板） |
| **部分** | 能力存在但受限（参数不全 / 只覆盖文件系统 / 依赖系统版本） |
| **未实现** | 命令在目录里但 `implemented=false`；管理器显示"未实现" |
| **不适用** | 独立进程模型下没有意义，或与本工程设计前提冲突 |

---

## 2. 总览（2026-11-21 21:00 实测，脚本对账）

| 组 | 条数 | 已实现 | 未实现 |
|---|---:|---:|---|
| Tab | 33 | 32 | 1（`tab.merge`） |
| Go | 10 | 10 | 0 |
| Nav | 6 | 6 | 0 |
| Path | 7 | 6 | 1（`path.copy.link`） |
| File | 35 | 33 | 2（`file.select.type` / `file.rename.batch`） |
| View | 12 | 11 | 1（`view.toggle.checkboxes`） |
| Window | 12 | 9 | 3（`win.preferences` / `win.ai.toggle` / `win.ai.stop`） |
| External | 4 | 2 | 2（`ext.run` / `ext.sendkeys`） |
| Bookmark | 15 | 15 | 0 |
| Config | 3 | 3 | 0 |
| Session | 4 | 2 | 2（`session.delete` / `session.list`） |
| Tool | 13 | 7 | 6 |
| Search | 2 | 0 | 2 |
| **合计** | **156** | **136** | **20** |

> 口径：脚本按 `src/commands.cpp` 里 `{CmdId::X, …}` 字面形式逐条统计分组与 `implemented`，
> 不按 README 宣称。命令行：见 `tools/check_commands.py`（同时校验 keymap × 处理器三方一致）。
>
> 总数 156 里，10 条确定式命令
> （`fs.open` / `fs.open.tab` / `fs.open.window` / `fs.copy.toclip` / `fs.cut.toclip` /
> `fs.properties` / `fs.rename` / `fs.delete` / `fs.copy` / `fs.move`）不是"mod 有而我们没有"，
> 而是"机器需要按路径点名操作"的补充 —— 详见 §3.5 与 `docs/OPERATION-MODEL.md` 第 4 步。
> 另 3 条（`tab.rename` / `tab.color` / `tab.close.duplicates`）是标签右键菜单的补齐，
> 每条都同时登记为命令（R20 人机等价：菜单动作必须有键盘/面板可达路径）。
>
> Tool 组已实现 7 条：`OpListDir` / `OpStat` / `OpTopN` / `OpDiskUsage` 是**当前视图**的结构化查询；
> `tool.hash` / `tool.dupfinder` / `tool.compare` 是 2026-11-21 新增的文件集工具
> （`file_hash.cpp` + `file_tools.cpp`），它们回答的是"两份是不是同一份"。

> 对账口径：`src/commands.cpp` 的 `implemented` 字段 × `app_commands*.cpp` 的 `reg(CmdId::X)`
> × `data/keymap.json` 的引用。一条命令"已实现"必须三者都在，`python tools/check_commands.py`
> 会同时校验；`tests/commands_test.cpp` 校验 keymap 里的每个命令 id 都存在。
> （旧版本手写的 90/36 与代码不一致：`GoTab1..9` 在 keymap 里绑了但目录里没有，
> `CopyToFolder`/`MoveToFolder` 也一直缺，已补齐。）

一句话结论：**"看、走、记、标签、路径、视图开关、文件集工具、命令契约、AI 接口"已经补齐；
剩下的缺口是搜索（2，要接 Everything 的 IPC）、AI 面板（2，缺窗口内对话 UI）、
批量重命名（建议接 PowerRename）、按类型选择，以及 6 条与既有 `fs.*` 语义重叠的 `tool.*` 包装命令。**

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

### 3.4 Path（7）—— 已实现 6

完整路径 / 文件名 / 当前目录 / UNC（`\\localhost\C$\…`）/ 扩展名 / 所有标签路径 → 剪贴板。
`PathCopyLink`（复制为快捷方式）未实现。

### 3.5 File（35）—— 已实现 33

mod 的 25 条 + 本工程新增的 10 条确定式 `Fs*` 命令。

| 已实现 | 说明 |
|---|---|
| 复制 / 剪切 / 粘贴 / 删除 | 走 shell 的 `IDataObject` + `IContextMenu` + `IFileOperation`（回收站/冲突/撤销交给系统） |
| 重命名 / 属性 / 打开 | shell 右键菜单动词 |
| 新建文件夹 / 新建文本文件 | `IFileOperation::NewItem` / `CreateFileW`（自动去重名） |
| 全选 / 定位首项 / 定位末项 | `IShellView::SelectItem` |
| 在新标签 / 新窗口打开、打开方式 | 选中项或当前目录 |
| 刷新 | `IShellView::Refresh` |
| 复制到 / 移动到文件夹（`CopyToFolder` / `MoveToFolder`） | `IFileOpenDialog(FOS_PICKFOLDERS)` + `IFileOperation::CopyItems/MoveItems`，是**交互式**入口（弹框选目标） |
| **`FsRename` / `FsDelete` / `FsCopyTo` / `FsMoveTo`** | **确定式**：目标由参数点名，绝不弹框、绝不读选中。实现方式不是另写一份文件操作，而是 `shell_ops::SelectByPath` 先选中再复用既有 Selection 路径（否则回收站/冲突/权限语义会有第二份） |
| **`FsOpen` / `FsOpenInNewTab` / `FsOpenInNewWindow` / `FsProperties`** | 确定式版本，补掉"只能操作选中项"的债务 |
| **`FsCopy` / `FsCut`** | 与 `Ctrl+C/X` 同义：只准备剪贴板，不产生副本/移动。想真复制到某处用 `FsCopyTo` |

未实现：`SelectByType`（按首项的扩展名选中同类）、`BatchRename`（建议接 PowerRename）。

> 选择族与撤销已于 2026-11-21 补齐：`SelectInvert`（`Ctrl+I`）/ `SelectFiles` /
> `SelectFolders`（`Ctrl+Alt+D`）/ `SelectByPattern`（`Ctrl+Shift+F`）走
> `shellops::SelectWhere` + `shellops::SelectByPattern`，匹配语义交给系统
> `PathMatchSpecExW`（不自创通配语法）；`CmdUndo`（`Ctrl+Z`）转发给 shell 视图自己的撤销栈。
> `SelectByType` 与它们共用 `SelectWhere`，缺的只是"取首项扩展名"这一步。
>
> 反向选择/按类型选择不需要单独的 `SelectionModel`：`IFolderView::Item` + `SelectItem`
> 已经够用，先前的判断（"缺选择集抽象"）过于保守，实测直接做即可。
>
> 交互式与确定式**并列存在**不是重复：`F2` 的体验就是"弹输入框问名字"，
> 给 `CmdRename` 加参数会让"传了参数就不弹框"变成隐式分叉。拆开后人机各走入口、逻辑共用，
> 这条口径见 `docs/OPERATION-MODEL.md` 第 4 步与 `ARCHITECTURE-RULES.md` R6。

### 3.6 View（12）—— 已实现 11

8 档视图模式（按钮 / `Ctrl+Shift+1..8` / `ViewNext/ViewPrev` / 面板）、
排序（名称/日期/类型/大小）、分组（无/类型/日期/大小）、
隐藏文件 / 系统文件 / 扩展名、隐藏三态（状态栏眼睛段）、导航窗格开关、
预览窗格开关（`FWF_SHOWPREVIEWPANE`，用户意图跨目录保持）。

未实现：`ToggleCheckboxes`。

### 3.7 Window（12）—— 已实现 9

新窗口、退出程序（`QuitExplorer` 重定义为关闭本程序，点分名 `win.quit`）、文件夹选项、
映射/断开网络驱动器、命令面板、快捷键管理器、聚焦地址栏、**聚焦搜索框（`Ctrl+F`，
2026-11-21 落地：`search-ms:` 协议）**。

未实现：`Preferences`（设置面板已存在且视图菜单能打开，但**没有**走命令 —— 属"声明了没接线"，
应改为 `reg(CmdId::Preferences, …)` 打开同一个面板）、`AiTogglePanel` / `AiStop`
（缺窗口内对话 UI，接口层 `ai_api` / `ai_server` 已就绪）。

> `QuitExplorer` 是本组唯一的命名债：mod 期它的行为是"重启 explorer.exe"，本工程里是"退出本程序"。
> 2026-11-20 已把表里的主名改成 `win.quit`，枚举名 `CmdId::QuitExplorer` 按规则 C1 保留
> （改枚举名会打断 keymap / 审计日志 / 脚本）。见 [NAMING.md](NAMING.md) §4.5。

### 3.8 External（4）—— 已实现 2

在当前目录打开 Windows Terminal（普通 / 管理员，失败回退 PowerShell）。
`RunCommand` / `SendKeys` 不实现（安全风险 > 收益）。

### 3.9 Bookmark（15）—— 全实现

- 数据：`data\bookmarks.html`（Netscape + `EXBAR_ID/ICON/FLAGS`）。
  不再与 mod 共用同一份（便携模式各写各的），需要共用请手工拷贝。
- 书签栏：图标 + 名称、分类下拉（扁平化缩进）、溢出 `»`、右键菜单
  （打开/复制路径/重命名/删除/新建分类/添加当前目录）、拖入文件夹添加、双击空白切换显隐。
- 显示/隐藏/切换：命令 + `Ctrl+Shift+B` + 鼠标规则。

### 3.10 Session（4）—— 已实现 2

`SessionSave`（保存当前标签组到 `settings.json`）、`SessionRestore`（重新打开）。
启动时若命令行没给路径，自动恢复上次会话（`sessionPaths` / `sessionActive`）。
`SessionDelete` / `SessionList` 未实现（命名标签组需要额外 UI）。

### 3.11 Tool（13）—— 已实现 7

| 命令 | 状态 | 说明 |
|---|---|---|
| `OpListDir` | 已实现 | 列出当前视图的项目（`shellops::ListView`），带 `limit` 截断标记；返回结构化 JSON |
| `OpStat` | 已实现 | 查单项属性（`shellops::StatItem`），必须在本目录内 |
| `OpTopN` | 已实现 | 谁最占空间：`entries[]` + `totalBytes` + `truncated`/`timedOut`/`elapsedMs` |
| `OpDiskUsage` | 已实现 | 这目录占多少：`totalBytes` + 完整性标记 |
| `FileHash` | 已实现 | MD5/SHA-1/SHA-256，BCrypt 流式读（64 KB 块），`paths` 省略则用选中项 |
| `DupFinder` | 已实现 | 先按大小分组再比 SHA-256；`Ctrl+Shift+Y` / 命令面板；返回组 + `wastedBytes` |
| `FolderCompare` | 已实现 | 相对路径配对（大小写不敏感），同大小再比内容；`byContent=false` 只比大小 |
| `OpReadText` / `OpNavigate` / `OpReveal` / `OpSelect` / `OpOpen` / `OpListCurrentView` | 未实现 | 与既有的 `fs.open` / `fs.copy` / `tool.list` 语义重叠，属于"包装命令"，等确认没有第二种语义再接线 |

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

### 3.12 Search（2）—— 未实现

`SearchEverything` / `OpSearch`：需要接 Everything 的 IPC（mod 的 `evipc`）。

---

## 4. 差异的根因

| 根因 | 影响的命令 | 处理原则 |
|---|---|---|
| 不在 explorer 进程内 | `TabMergeAll`、`SendKeys` | 不适用 / 不实现 |
| 视图不自绘 | `ToggleCheckboxes` | shell 视图未暴露接口；预览窗格已改用 `IFolderView2` 的 folder flag，无需自绘 |
| 需要独立进程/协议 | Search 2（Everything 的 IPC） | 复用 mod 的 `evipc` 协议。文件集三条**不需要**：显式预算 + 完整性上报已够（见 §3.11） |
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
| 序列键 | 38 条（`g g`、`g t`…） | 同 | 一致 |
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
