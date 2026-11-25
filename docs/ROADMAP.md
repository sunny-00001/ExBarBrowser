---
status: current
baseline: 2026-11-22 20:25
---

# 路线图

> 阶段划分沿用 ExBar 主仓的 P0–P4 编号（见 `ExBar换栈总体方案.md`），但**内容是独立的**：
> 本工程不等待 mod 的换栈进度，也不因 mod 的改动而重排。
>
> 每条里程碑必须带"可验收的判据"，否则不进计划表。能力缺口清单见 [PARITY.md](PARITY.md)。
> 已完成的阶段不删段落 —— 把"原计划"与"实际做法"并列保留，是后续判断"为什么不按计划走"的唯一依据。

---

## 0. 三条原则

1. **先补日常，再补差异化**。用户每天用"复制路径、关其他标签、隐藏文件"，而不是"AI 面板"。
2. **能复用 mod 的资产就复用，但只复用数据格式与协议，不复用代码**。书签、会话、搜索通道、helper 都是资产；
   `ebch` 那 1.3 万行不是。
3. **每个阶段都要能回退到上一版**。没有开关的改动不做；没有验收判据的功能不开工。

---

## 1. 阶段总览

```
P1  骨架            ✅ 2026-11-19 完成
 │
P1.1 收尾            ✅ 2026-11-19 完成（标签溢出 / 视图菜单 / nav_bar 拆分）
 │
P2  可用（日常替代）  ✅ 2026-11-19 完成（配置/标签/路径/书签/会话/视图开关/输入端）
 │                    退出判据：连续一周作为默认文件夹处理器不出问题
 │
P2.5 契约化          ✅ 2026-11-20 完成（v0.2.1）
 │                    操作契约 10 步 + AI 接口 + 导航组件化 + 便携存储 + 基础设施收编
P3  好用（可发现）    搜索 → 工具（helper IPC）→ 预览窗格 → 批量选择/复制到/移动到
 │                    已提前落地：目录统计（OpTopN/OpDiskUsage）、书签导入导出与拖放
P4  差异化            AI 面板 → chrome Web 化 → 与 mod 的 UI 资产互通
 │                    拆三期（2026-11-22）：P4-a 原生活动流与审批（零新依赖）
 │                    → P4-b 自带模型客户端 + 纯文本流式消息区（形态 A，不引入 WebView2 / DirectWrite）
 │                    → P4-c 渲染层 + 动作粒度（检索层 / 多步编排 / 计划级确认）+ 与 mod 资产互通
```

版本号映射见 [../CHANGELOG.md](../CHANGELOG.md) 末尾。

---

## 2. P1.1 收尾

| 项 | 现状 | 做法 | 验收 |
|---|---|---|---|
| 标签溢出 | 标签总宽超过窗口后右侧被裁 | 两种方案二选一：①总宽超限时按比例压缩到最小 112 px；②横向滚动（`WM_MOUSEHWHEEL` + 左右箭头按钮）。**先做①**，成本低、无新状态 | 开 20 个标签，全部可见可点，`+` 按钮始终在窗口内 |
| 盘根 chevron | 已改为列该级自己的子文件夹，盘根也有效 | 已修（见 CHANGELOG） | 盘根 chevron 列出该盘顶层文件夹 |
| 弹层二级菜单 | `PopupRow::folder` 的平铺形态画了 chevron 但没有子菜单 | 分段下拉已改走 `ShowFolderTree`（支持就地展开）；平铺形态只在不需要展开时使用 | 视觉与行为一致，用户不会以为能展开 |
| 视图菜单选中态 | 8 档能切，但弹层里不标当前档 | `PopupRow` 增加 `checked` 字段（或复用 `data`），当前档打勾 | 打开菜单时当前模式有勾 |
| `/select` 选中 | 只打开父目录 | `ShellView::Navigate` 后取 `IShellView` → `IFolderView::SelectItem`（按解析名匹配） | `ExBarBrowser.exe /select,"C:\Windows\notepad.exe"` 打开 `C:\Windows` 且 notepad.exe 处于选中态 |
| 标题与状态栏 | 标题 `名称 — ExBar`；状态栏无选中信息 | 标题改为 `名称 — ExBarBrowser`（与进程名一致）；状态栏左侧在"项目数"后追加"选中 N 项"（需先取 `SVGIO_SELECTION`） | 标题与任务栏一致；选中 2 个文件后状态栏显示"选中 2 项" |
| 技术债：`nav_bar.cpp` 549 行 | 已超 500 行上限 | 把 `Paint` / `DrawBreadcrumb` / `DrawButton` / `DrawGlyph` 拆到 `nav_bar_paint.cpp` | `wc -l src/*.cpp` 全部 ≤500 |
| 技术债：日志两份实现 | `common.h::LogDbg` 与 `shell_view.cpp::LogLine` 重复 | 让 `shell_view.cpp` 改用 `LogDbg`，删掉本地实现 | 全仓只有一处日志实现 |
| 关闭确认 | 无 | 不做（保持浏览器手感）；只在"关闭多个标签"时可选 | — |

---

## 3. P2 可用（目标：可当默认文件夹处理器）

### 3.1 P2-0 配置与状态（一切的前置）

**问题**：现在窗口几何、标签、视图设置全不保存，每次启动从零开始。

**方案**：与 mod 共用同一套目录与 Schema，不另起格式。

```
%APPDATA%\ExBar\            （可漫游、可 diff、可备份）
├─ settings.json            本程序的标量偏好（新增 "exbrowser" 段，不污染 mod 的键）
├─ bookmarks.html           与 mod 共用（Netscape 格式 + EXBAR_* 扩展）
%LOCALAPPDATA%\ExBar\
├─ state\state.json         窗口几何、MRU（新增 "exbrowser" 段）
└─ state\sessions.txt       与 mod 共用（纯文本，一行一组标签）
```

> **已落地实现与此处规划不同（见下方"现状"）**：最终选择了便携模式，数据放在
> `<exe>\data`，且书签不再与 mod 共用。本节保留为当时的规划原文。

**现状（P2-0 实际落地）**：

```
<exe>\data\
├─ settings.json             设置/会话/最近访问/视图与窗格偏好/键位覆盖
├─ bookmarks.html           书签（Netscape + EXBAR_*，不再与 mod 共用）
├─ keymap.json              键位（直接编辑）
├─ mouse.json               鼠标规则（直接编辑）
└─ audit.log                命令审计
```

理由：整个程序目录可整体拷走（便携/绿色部署），不写注册表、不写 `%APPDATA%`；
书签不共用是为了两个程序各写各的、避免互相覆盖。首次运行若检测到旧的
`%APPDATA%\ExBar` 里有本工程的文件，会一次性拷贝过来（旧目录保留不删）。

**约束**：

- 写入必须是"读-改-写 + 原子替换（`MoveFileEx` + `REPLACE_EXISTING`）"，不能整文件覆盖别人的段。
- 解析失败保留上一份，不覆盖坏文件（沿用 mod 的 `BadInput*` 策略）。
- 不做文件监听热重载（P2 不需要）；启动读一次，退出写一次。

**验收**：改窗口大小 → 关闭 → 重开，几何还原；手工把 `settings.json` 改坏 → 启动不崩、日志记一条、用默认值。

### 3.2 P2-1 标签管理

| 功能 | 实现要点 | 验收 |
|---|---|---|
| 标签右键菜单 | `TabStrip` 新增 `WM_RBUTTONUP` → `WM_APP_TAB_MENU(index)`，`App` 弹菜单 | 右键有：关闭/关闭其他/关闭左侧/关闭右侧/复制标签/复制路径 |
| 拖拽排序 | `TabStrip` 记录 `dragFrom_`，`WM_MOUSEMOVE` 超过阈值进入拖拽，`WM_LBUTTONUP` 发 `WM_APP_TAB_MOVE(from,to)` | 拖 3 个标签重排后，视图内容与标题跟着走（不是只换标题） |
| 已关闭标签栈 | `App` 维护 `closed_`（最多 20 条，存路径 + 显示名），`Ctrl+Shift+T` 恢复 | 关 3 个再恢复 3 个，顺序为后进先出 |
| `Ctrl+1..9` | `ACCEL` 加 9 行 + `WM_COMMAND` 范围判断 | 第 9 个键跳到最后一个标签（浏览器惯例） |
| 上一个激活的标签 | `App` 维护 `prevActive_`，`TabPrevTab` 用它 | 在 A→B→C 后按该键回到 B |

**风险**：拖拽与"点击切换"容易打架（阈值太小误触发）。**回退**：拖拽用独立开关
（`settings.json` 的 `exbrowser.tabDrag`），关掉即回到 P1 行为。

#### 3.2.1 标签栏仍缺的（2026-11-21 对照 Chrome / Edge / VS Code / Opus 复核）

已完成的见 `CHANGELOG.md`（右键 7 项、重命名/颜色、拖拽指示线、溢出菜单只列隐藏项、
`Ctrl+1..9`、`Ctrl+Tab` 系列、快速关闭、`»` 命中修复、`WM_CAPTURECHANGED` 复位、
双击标签正文关闭、`+` 移到标签右侧并固定打开此电脑、拖拽边缘自动滚动、Esc 取消拖拽、
关闭后焦点显式化）。

按"用户会不会真的撞上"排。✅ = 本轮已完成，保留行以对照当时的判断与实际做法：

| 缺口 | 现状 | 为什么该做 | 参考 |
|---|---|---|---|
| **固定标签（Pin）** | 只有"锁定"（不可关闭），没有 Pin | 两者语义**不同**：Pin = 缩成图标贴到最左、不参与滚动、按站点持久化；Lock 只是"关不掉"。VS Code 的 pinned 会"永远排在最前且滚不出去"，本项目锁定标签在溢出后仍会被滚走。**待独立迭代**：它要重塑标签栏布局（pinned 分区/窄图标/独立滚动域），与"+"移位同批做风险过高，故本轮未做 | Chrome、VS Code、Edge 三家都有 |
| ✅ **拖拽到边缘自动滚动** | 已完成：溢出时拖到左/右边缘带触发 30ms 定时器滚动，落点与 MOUSEMOVE 同一口径 | （当时的判断）拖到视野外只能靠菜单绕；Chrome/Edge 拖到边缘自动横向滚动 | Chrome、Edge、VS Code |
| **拖拽出标签栏 → 新窗口** | **已实现（2026-11-21）**：单实例锁已移除，`OpenPathInNewWindow` 启动子进程时带 `--new-window`，直接新开实例、共享同一份用户数据 | 已落地：默认启动是"激活优先"（转发给已有窗口开标签），显式 `--new-window` 才开新窗口 | Chrome、Edge、Opus |
| ✅ **关闭标签后的焦点落点** | 已完成：关闭前记录焦点是否在被关视图，关掉后显式 `FocusActiveView()`，不依赖销毁次序 | （当时的判断）原本靠 `GetFocus()==null` 走 `!focus` 分支侥幸搬回，换关闭入口就落空 | 显式判一句更稳 |
| **中键粘贴 / 中键关闭锁定标签** | 中键关标签已实现（跳过锁定）；没有"中键点标签栏空白 = 打开剪贴板路径" | Chrome 中键点空白无行为，但 TC/Opus 有。可选 | TC、Opus |
| **标签组（同色自动折叠）** | 颜色只作视觉标记 | 颜色的价值在于"能折叠成一组"，现在有颜色但无行为，长期会觉得是装饰 | Chrome、Edge、VS Code |
| **标签搜索 / 快速跳转** | 无（有 `Ctrl+1..9` 与 `g 1..9` 序列） | 标签超过 ~15 个后位置记忆失效。Chrome 用 `Ctrl+Shift+A`，VS Code 用 `Ctrl+P` | Chrome、VS Code |
| ✅ **拖拽时 Esc 取消** | 已完成：Esc 复位拖拽态并释放捕获，松手不移位；`CancelDrag` 统一服务它与 `WM_CAPTURECHANGED` | （当时的判断）拖到一半想放弃只能松手，松手就真的移了 | Chrome |
| **关闭历史不保留名字/颜色** | `closed_` 只存路径 | 重开一个改过名/上过色的标签，名字和颜色都丢了（按路径持久化那条会救回来，但如果该路径的 tabMeta 已被清掉就丢） | — |

**优先级建议**：固定标签（Pin）> 拖拽边缘滚动 > 拖拽出标签栏 > 关闭后焦点 > 其余。
前两条是"标签一多就天天撞"的，后两条是边界体验。
本轮进度：边缘滚动、拖出窗口（受阻）、焦点已处理；Pin 留作下一独立迭代（布局重塑）。
本轮另补齐了用户直接提出的双击关闭 / `+` 移位 / `+` 打开此电脑。


### 3.3 P2-2 路径与外部命令

| 功能 | 实现要点 | 验收 |
|---|---|---|
| `PathCopy*`（6 条） | 新文件 `src/path_copy.cpp`：取选中项（`IFolderView::Items(SVGIO_SELECTION)`）→ `GetDisplayNameOf`；无选中则用当前目录 | 选中 2 个文件复制，剪贴板是两行完整路径（CRLF 分隔，与 mod 一致） |
| `PathCopyTab` | `Tab::path()` + `OpenClipboard` | 复制的是当前标签路径 |
| `OpenTerminal(Admin)` | `ShellExecuteW(L"wt.exe", L"-d \"<path>\"")`，失败回退 `powershell.exe -NoExit -Command Set-Location` | 在 `C:\Windows` 按 `Ctrl+M` 打开终端且工作目录正确 |
| `FolderOptions` / `MapDrive` / `UnmapDrive` | `ShellExecuteW(L"control.exe", L"folders")` 等 | 能打开对应系统对话框 |

**剪贴板注意**：`OpenClipboard(hwnd)` 失败要重试 3 次（别的进程可能占着）；写完必须 `CloseClipboard`。
用 `CF_UNICODETEXT`，行分隔用 `\r\n`。

### 3.4 P2-3 书签栏

| 项 | 内容 |
|---|---|
| 存储 | `data\bookmarks.html`（本工程自己的书签，不再与 mod 共用） |
| UI | 新模块 `src/bookmark_bar.cpp`，高度 `S(28)`，插在导航栏与视图区之间；可折叠（`Ctrl+Shift+B`） |
| 交互 | 单击导航、中键新标签、右键"添加当前目录/删除/重命名" |
| 验收 | mod 里加的书签在本程序可见可点；本程序隐藏书签栏后布局正确回收高度 |

**风险**：两个程序同时写同一个 html 会互相覆盖。**决策**：P2 只读；写入要等"文件锁 + 合并写"设计定稿
（见 §6 待决策）。

### 3.5 P2-4 会话

| 项 | 内容 |
|---|---|
| 存储 | `%LOCALAPPDATA%\ExBar\state\sessions.txt`（与 mod 同格式：一行一组，组名 + 制表符 + 路径列表） |
| 命令 | 保存当前全部标签为命名组、恢复、删除、列出（对应 `Session*` 4 条） |
| UI | 先做命令 + 一个简单输入框；完整的会话管理器放 P3（与命令面板一起） |
| 验收 | 保存 5 个标签 → 关闭程序 → 重开 → 恢复，标签顺序与路径完全一致 |

### 3.6 P2-5 视图开关

| 项 | 实现要点 | 验收 |
|---|---|---|
| 隐藏文件 / 系统文件 / 扩展名 | `SHGetSetSettings`（`fShowAllObjects` / `fShowSuperHidden` / `fShowExtensions`）→ 对视图发 `WM_COMMAND(IDM_VIEW_...)` 或 `IShellView::Refresh` | 切换后当前视图立即生效，且**只影响本进程**（不改全局） |
| `ToggleNavPane` | `SetOptions(0)` / `SetOptions(EBO_SHOWFRAMES)` + 重新 `SetRect` | 切换后导航树出现/消失，视图不错位 |
| `ViewNext` / `ViewPrev` | 复用 `nav_bar_actions.cpp` 的 `kViews` 表，`App` 记当前档位 | 连按 8 次回到起点，弹层里的勾跟着变 |
| 状态栏眼睛段 | 新控件段落（左键循环三态），依赖上面三项 | 点三次回到初始态 |

**风险**：`SHGetSetSettings` 是进程级全局设置，可能影响同进程内所有标签（可接受）但**不要**影响系统其他窗口
（它是 per-process 的，不会写注册表——需实测确认）。

---

## 4. P3 好用（目标：能力可发现、可编排）

### 4.1 P3-0 命令目录（前置）—— ✅ 已完成（超额）

原计划"编成 `data/capabilities.json` 的子集（id/摘要/参数/风险级）"，实际做得更彻底：
命令表自带 `argsSchema`（JSON Schema，直接复用为 MCP `inputSchema`）、`outputSchema`、
`Effects{readOnly,destructive,idempotent,openWorld,undoable,scope}`、`Requirements` 前置条件、
`aiExposed`/`needsSelection` 暴露决策，以及点分命名空间（`nav.back` ↔ `NavBack` 双向）。
命令目录 / 已实现 / 对 AI 暴露三项由 `tools/check_commands.py` + `check_ai_meta.py` 三方对账得出，本文不写死数字（见 P2.5 判据）。

**验收**：命令面板能搜到全部已实现命令（✅）；`implemented: false` 显示灰色并注明"未实现"（✅）。

### 4.2 P3-1 命令面板 —— ✅ 已完成（P2 提前落地）

`Ctrl+Shift+P` 打开，与快捷键同走 `CommandBus`。

### 4.3 P3-4 快捷键管理 —— ✅ 已完成（P2 提前落地）

`Alt+K` 打开管理器：范围筛选、改键/清除/恢复默认、冲突/系统占用/未实现标记。
存储改为 `data\keymap.json`（不再有 `exbrowser` 段，与 mod 的数据目录已分离）。

### 4.4 P3-2 搜索（一半已做：`search.find`；Everything 通道待做）

- **已做（2026-11-21）**：`search.find` 走 shell 自己的 `search-ms:` 协议，结果落在
  同一个 `IShellView` 里 —— 排序 / 右键 / 拖放照旧可用，不需要自绘结果列表，
  也不需要 Everything。导航栏搜索框与命令总线共用 `App::RunSearch` 一份实现，
  URL 拼装是纯函数（`src/search_url.*` + 单测），端到端见 `obj/e2e_search.py`。
  语义：搜索是**导航**，空串 / 只有空白 = 退出搜索回到来源目录。
- 待做：接 `evipc`（Everything IPC）：复用 mod 的 actor 化客户端，命名管道 + 共享内存。
  对应命令 `SearchEverything`。这一步的价值是"全盘按文件名即时搜索"（200 ms 内出结果），
  与 `search.find` 的"当前目录内查"是两件事 —— 所以没有拿 `search-ms` 去冒充它。
- **验收**：输入 `*.log` 在 200 ms 内出前 50 条；双击结果导航到所在目录并选中。

### 4.5 P3-3 工具（helper IPC，部分已做）

- 原计划复用 `ExBarHelper.exe` 的 `kVer=2` 协议。**实际拆成两半**：
  - **不需要 helper 的部分已经做完**：`OpListDir` / `OpStat` / `OpTopN` / `OpDiskUsage`
    走纯文件系统遍历（`src/disk_scan.cpp`），不碰 COM、可脱离窗口单测。
  - **文件集工具（2026-11-21 已完成，未使用 helper）**：`FileHash` / `DupFinder` / `FolderCompare`。
    原判断是"放 UI 线程会假死，必须进 helper 或工作线程 + `PostMessage`"，实测下来
    **显式预算已经够用**：`timedOut` / `complete` / `deniedDirs` / `hashedFiles` 如实上报，
    调用方看得见"没扫完"；而单文件摘要本来就必须读完整文件，helper 只能把等待挪走，
    换不来结果正确性，却要多背一整套 IPC 与生命周期管理。
    真要做后台化，正确形态是**把这三条挪到工作线程 + 进度窗口**，不是再造 helper 进程。
- **长任务作业化（2026-11-22 已完成）**：上一条留下的"真要做后台化"已经做了 ——
  五条长任务命令（`tool.dupfinder` / `tool.hash` / `tool.compare` /
  `tool.diskusage` / `tool.topn`）全部搬到**工作线程**（`src/job.{h,cpp}`），
  配**进度窗口**（`src/job_ui.{h,cpp}`，带「取消」按钮）。
  **没有用 helper 进程**：helper 要把结果整份搬回主进程，
  而这里要的正是"边跑边看进度、随时能停"。
  - 新增 `async` 参数：立刻返回 `jobId`，把"跑多久"与"等多久"拆开
    （AI 管道只有 10 秒超时，同步扫大目录必然超时）。
  - 新增三条作业命令 `tool.job.get` / `tool.job.cancel` / `tool.job.list`，
    以及事件 `tool.job.progress` / `tool.job.done`。
  - 结果新增 `cancelled`（与 `timedOut` 分开：超时可重试，取消重试只是白费力气）。
  - 人工路径顺带补上了空白：`tool.topn` / `tool.diskusage` 过去**没有任何人可见的输出**
    （命令面板只调 `Invoke` 不调 `InvokeResult`，返回值被丢弃），
    现在跑完给一份可读报告。
- 授权：默认只读；写操作需显式开关（与 mod 的 `approval` 模型一致，AI 侧双门已实现）。

### 4.6 P3-5 预览窗格（待做）

- `IPreviewHandler` + 自绘面板（右侧 `S(320)`），支持文本/图片/PDF（系统已注册的 handler）。
- 依赖：`IFolderView` 选中变化通知（`DAdvise` `SVGIO_SELECTION` 或轮询 `WM_ACTIVATE`）。
- **这是本阶段最大的一块**，如果 P3 时间不够，可以只做"文本 + 图片"两种。

### 4.7 P3-6 选择族与撤销（2026-11-21 大部分已完成）

已完成：`SelectInvert` / `SelectFiles` / `SelectFolders` / `SelectByType` / `SelectByPattern` /
`CmdUndo` / `BatchRename`（模式串 + 预览 + 全部先校验，未接 PowerRename）。
**前置判断被推翻**：不需要单独的 `SelectionModel` 抽象 —— `shell_ops` 的
`SelectWhere(pred)` / `SelectByPattern` 直接基于 `IFolderView::Item` + `SelectItem`
就够用，先造抽象反而会多一层"谁持有选中集"的同步问题。

仍未做：
- `ToggleCheckboxes` —— shell 视图无公开接口（DECISIONS D2），键位已删。
- `PathCopyLink` —— 需要判断"能否生成链接"，低价值。

---

## 5. P4 差异化

P4 拆成三期（2026-11-22 决定）：**P4-a 不引入任何新依赖**，
**P4-b 只引入自带模型客户端**（WinHTTP + SSE，消息区做**纯文本流式**，不引入 WebView2 也不引入 DirectWrite/Direct2D ——
先验证链路，渲染层后置，见 D3），
**P4-c 补渲染层与动作粒度**（检索层 / 多步编排 / 计划级确认，见 D11；
三项都**先记着，等 P4-b 跑完再定**）。
拆的理由是 P4-a 的产物（活动流 + 审批）本身就是安全边界，必须能独立验收 ——
不能让它与"WebView2 可用性""agent 装没装"这些外部条件绑在一起。
同理，P4-b 的模型链路也不该与"富文本渲染做没做"绑在一起。

### 5.1 P4-a 原生活动流与审批（零新依赖）

| 项 | 内容 | 前置 |
|---|---|---|
| **宿主侧审批通道** | **新加，工作量可能大于面板本身**：宿主挂起一个待批请求 → 面板显示为面板级固定条 → 用户点完回执给请求方。现有双门是**客户端声明式**的（`aiAllowDanger` 静态开关 + 调用方带 `approve=true`），宿主那个弹框回调 `confirmFn_` **只对 `ActorKind::Human` 生效且同步返回 bool** —— AI actor 走不到。而在形态 A（D13）下，那个"客户端"就是我们自己 —— 更容易把审批写成走过场，必须显式补这条通道 | `ai_api.cpp` 的双门判定（`:265`）；`commands_confirm.cpp` 的 `confirmFn_` |
| 进程内事件订阅 | 面板不是管道客户端，用不了 `events/subscribe`。要新增：**订阅线程 `WaitFor(afterSeq, &line, -1)` → 投递 UI 线程**。`Publish` 线程安全，且事件在返回值之前发布 → UI 线程收到时状态已新 | `ai_events.h`（只有 `Since`/`WaitFor`，无进程内回调） |
| 分组键 = `traceId` | **不用 `turn`** —— `RequestScope` 每请求 `++g_turn`，MCP 一次 `tools/call` 就是一个 turn，一次对话轮会是多个 turn。形态 A 下我们自己发请求，可以在一次对话轮的所有 `tools/call` 上注入**同一 traceparent** → traceId 相同自然归一组 | `ai_events.cpp` 的 `RequestScope`；AI 引擎线程要注入 `_meta.traceparent` |
| 面板宿主 | AI 面板是**同一客户区最右的一列**，由我们的布局划出来，**把 Shell 窗格区往左挤**（squeeze）。快捷键启停（`win.ai.toggle`，`CmdId::AiTogglePanel` 已预留，实现它 + 补 `assertion` `aiPanelVisible=$changed`）。宽度**复用 nav pane 的分割条机制**。**不盖在列表上**（overlay 会遮住选中项） | `ui/containers/panel`（B5）；`app_layout.cpp` 的 `RECT inner` 右侧让位 |
| 焦点契约 | 文件区态 / 面板态两态显式定义；`Esc` 三级（关菜单 → 停这一轮 → 收面板）；焦点必须能一键回文件列表 | — |
| 活动流面板 | 侧栏面板，按 `turn`/`step` 分组展示命令事件：状态点 + 中文标题 + 点分命令名 + 风险徽章（读/写/危）+ 时间；可按「全部/写/危险/失败」筛选，点行展开详情（分组 / args / 结果 / 断言 / 失败原因）。**原生自绘**，`ui_draw.h` 即可，**不用 WebView2** | `Panel` 载体 + 事件订阅（`ai_events.h` 已有） |
| 审批 UI | agent 或外部客户端请求危险命令时，面板内出现审批块（允许一次 / 本会话允许 / 拒绝）。**原生自绘**，且必须与 `aiAllowDanger` + `approve` 双门同源。**审批块是面板级固定条（横跨底部、在输入区之上），不随页签切换而消失** —— 它在面板层，不在页签层 | `CommandBus::SetConfirmFn`（已有） |
| 状态栏 chip + peek 气泡 | 收起后不是消失：状态栏 `✦ AI` chip 上方浮一张气泡（完成摘要 + 撤销 + 打开对话）。状态栏是段位图驱动的（`StatusSegmentIcon` / `SetEnabledSegments` / `HitSegment`），加这一段要动段位图映射 | 状态栏段位图 + `statusSegOrder` 设置项 |
| 会话历史 | 按 `traceId` 归档到 `events.jsonl`（已落盘），面板能回看上一轮 | `events.jsonl` 已有，`LoadFromDisk` 已有 |
| **对话散文落盘** | 新增 `<dataDir>/chat.jsonl`：一条记录 = 一个对话轮，带 `traceId` 与 `events.jsonl` 关联。`events.jsonl` 只存**事实**（谁调了什么命令、成没成），**不含 agent 的话** —— 不存的话重启后只剩"命令活动" | `ai_events.cpp` 的 `EventsPath()` 约定（同目录、追加写 + 按大小轮转、坏行跳过） |
| **agent 子进程归属** | **每窗口一个**。共用一个的话，第二个窗口的请求带着第一个窗口的上下文，而 `get_state` 类工具返回的是**当前进程**的状态 —— 对不上，模型会误推断 | D10 子决定①；MCP 发现文件本来就是多实例枚举（`ai-pipe.<pid>.txt`） |

> **Web 侧已先行落地两种看法，共用同一批 `tool` 事件**：独立的「活动流」页签用于
> 事后审计（按 `turn` 分组、筛选、展开详情）；嵌在助手回答里的「执行台账」用于
> 正常聊天流 —— 成功步骤默认折叠，失败 / 效果未达成 / 危险命令默认展开，
> 历史恢复也会重建台账。原生 Win32 自绘活动流仍按上表待做，Web 与原生是
> 同一事件源的两个载体，不互相替代。

**验收**：面板能完整显示一轮命令活动；危险命令的审批块可点、且拒绝后命令**真的没执行**
（审计日志里能看到 `ok=false`）；**AI 发起的危险命令会真的弹框问用户**
（而不是靠客户端带 `approve=true` 声明 —— 现在只有声明，没有询问）；
断网/无 agent 时面板不空、不报错；
**展开/收起时 Shell 文件列表的列宽会重排**（与 explorer 拖窗口同理），
且 details 视图宽于窗口时的**横向滚动位置不重置**（要实测，不达标就得补位置保持）；
**切到别的页签时审批块仍在**（不能只在"活动"页签里）；
**用户手动点标签栏/排序不会出现在活动流里**（`command` 事件的唯一发布点是 `ai_api.cpp:417`，
天然成立，但要作为验收项钉住）。

### 5.2 P4-b 自带模型客户端 + 纯文本流式消息区（形态 A）

> **2026-11-22 21:54 改向**：本节原为"ACP 对话"（形态 B），用户拍板改为**形态 A**
> （本工程自己发请求、自己管密钥），依据见 **D13**。D10 的 ACP 方案整段保留为
> 被否方案记录，不再执行。

| 项 | 内容 | 状态 |
|---|---|---|
| **SSE 流式解析** | `src/ai_sse.h/.cpp`：跨 chunk 残行缓冲、只按 `\n` 切（UTF-8 多字节安全）、`[DONE]` 哨兵、`event:`/注释/空行忽略、`tool_calls.arguments` 分片累积、`reasoning_content` 与正文分离、断流时交付"不完整"标记。**纯函数、零 Win32、可离网单测** | ✅ 已完成（23 条断言全绿） |
| **HTTPS 传输** | `src/ai_http.h/.cpp`：WinHTTP 会话/连接/请求三级 RAII；接收超时 180s（默认 30s 会掐断长生成）；强制 TLS1.2+；HTTP≥400 时把服务端正文一并回显（不能只给"HTTP 401"）；`Cancel` 令牌支持中途停止；`ParseBaseUrl` 只接受 `https://`（明文会泄露密钥） | ✅ 已完成（22 条断言全绿） |
| **构建接入** | `build_fast.py` 的 `LIBS` 加 `-lwinhttp`、`-lcrypt32`（系统库，仍是单 exe 零安装）；`tests/run_all.sh` 同步（否则链接期 `undefined symbol: WinHttpOpen`） | ✅ 已完成 |
| 密钥存储 | DPAPI（`CryptProtectData`）加密到 `<dataDir>/ai_key.bin`，绑定当前用户。**只在引擎线程解密、不进日志、不写 `settings.json`** | 待做（~150 行） |
| Provider 配置 | 三字段：`baseUrl` / `model` / `key`。国内主流全部兼容 OpenAI chat format（DeepSeek / DashScope / Moonshot / 智谱 / MiniMax）。**刻意不做 reasoning/thinking 参数适配** —— Pulse 为 13 家写了一套 `ThinkKind`/`ThinkWire` 枚举，我们不重复这笔账 | 待做（~300 行含设置页） |
| AI 引擎线程 | 一条常驻线程：组 `messages[]` → `PostChatStreaming` → 事件投 UI 队列。**UI 线程绝不做网络**（`WinHttpReadData` 会阻塞到有数据）。**工具循环有步数上限**（`maxSteps`，参考 AgentChat 的 12）；`usage` 跨步按**最大**合并而非求和（兼容重发累计值的网关） | 待做（~200 行） |
| 工具结果裁剪 | 每个工具结果**硬截断**（参考 AgentChat 的 8000 字符）。一次 `readdir` 大目录就能吃掉整个上下文窗口，模型随即停止推理真问题 | 待做（并入上一条） |
| 消息区（纯文本） | GDI 换行追加，**只投增量**：UI 用定时器（≈30ms）从队列取 delta 合并重绘，不每 chunk 重排全文 | 待做（~400 行） |
| 面板输入框 | 面板底部一个输入框；Enter 发送，Shift+Enter 换行。**chip 语义 = 选哪个 provider**（DeepSeek / 通义 / 智谱 / 月之暗面…），因为现在是本工程自己接模型 | 待做 |
| tool_calls → CommandBus | 收到完整 `toolCall` 后解析 `toolArgs` → 映射到命令表 → 经 `CommandBus` 执行。**危险命令仍走双门 + 审批**（D10 里与形态无关的部分继续有效）。**审批判定必须代码化**（按命令 `Effects.destructive`），不能只写在系统提示里；**只读模式下不发审批卡**，直接回 `not executed`（参考 AgentChat 的 `ctx.readonly` 处理） | 待做（~300 行） |

**为什么这一期只做纯文本**（沿用 2026-11-22 的判断，理由随形态改变而更新）：

1. **顺序**：先把"发请求 → 收流 → 显示 → 工具回调"整条链路跑通，
   而不是一上来就投 ≈2,600 行渲染层。
2. **渲染选型不阻塞链路**：A1（WebView2）与 A2（原生 DirectWrite）
   **只影响消息区怎么画**，前面 ≈1,750 行完全共用（D13 已写明）。
3. **纯文本够用**：文件管理器里的 AI 回复以「做了什么 + 结果」为主；
   真正需要渲染的是 Markdown 表格 / 代码高亮 / diff，属于 P4-c 按实测决定。

**验收**：在面板里说一句话 → 请求真的发出（抓包可见 SSE）→ 回复**逐段**出现
（首字延迟 < 2s，不是等全部生成完才显示）；说"删掉 build 里 3 天前的 obj"
→ 模型返回 `toolCall(fs.delete)` → **真的弹审批块** → 批准后命令执行、
拒绝后命令**真的没执行**（审计里 `ok=false`）；
**长回复（>8k token）滚动不卡**（这是 D13 复检条件 2 的实测点，不达标就提前做渲染层）；
断网 / key 错 / 服务端 5xx 时**显示具体原因**（不是"AI 不说话"）；
点"停止"能在 1s 内中断请求。

**明确不做**（2026-11-22）：
- 不做 ACP / 不拉起任何 agent 子进程（D13 取代 D10 的形态选择）。
- **P4-b 不引入 WebView2，也不引入 DirectWrite/Direct2D** —— 消息区纯文本，
  `LIBS` 只加 `winhttp` + `crypt32`（都是系统库）。
- 不做 13 家 provider 的思考档位适配（只做 `content` 流式 + `reasoning_content` 分离展示）。

**键已钉住的两个坑**（有单测守着，不要"顺手简化"）：
1. **只能按 `\n` 切行**，绝不能按字节长度切 —— 否则 UTF-8 多字节会被切开成乱码。
2. **`tool_calls.arguments` 是分片 JSON**，必须按 `index` 累积到 `finish_reason`
   再整体解析，中途解析永远失败。

### 5.3 P4-c 渲染层、动作粒度与 mod 资产互通

| 项 | 内容 | 前置 |
|---|---|---|
| 渲染层（待 P4-b 跑通后再定） | 消息区的富文本渲染，二选一：**原生富文本**（DirectWrite + 自写 Markdown 子集）或 **WebView2**（`ui/containers/web_panel`，B9）。**2026-11-22 决定：先不排期**，等实测看到模型确实返回 Markdown / 代码块 / diff 再定（D13 复检条件 2） | D3；原生路线需 `build_fast.py` 加 `dwrite`/`d2d1`；Web 路线需 B9 |
| `find_actions` 检索层 | agent 按词检索命令表（本地匹配 `CommandTable`），返回 id/title/args/now/enabled+why/risk/asks_user。**本工程命令表 215 条，其中 171 条 `aiExposed` 进 `tools/list`，全量列表对 agent 上下文是重消耗**。**2026-11-22 决定：先记着，等 P4-b 跑完再定是否做** | D11 |
| `run_actions` 多步编排 | agent 一次交一串步骤（≤N 步），整体作为一个撤销组。**前置：`fileundo::Stack` 现在是扁平 `vector<Item>`，没有 group 标记，要加**。同上，先记着 | D11 |
| 计划级确认 | 写动作先出方案卡 → 一次确认 → 一次撤销（Pulse 风格）。**与现有 `aiAllowDanger` + `approve` 逐命令双门是两层叠加，不是二选一**。同上，先记着 | D11 |
| 与 mod 的资产互通 | 同一套图标、同一套色令牌；`bookmarks.html` / `keymap` **不再共用**（便携模式后各写各的），需要互通时走导入导出（`bookmark_io.cpp` 已实现 Netscape 导入导出） | P2-3、P2-4、P3-4 |

---

## 6. 待决策（不阻塞当前阶段）

| 决策 | 需要定的时候 | 选项 |
|---|---|---|
| 书签并发写 | 已落地（2026-11-19） | 采用"原子替换直写"（`.tmp` + `MoveFileEx`）；两个程序同时改时**后写覆盖先写**。当前约定：不要让 mod 与 ExBarBrowser 同时编辑书签；后续可加文件锁 + 读改写 |
| 搜索结果的呈现 | P3-2 开工前 | ①自绘结果列表（简单） ②注册 NSE 虚拟文件夹（与 mod 一致，但要 Shell 扩展） |
| 多窗口 | 已落地（2026-11-21） | 单实例锁已移除：默认启动"激活优先"（转发给已有窗口开标签），`--new-window` / `EXBARBROWSER_MULTI` 显式多开 |
| 配置写入权限 | 已落地 | 只写 `<exe>\data`；键位/鼠标直接使用 `keymap.json` / `mouse.json`（数据目录已与 mod 分离，不存在撞名） |
| 版本号 1.0 判据 | P2 结束 | 当前定义：P2 全绿 + 连续一周作为默认处理器无崩溃 |

---

## 7. 里程碑验收总表

| 阶段 | 一句话判据 | 可量化指标 | 状态 |
|---|---|---|---|
| P1 | 能开、能看、能走 | 编译零警告；5 标签连续开关 20 次不崩 | ✅ |
| P1.1 | 20 个标签也不乱 | 20 标签全部可见；盘根无空箭头；`/select` 选中成功 | ✅（`/select` 仍未选中目标项） |
| P2 | 可以当默认文件夹处理器 | 重启还原几何与标签；路径复制/终端/书签/会话可用；一周无崩溃 | ✅ |
| P2.5 | 人机同源：同一操作对人可用、对机器可调 | 全部命令带 schema/效果/前置；对 AI 暴露的命令全部满足 R20 人机等价；**48 个单测**（47 通过 / 1 环境限制）+ **25 项** `check_ai_meta` 与 `check_commands`/`check_components` 全绿 | ✅ |
| P3 | 能力可发现、可编排 | 命令面板覆盖全部已实现命令（✅）；搜索 200 ms 内出结果；预览 3 种格式 | 进行中 |
| P4-a | 活动流与审批可用（原生，零新依赖） | 面板完整显示一轮命令活动；危险命令审批块可点、拒绝后命令确实没执行 | **Web UI 侧已落地**：浏览器面板新增结构化活动流视图（对话/活动流切换、按 `turn` 分组、风险徽章 read/write/danger、全部/只读/写/危险/失败筛选、点行展开参数/结果/自断言/失败原因；刷新后从 history 重建），并把同一批事件嵌成助手回答内的「执行台账」（成功折叠，失败/未达成/危险展开，历史也重建）。服务端 SSE `tool` 事件补 `command/risk/step/ts/turn` 并随历史落盘。**原生面板侧仍未开始**（`ui_draw.h` 自绘活动流），Web UI 与原生是同一批事件的两个载体，不互相替代。审批块 Web 侧此前已可点、拒绝后命令确实不执行 |
| P4-b | 窗口内能对话（纯文本流式） | 一句话发给模型 → 模型返回 `toolCall` → 命令执行 → 纯文本逐段回到面板（首字 <2s）；危险命令弹审批块，拒绝则命令真没执行；长回复滚动不卡；断网/错 key 显示具体原因 | **原生侧 SSE/HTTP 已完成**；`aichat/`（WebView2 面板路线）端到端已跑通：模型→`toolCall`→命令执行→拒绝则命令真没执行（实测文件仍在）/批准则真执行（实测文件被删）、审批判定改由命令表 `annotations.destructiveHint` 驱动。**安全边界已补**：`allowedRoots`/`denyCommands`/`dryRun` 三项此前都是"配置了没人读"的假开关（执行点只在已无调用方的 `tools.js`/`opus.js` 里），现在统一收口到 `service/guard.js`，挂在 `tools-exbar.dispatch` 这个唯一出口上，三套测试 52+14+5 全绿。**仍未做**：密钥 DPAPI 落盘（现在是本地明文 + `.gitignore`）、面板嵌进主窗口（现在还是独立窗口）、长回复性能实测 |
| P4-c | 渲染层、动作粒度与 mod 视觉一致 | （渲染层与三项动作粒度**待 P4-b 跑完后定**，本期不排期）；同一套色令牌与图标 | 未开始 |
