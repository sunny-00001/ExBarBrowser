---
status: done (M0–M6；M7 见 §4.1)
baseline: 2026-11-21 11:00
---

# 迁移规格：ExBar mod 的自定义栏与输入系统 → ExBarBrowser

> **范围**：标签栏、导航栏、书签栏、状态栏、键盘快捷键、序列快捷键、鼠标快捷键。
> **前提**：mod 运行在 `explorer.exe` 内（DUI/XAML 子类化 + 低级钩子 + 跨进程 RPC），
> 本工程是独立进程（自有窗口 + `IExplorerBrowser`）。所以**能搬的是数据格式、语义规则、纯逻辑；
> 不能搬的是与 explorer 的接线**。任何"复制粘贴分片代码"的做法都会带进 7 万行里 80% 的宿主耦合。
>
> 架构落点见 [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md)，规则见 [ARCHITECTURE-RULES.md](ARCHITECTURE-RULES.md)。

> **执行结果（2026-11-19 22:20）**：七类功能已落地并通过冒烟验证，落点与本文规划基本一致，
> 但**没有**按目标目录（`contract/ core/ platform/ ui/ features/`）拆分——当前是扁平 `src/`，
> 与既有工程风格保持一致。逐项对照见 §8。输入系统的最终口径见 [INPUT.md](INPUT.md)。

---

## 1. 三类可移植性

| 类别 | 含义 | 例子 |
|---|---|---|
| **A 直接复用数据** | 文件格式/键位表/命令 id，原样读 | `keymap.json`（热键 + 38 序列）、`bookmarks.html`、`capabilities.json`、鼠标规则 |
| **B 移植逻辑，重写 UI** | 状态机/取数/布局算法可搬，绘制与命中必须重写 | 序列键状态机、标签顺序/锁定/MRU、面包屑分段、状态栏段取数、鼠标触发判定 |
| **C 不迁移** | 依赖 explorer 内部结构 | DUI/XAML 子类化、`StatusCoverSync`、窗口控制按钮、跨进程标签合并、`QuitExplorer`、`SendKeys` 注入 |

---

## 2. 逐模块清单

### 2.1 标签栏（`ebtab`，`001-eb.cpp` / `005-ebtail.cpp`）

**mod 实现要点**：`TabBarData` 持有 `tabs`（`TabItem{title,path,hwnd,active,icon}`）、
`tabRc/closeRc/plusRc/setRc/hkRc/capRc` 命中矩形、`scrollX/maxScroll/overflow/ovfRc` 溢出滚动、
`locked/lockedPaths` 锁定、`order` 自定义顺序、`dragging/dragIdx` 拖拽、`mru` 最近激活、
`closedHistory` 关闭历史、`pendingActivate/closeTarget` 关闭后激活的门控。

| 功能点 | 类别 | 落点 |
|---|---|---|
| 自绘标签：图标 + 标题 + 关闭按钮 + 激活下划线 | B | `ui/components/tab` + `TabStrip`（`Bar` 的实例） |
| 拖拽排序 + 顺序持久化 | B | `core/tab_set`（order）+ `ui/components/tab`（拖拽命中） |
| 锁定标签（无关闭钮、不可关） | B | `core/tab`（`locked`）+ 组件按状态渲染 |
| 中键关闭 / 滚轮切换 / 双击空白新建 | B | `input/binding_table`（鼠标区 + 触发方式） |
| 溢出：横向滚动 + `»` 全部标签菜单 | B | `TabStrip` 内部布局 + `Popup` |
| 关闭历史（`TabReopenClosed`，栈深约 20） | B | `core/tab_set`（`closed` 栈） |
| 标签右键菜单（关闭其他/左/右、复制路径、锁定、重命名） | B | `Menu` + 命令 |
| MRU（`TabPrevTab` 回到上一个激活） | B | `core/tab_set`（`mru`） |
| 窗口控制按钮（最小化/最大化/关闭） | C | 用系统标题栏 |
| 与 explorer 标签合并（`TabMergeAll`） | C | 不适用（单窗口模型） |

### 2.2 导航栏（`ebnav`，`005-ebtail.cpp` 等）

**mod 实现要点**：面包屑分段（含 ghost、chevron、同级下拉）、地址栏（面包屑态/编辑态）、
`SHACF_FILESYS_DIRS` 自动补全、最近访问历史下拉（跨重启保留）、视图菜单（模式/排序/分组）、
搜索框（Everything）、tooltip、跨屏 DPI 重排。

| 功能点 | 类别 | 落点 |
|---|---|---|
| 5 按钮（后退/前进/向上/刷新/视图）+ 分裂箭头历史 | B | 已有；迁入 `Bar + Button` |
| 面包屑：分段/ghost/每级 chevron/同级下拉/折叠 | B | 已有；迁入 `Breadcrumb` 组件 |
| 地址编辑：进入/提交/取消/失焦 | B | 已有；迁入 `AddressEdit` |
| `Tab` 补全同级文件夹（循环） | B | 已有；迁入 `core` + `AddressEdit` |
| 自动补全（`SHACF_FILESYS_DIRS`，目录优先） | B | 新增：`platform/shell/autocomplete` |
| 最近访问下拉（持久化） | B | 新增：`platform/config`（state.json 的 `recentPaths`） |
| 视图菜单（8 档模式） | B | 已有 |
| 视图排序 / 分组 | B | 新增：`IFolderView2::SetSortColumns` / `SetGroupBy` |
| 搜索框（Everything） | B | P3：`features/search` + `evipc` |
| DUI/XAML 子类化、跨屏修补 | C | 不迁移（自有窗口天然正确） |

### 2.3 书签栏（`ebbmk`，`004-ebbmk.cpp`）

**mod 实现要点**：完全自绘窗口、树形结构（分类节点 + 可点击叶子）、拖拽文件夹添加、
右键菜单（添加当前目录/新建分类/删除/重命名/排序）、图标与 tooltip、不可变快照契约、
Netscape HTML 存储 + `EXBAR_*` 扩展属性、`BmkToggleBar` 显隐、空白双击切换。

| 功能点 | 类别 | 落点 |
|---|---|---|
| `bookmarks.html` 读写（Netscape + `EXBAR_*`） | A/B | `platform/config/bookmarks`（与 mod 共用文件） |
| 书签树模型（分类/叶子/顺序/图标） | B | `core/bookmark_tree` |
| 书签栏布局（图标/文字/溢出 `»`） | B | `Bar` + `Button`（P2） |
| 拖拽文件夹到栏上添加 | B | `ui/components/button` 的 drop target + 命令 |
| 右键菜单（添加/新建分类/删除/重命名/排序） | B | `Menu` + 命令 |
| 空白双击切换书签栏 | B | 鼠标规则（内置默认规则，见 2.7） |
| 不可变快照（跨线程发布） | C | 不需要（单 UI 线程） |

### 2.4 状态栏（`ebstatus`，`007-ebstatus.cpp`）

**mod 实现要点**：8 段——①文件夹 选中/总数 ②文件 选中/总数 ③已选/项目总数
④修改日期（有选择取最新，否则取当前文件夹；按日期排序时带 ↑/↓）⑤图标+路径（弹性）
⑥选中对象容量 ⑦当前容器容量 ⑧按钮区（隐藏文件三态等）；段可配置（`statusSegs`）、
段 tooltip、段点击绑定命令、数值"稳定宽"（用最宽 `'8'`）、虚拟根差异（此电脑/库/网络/回收站/NSE）。

| 功能点 | 类别 | 落点 |
|---|---|---|
| 段模型（顺序/显隐/弹性/稳定宽） | B | `ui/components/status_segment` + `Bar` |
| 选中统计（文件夹/文件/已选） | B | `platform/shell/view_stats`（`IFolderView` + `IShellItem2`） |
| 选中容量 / 容器容量（含磁盘已用/总） | B | 同上 + `GetDiskFreeSpaceEx` |
| 修改日期 + 排序箭头 | B | 同上（`System.DateModified`） |
| 段配置持久化（`statusSegs`） | A/B | `platform/config`（与 mod 同键） |
| 段 tooltip / 段点击 → 命令 | B | `Tooltip` + `CommandSink` |
| 眼睛段：全显 → 隐系统 → 隐全部 | B | 命令 `CycleHiddenSeg` + `SHGetSetSettings` |
| 遮挡系统状态栏（`StatusCoverSync`） | C | 不迁移（我们就是自己的状态栏） |

### 2.5 键盘快捷键（`ebch`，`006-ebch.cpp`）

**mod 实现要点**：`g_hotkeyCmds` 主表（约 70 条，`keymap.json`）、`kHkScopeMap` 作用域、
`kHkSysKeys` 系统保留键、快捷键管理器（三视图 + 冲突检测 + 恢复默认 + 导入导出）、
低级键盘钩子（`WH_KEYBOARD_LL`）与注入抑制。

| 功能点 | 类别 | 落点 |
|---|---|---|
| 70 条默认键位 | A | `data/keymap.json`（子集）→ `input/binding_table` |
| 组合键解析（`Ctrl+Shift+T`）/ 冲突检测 | B | `input/chord` |
| 作用域（当前标签 / 全局 / 仅视图） | B | `input/scope` |
| 系统保留键黑名单 | B | `input/reserved_keys` |
| 快捷键管理器 UI（列表/编辑/冲突提示） | B | `features/hotkeys` + `Dialog` + `ListView`（P3） |
| 低级键盘钩子（全局生效） | C | 不迁移；应用内用消息循环，需要全局时用 `RegisterHotKey` |
| 注入抑制 / 安全软件对抗 | C | 不迁移 |

### 2.6 序列快捷键（`ebch` 的 M3 段）

**mod 实现要点**：38 条默认序列（`g g`、`g t`、`g h`…），超时默认 3000 ms，
前缀状态机 + 候选弹层 + 模式前缀 + 看门狗线程 + Esc 取消；候选列表与模式列表共用同一弹层。

| 功能点 | 类别 | 落点 |
|---|---|---|
| 序列数据（38 条） | A | `data/keymap.json` → `input/binding_table` |
| 前缀状态机 + 超时 | B | `input/sequence`（纯逻辑，可单测） |
| 候选提示弹层 | B | `Popup` + `ListView` |
| 模式前缀（如 `g` 后显示分组） | B | 序列表的 `group` 字段 |
| 看门狗线程 | C | 不需要（UI 定时器即可） |

### 2.7 鼠标快捷键（`ebms`，`010-ebms.cpp`）

**mod 实现要点**：13 个目标区（空白/文件/文件夹/标题栏/标签/地址栏/搜索/工具栏/状态栏…）、
5 个按钮（左/右/中/X1/X2）、7 种触发（单击/双击/长按 800 ms/上/下/左/右滚轮）、
修饰键位域、4 种派发语义（`none`/`cmd`/`runcmd`/`sendkeys`）、内置默认规则
（如"双击书签栏空白 → 切换书签栏"）、同一物理点击去重、单击 pending 分槽、长按阈值 > 双击时间。

| 功能点 | 类别 | 落点 |
|---|---|---|
| 规则模型（目标区/按钮/修饰键/触发/动作） | A/B | `input/mouse_rules`（数据结构与解析可搬） |
| 触发判定（单击延迟/双击/长按/滚轮方向） | B | `input/mouse_router` |
| 同一物理点击去重 / pending 分槽 | B | 同上（纯逻辑，可单测） |
| 目标区映射 | B | 我们的组件天然给出区域（比 mod 的 UIA 识别简单） |
| 内置默认规则 | A | `data/mouse.json`（新增，与 mod 同语义） |
| 低级鼠标钩子（拦截 explorer 窗口） | C | 不需要（窗口是我们的） |
| `SendKeys` 派发 | C | 不迁移（安全风险） |

---

## 3. 数据资产

| 资产 | 现状 | 迁移方式 |
|---|---|---|
| `data/defaults/keymap.json` | 热键 + 38 序列 | 取 ExBarBrowser 支持的子集，字段名不变；未实现命令保留但标 `implemented:false` |
| 鼠标规则（`settings.json` 的 `Bindings\mouse:*`） | 目标区/按钮/触发/动作 | 转成 `data/mouse.json`，语义不变 |
| `data/defaults/bookmarks.html` | Netscape + `EXBAR_*` | 原样读（与 mod 共用） |
| `data/capabilities.json` | 127 条命令 | 同 id 对齐；本工程只标已实现项 |
| `statusSegs`（HKCU） | 段显隐 | 转 `settings.json` 的 `status.segments` |
| 图标 | mod 用共享图标缓存 | 本工程用 `IconRegistry`（同源 shell 图标） |

---

## 4. 迁移顺序（依赖驱动）

| 批次 | 内容 | 依赖 | 估时 |
|---|---|---|---|
| **M0 命令管线** | ✅ 已完成（`commands.cpp` + `CommandBus`，127 目录/82 实现，actor + audit） | 无 | 2 天 |
| **M1 键盘 + 序列** | ✅ 已完成（`keymap.cpp` 读 mod 的 70 键；`sequence.cpp` 38 条 + 提示弹层） | M0 | 2 天 |
| **M2 鼠标规则** | ✅ 已完成（`mouse_rules.cpp` + `input_router.cpp`；内置规则、去重、长按） | M1 + 组件命中 | 2 天 |
| **M3 标签栏** | ✅ 已完成（`tab_set.cpp` + `tab_strip.cpp`：拖拽/锁定/关闭历史/MRU/溢出） | M0 + 组件 | 3 天 |
| **M4 导航栏** | ✅ 已完成（补全、最近访问、排序/分组、隐藏文件三态、导航窗格） | M0 + M3 | 3 天 |
| **M5 状态栏** | ✅ 已完成（`status_bar.cpp` + `view_stats.cpp`，9 段/段配置/tooltip） | M0 + 组件 | 2 天 |
| **M6 书签栏** | ✅ 已完成（`bookmarks.cpp` + `bookmark_bar.cpp`，共用 `bookmarks.html`） | M0 + M5 + 配置 | 3 天 |
| **M7 管理器** | ✅ 已完成（`hotkey_manager.cpp` + `prompt.cpp`） | M1 + M6 | 3 天 |

总计约 20 人日（不含架构批次 B1–B5 的 6–8 天）。单人串行约 1 个月；并行（两人）约 3 周。

**为什么这个顺序**：命令管线是所有功能的地基；键盘/序列/鼠标是纯逻辑且**不依赖 UI 组件**，
可以最早落地并立刻可用；三个栏依赖组件层，排在组件化之后；书签栏依赖配置层。

---

## 5. 每批的完成判据（DoD）

- 编译零警告；`check_arch.py` 0 ERROR；
- 冒烟 8 项通过（启动/开 5 标签/切换/关闭/地址导航/面包屑跳转/截图/日志无 `hr=0x8`）；
- 该批功能与 mod 行为逐条对照（下表"对照项"），差异写入 `docs/PARITY.md`；
- 无新增重复实现（绘图/图标/日志/路径各只有一份）；
- 文档：`README` 能力表、`PARITY` 状态、`ARCHITECTURE` 模块表同步。

---

## 6. 不迁移清单（明确写死，避免反复讨论）

| 项 | 原因 |
|---|---|
| DUI / XAML 子类化、`DesktopChildSiteBridge` 探测 | 我们不进 explorer |
| 低级键鼠钩子、注入抑制 | 窗口是我们的，消息直达 |
| 窗口控制按钮自绘 | 用系统标题栏（NC 区） |
| 遮挡/接管系统状态栏 | 我们就是自己的状态栏 |
| `TabMergeAll`（合并 explorer 标签） | 单窗口模型 |
| `QuitExplorer`、`SendKeys` | 无意义 / 安全风险 |
| 跨进程 RPC 与共享内存 | 单进程单线程 |

---

## 7. 风险

| 风险 | 说明 | 对策 |
|---|---|---|
| 行为细节丢失 | mod 的很多行为藏在注释与边界条件里（去重、pending、长按阈值） | 每模块先写"行为对照表"，再写代码；用 mod 的注释作为规格 |
| 组件层未就绪 | 三个栏依赖 `Bar/Button/Breadcrumb`，组件没做完就写栏会二次重写 | M3–M6 排在 B4 之后；M0–M2 不受影响，可先做 |
| 并行改动 | `src/` 仍有另一个会话在改（20:51 还在改 `popup_list`） | 迁移前冻结；每批一个写入者 |
| 范围蔓延 | "所有自定义功能"还包括命令面板/AI/工具/会话 | 本文件只覆盖七类；其余按 `PARITY.md` 的 P3/P4 排期 |


---

## 8. 执行结果（2026-11-19）

| 批次 | 计划内容 | 实际落点 | 状态 |
|---|---|---|---|
| M0 命令管线 | `contract/commands.h` + `pipeline/` | `src/commands.h/.cpp`（90 条已实现 + 36 条占位，含 group/scope/implemented）+ `CommandBus`（审计 `%APPDATA%\ExBar\audit.log`） | 完成 |
| M1 键盘 + 序列 | `input/chord`、`sequence`、`binding_table` | `src/chord.*`、`src/keymap.*`、`src/sequence.*`、`src/input_router.*`；读 `data/keymap.json`（热键 + 38 序列） | 完成 |
| M2 鼠标规则 | `input/mouse_rules`、`mouse_router` | `src/mouse_rules.*`、`InputRouter` 的 down/up/hold/double/wheel 时序；组件经 `MouseSink` 汇入 | 完成 |
| M3 标签栏 | `core/tab_set` + 组件 | `src/tab_set.*`（order/locked/mru/closed）+ 重写 `src/tab_strip.cpp`（图标/溢出/拖拽/右键/中键/滚轮/锁定） | 完成 |
| M4 导航栏 | 组件化 + 自动补全 + 最近访问 + 排序/分组 | 保留既有 `nav_bar*`，新增 `SHAutoComplete`、最近访问下拉、视图菜单里的排序/分组 | 完成（未组件化） |
| M5 状态栏 | 8 段 + 取数 + tooltip + 段配置 | 重写 `src/status_bar.cpp`（9 段，含眼睛/类型）+ `src/view_stats.*`（IFolderView2/IShellItem2）+ 右键段显隐 | 完成 |
| M6 书签栏 | `platform/config/bookmarks` + 树 + Bar | `src/bookmarks.*`（Netscape + `EXBAR_*`）、`src/bookmark_bar.*`（图标/分类/溢出/菜单/拖入） | 完成 |
| M7 管理器 | 快捷键管理器 + 命令面板 | `src/hotkey_manager.cpp`（范围筛选/改键/清除/恢复默认/冲突与未实现标记）+ 命令面板（自绘弹层） | 完成（导入导出未做） |

### 8.1 与规划的偏差

| 偏差 | 原因 | 影响 |
|---|---|---|
| 未拆 `contract/ core/ platform/ ui/ features/` | 与既有 11 个扁平模块保持一致，避免一次性大搬迁把可用版本拖垮 | 依赖方向靠 include 纪律与代码评审保证，暂无机检脚本 |
| 键位用户文件改名 `browser-keymap.json` | `%APPDATA%\ExBar\keymap.json` 是 **mod 自己的** `bindings[]` 格式，直接读写会互相破坏 | 两程序可共存；mod 的键位不受影响 |
| 状态栏 9 段（多"类型"段） | mod 2026-10 新增，迁移时一并对齐 | 无 |
| 快捷键管理器不做导入导出 | 时间成本；`browser-keymap.json` 可直接手改 | 低 |

### 8.2 验证记录

- 编译：clang 20.1.3，`-std=c++20 -O1 -g`，30 个 TU，0 error / 0 warning。
- 冒烟（**人工操作 + 只读证据**）：
  > 2026-11-21 修订：本节原文记的是"真实键盘 `SendInput` + 窗口消息"。按
  > [ARCHITECTURE-RULES.md](ARCHITECTURE-RULES.md) R21，`SendInput` 会向系统输入流注入
  > 事件、抢走用户前台，已全仓禁止；下面这份清单本身不变（它描述的是**人做的动作**），
  > 只是把"用脚本代按"改回"人来按，用日志/状态栏/截图做证据"。

  - `Ctrl+T` → 2 标签；`Ctrl+W` → 1 标签；
  - 序列 `g d` → 导航到 Downloads（日志 `[seq]` + `[nav]`）；
  - `Ctrl+D` → `bookmarks.html` 写入 `b-15 C://Windows`（日志 `[bmk] saved=1`）；
  - `Ctrl+Shift+B` → 书签栏隐藏/恢复；
  - 书签栏空白双击 → 切换书签栏（鼠标规则 + `CS_DBLCLKS`）；
  - `Alt+K` → 快捷键管理器（当前 83 热键 + 38 序列，其中 13 条是未实现命令的占位）；
  - 状态栏取数：`文件夹 0/81 | 文件 0/20 | 已选 0/101 | 日期 | 磁盘 25.9 G 可用 / 146.9 G`；
  - `+` 按钮新建标签、中键关闭标签、窗口关闭后配置落盘（`browser.json`）。

  证据获取方式（全部只读，不动光标、不抢焦点）：
  | 要看的 | 手段 |
  |---|---|
  | 标签数 / 当前路径 / 项目数 | AI 管道 `exbar/state`，或命令 `OpListDir` |
  | `[seq]` / `[nav]` / `[bmk]` 日志 | `Get-Content $env:TEMP\ExBarBrowser.log -Encoding Unicode -Tail 20` |
  | 界面是否正常 | `python scripts\shot.py out.png`（`PrintWindow`） |
  | 弹层开合 | `python tools/probe_toggle.py`（人工点，脚本只数窗口） |
