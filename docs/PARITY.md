---
status: current
baseline: 2026-11-20 18:40
---

# 能力对照：ExBar mod vs ExBarBrowser

> 回答一个问题：**独立进程路线现在能替代 explorer 到什么程度，缺口在哪，哪些缺口根本不该补。**
>
> 数据来源：`src/commands.cpp` 的命令目录（137 条，逐条对照 `implemented` 字段，不按 README 宣称），
> mod 侧对照其 127 条命令目录（`data/capabilities.json`）。本工程在 mod 的 127 条之外自加 10 条
> 确定式 `Fs*` 命令，因此总数是 137。

---

## 1. 图例

| 标记 | 含义 |
|---|---|
| **已实现** | 本工程有命令，且能由用户直接操作（按钮 / 快捷键 / 序列 / 鼠标 / 面板） |
| **部分** | 能力存在但受限（参数不全 / 只覆盖文件系统 / 依赖系统版本） |
| **未实现** | 命令在目录里但 `implemented=false`；管理器显示"未实现" |
| **不适用** | 独立进程模型下没有意义，或与本工程设计前提冲突 |

---

## 2. 总览（2026-11-20 实测，脚本对账）

| 组 | 条数 | 已实现 | 未实现 | 不适用 |
|---|---:|---:|---:|---:|
| Tab | 28 | 27 | 1（`TabMergeAll`） | 0 |
| Go | 10 | 10 | 0 | 0 |
| Path | 7 | 6 | 1（`PathCopyLink`） | 0 |
| File | 35 | 28 | 7 | 0 |
| View | 12 | 11 | 1（`ToggleCheckboxes`） | 0 |
| Window | 12 | 8 | 4（搜索框 / 首选项 / AI 面板 / AI 停止） | 0 |
| External | 4 | 2 | 2（`RunCommand` / `SendKeys`） | 0 |
| Nav | 6 | 6 | 0 | 0 |
| Tool | 13 | 4 | 9 | 0 |
| Session | 4 | 2 | 2（删除 / 列表） | 0 |
| Bookmark | 4 | 4 | 0 | 0 |
| Search | 2 | 0 | 2 | 0 |
| **合计** | **137** | **108** | **29** | **0** |

> 总数 137 = mod 的 127 条 + 本工程自加的 10 条确定式命令
> （`FsOpen` / `FsOpenInNewTab` / `FsOpenInNewWindow` / `FsCopy` / `FsCut` / `FsProperties` /
> `FsRename` / `FsDelete` / `FsCopyTo` / `FsMoveTo`）。它们不是"mod 有而我们没有"，
> 而是"机器需要按路径点名操作"的补充 —— 详见 §3.5 与 `docs/OPERATION-MODEL.md` 第 4 步。
>
> Tool 组的 4 条已实现是 `OpListDir` / `OpStat` / `OpTopN` / `OpDiskUsage`：
> 后两条是 mod 里没有、为 AI 结构化查询新增的（mod 的 `ebop` 只到"导航/选中"层）。

> 对账口径：`src/commands.cpp` 的 `implemented` 字段 × `app_commands*.cpp` 的 `reg(CmdId::X)`
> × `data/keymap.json` 的引用。一条命令"已实现"必须三者都在，`python tools/check_commands.py`
> 会同时校验；`tests/commands_test.cpp` 校验 keymap 里的每个命令 id 都存在。
> （旧版本手写的 90/36 与代码不一致：`GoTab1..9` 在 keymap 里绑了但目录里没有，
> `CopyToFolder`/`MoveToFolder` 也一直缺，已补齐。）

一句话结论：**"看、走、记、标签、路径、视图开关、命令契约、AI 接口"已经补齐；剩下的缺口集中在
文件工具集（Tool 9 条）、搜索（2）、AI 面板（2）和少量文件操作（撤销/按类型选择/批量重命名）。**

---

## 3. 逐组明细

### 3.1 Tab（28）—— 已实现 27

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

### 3.5 File（35）—— 已实现 28

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

未实现：`CmdUndo`、`SelectInvert`、`SelectFiles`、`SelectFolders`、`SelectByType`、
`BatchRename`、`SelectByPattern`。

> 反向选择/按类型选择可以基于 `IFolderView::Item` + `SelectItem` 实现，但需要"选中集"抽象
> （`SelectionModel`）；批量重命名建议直接接 PowerRename，不自己做规则编辑器。
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

### 3.7 Window（12）—— 已实现 8

新窗口、退出程序（`QuitExplorer` 重定义为关闭本程序）、文件夹选项、
映射/断开网络驱动器、命令面板、快捷键管理器、聚焦地址栏。

未实现：`FocusSearch`（无搜索框）、`Preferences`、`AiTogglePanel`、`AiStop`。

### 3.8 External（4）—— 已实现 2

在当前目录打开 Windows Terminal（普通 / 管理员，失败回退 PowerShell）。
`RunCommand` / `SendKeys` 不实现（安全风险 > 收益）。

### 3.9 Bookmark（4）—— 全实现

- 数据：`data\bookmarks.html`（Netscape + `EXBAR_ID/ICON/FLAGS`）。
  不再与 mod 共用同一份（便携模式各写各的），需要共用请手工拷贝。
- 书签栏：图标 + 名称、分类下拉（扁平化缩进）、溢出 `»`、右键菜单
  （打开/复制路径/重命名/删除/新建分类/添加当前目录）、拖入文件夹添加、双击空白切换显隐。
- 显示/隐藏/切换：命令 + `Ctrl+Shift+B` + 鼠标规则。

### 3.10 Session（4）—— 已实现 2

`SessionSave`（保存当前标签组到 `settings.json`）、`SessionRestore`（重新打开）。
启动时若命令行没给路径，自动恢复上次会话（`sessionPaths` / `sessionActive`）。
`SessionDelete` / `SessionList` 未实现（命名标签组需要额外 UI）。

### 3.11 Tool（13）—— 已实现 4

| 命令 | 状态 | 说明 |
|---|---|---|
| `OpListDir` | 已实现 | 列出当前视图的项目（`shellops::ListView`），带 `limit` 截断标记；返回结构化 JSON |
| `OpStat` | 已实现 | 查单项属性（`shellops::StatItem`），必须在本目录内 |
| `OpTopN` | 已实现 | 谁最占空间：`entries[]` + `totalBytes` + `truncated`/`timedOut`/`elapsedMs` |
| `OpDiskUsage` | 已实现 | 这目录占多少：`totalBytes` + 完整性标记 |
| `OpReadText` / `OpNavigate` / `OpReveal` / `OpSelect` / `OpOpen` / `OpListCurrentView` | 未实现 | 与既有的 `FsOpen` / `FsOpenInNewTab` / `FsCopy` 语义重叠，等 helper 通道落地后合并，不重复造 |
| `DupFinder` / `FileHash` / `FolderCompare` | 未实现 | 需要 helper 进程与 IPC 协议（mod 的 `ebtool`），本工程当前没有 helper 通道 |

> `OpTopN` 与 `OpDiskUsage` 是**新写**的（mod 没有）：`src/disk_scan.cpp` 纯文件系统遍历、
> 不碰 COM，因此能脱离窗口单测。三条硬约束写在文件头：必须能中途停（deadline + 部分结果）、
> 必须报告完整性（`truncated`/`timedOut`）、不跟随重解析点（否则 junction 无限递归）。
> 单测 `tests/disk_scan_test.cpp` 26 项。

### 3.12 Search（2）—— 未实现

`SearchEverything` / `OpSearch`：需要接 Everything 的 IPC（mod 的 `evipc`）。

---

## 4. 差异的根因

| 根因 | 影响的命令 | 处理原则 |
|---|---|---|
| 不在 explorer 进程内 | `TabMergeAll`、`SendKeys` | 不适用 / 不实现 |
| 视图不自绘 | `ToggleCheckboxes` | shell 视图未暴露接口；预览窗格已改用 `IFolderView2` 的 folder flag，无需自绘 |
| 无 helper 通道 | Tool 9、Search 2 | 复用 mod 的 helper 与协议，不新建进程（`Op*` 四条已用纯文件系统遍历落地，不需要 helper） |
| 选择集抽象缺失 | 反向选择 / 按类型选择 / 复制到 / 移动到 | 先落 `SelectionModel`，再补命令（`CopyToFolder`/`MoveToFolder` 已用交互式对话框绕过） |
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
- **机器可调**（AI/脚本）：86 条命令带 JSON Schema 与前置条件，走同一条 `CommandBus`，
  审计记 `actor=ai` —— 这一层 mod 没有，是本工程 0.2.1 的主要增量。
- **文件工具与搜索**是下一阶段的主战场，且都要先有 helper 通道。
- 不建议为了"127 条全绿"而硬造：`TabMergeAll`、`SendKeys`、`BatchRename`、
  `ToggleCheckboxes` 的投入产出比都低。
