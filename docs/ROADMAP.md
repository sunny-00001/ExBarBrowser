---
status: current
baseline: 2026-11-21 11:00
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
| **拖拽出标签栏 → 新窗口** | 不支持；**受架构阻塞**：本程序是单实例（`main.cpp` 用互斥体 + `WM_COPYDATA` 转发），`OpenPathInNewWindow` 直接 ShellExecute 同 exe 会被转发回当前窗口开标签。要真开新窗口必须走 `EXBARBROWSER_MULTI=1` 绕过转发，而那会引入"两进程同时写 settings.json、后写整体覆盖"的数据丢失风险（ROADMAP §6 待决策项） | 该项须先定多窗口模型再做，不宜在 UI 层单方面实现 | Chrome、Edge、Opus |
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

### 4.4 P3-2 搜索（待做）

- 接 `evipc`（Everything IPC）：复用 mod 的 actor 化客户端，命名管道 + 共享内存。
- UI：导航栏右侧搜索框（`NavBar` 扩展），结果以"虚拟文件夹"呈现还是"结果列表"呈现需要定
  （mod 是 NSE 虚拟文件夹 `ebda`，本工程没有 NSE，只能自绘列表）。
- 对应命令 `SearchEverything` / `OpSearch`（`FocusSearch` 依赖搜索框存在）。
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

| 项 | 内容 | 前置 |
|---|---|---|
| AI 面板 | 侧栏聊天。**接口层已完成**（`ai_api`/`ai_server`：JSON-RPC over 命名管道、89 工具、令牌 + 危险双门），缺的是窗口内对话 UI 与会话历史 | 会话存储 + 面板载体（`docs/ARCHITECTURE-TARGET.md` 的 Panel） |
| chrome Web 化 | 用 `ui/theme/tokens.css` 统一视觉；WebView2 只承载"面板"，标签栏/导航栏保持 GDI（启动速度优先） | 与 `ExBarUI` 的宿主代码复用评估 |
| 与 mod 的资产互通 | 同一套图标、同一套色令牌；`bookmarks.html` / `keymap` **不再共用**（便携模式后各写各的），需要互通时走导入导出（`bookmark_io.cpp` 已实现 Netscape 导入导出） | P2-3、P2-4、P3-4 |

**明确不做**：把整个窗口 Web 化（一个空白 WebView2 就 260 MB，见 `ExBar换栈总体方案.md` §4.3；
本工程的价值之一就是轻）。

---

## 6. 待决策（不阻塞当前阶段）

| 决策 | 需要定的时候 | 选项 |
|---|---|---|
| 书签并发写 | 已落地（2026-11-19） | 采用"原子替换直写"（`.tmp` + `MoveFileEx`）；两个程序同时改时**后写覆盖先写**。当前约定：不要让 mod 与 ExBarBrowser 同时编辑书签；后续可加文件锁 + 读改写 |
| 搜索结果的呈现 | P3-2 开工前 | ①自绘结果列表（简单） ②注册 NSE 虚拟文件夹（与 mod 一致，但要 Shell 扩展） |
| 多窗口 | P2-1 之后 | ①始终单窗口（当前） ②`/new` 强制新窗口，互斥量按窗口计数 ③完全多实例 |
| 配置写入权限 | 已落地 | 只写 `<exe>\data`；键位/鼠标直接使用 `keymap.json` / `mouse.json`（数据目录已与 mod 分离，不存在撞名） |
| 版本号 1.0 判据 | P2 结束 | 当前定义：P2 全绿 + 连续一周作为默认处理器无崩溃 |

---

## 7. 里程碑验收总表

| 阶段 | 一句话判据 | 可量化指标 | 状态 |
|---|---|---|---|
| P1 | 能开、能看、能走 | 编译零警告；5 标签连续开关 20 次不崩 | ✅ |
| P1.1 | 20 个标签也不乱 | 20 标签全部可见；盘根无空箭头；`/select` 选中成功 | ✅（`/select` 仍未选中目标项） |
| P2 | 可以当默认文件夹处理器 | 重启还原几何与标签；路径复制/终端/书签/会话可用；一周无崩溃 | ✅ |
| P2.5 | 人机同源：同一操作对人可用、对机器可调 | 全部命令带 schema/效果/前置；对 AI 暴露的命令全部满足 R20 人机等价；32 个单测 475 项断言 + 16 组静态检查全绿 | ✅ |
| P3 | 能力可发现、可编排 | 命令面板覆盖全部已实现命令（✅）；搜索 200 ms 内出结果；预览 3 种格式 | 进行中 |
| P4 | 与 mod 视觉一致、AI 面板可用 | 同一套色令牌与图标；AI 面板能在窗口内对话（接口层已完成） | 未开始 |
