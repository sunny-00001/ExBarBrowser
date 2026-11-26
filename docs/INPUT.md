---
status: current
baseline: 2026-11-21 11:00
---

# 输入端：键盘 / 序列 / 鼠标

> 本文描述 ExBarBrowser 当前生效的输入系统（`src/input_router.*`、`src/chord.*`、
> `src/keymap.*`、`src/sequence.*`、`src/mouse_rules.*`）与它读写的数据文件。
> 目标形态见 [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md) §4，迁移规格见
> [MIGRATION-PLAN.md](MIGRATION-PLAN.md) §2.5–2.7。
>
> **本文讲"人怎么输入"，不讲"脚本怎么代输入"。** 自动化一律走 `CommandBus` / AI 管道，
> 禁止模拟鼠标键盘（含 `PostMessage` 投 `WM_LBUTTON*`/`WM_KEY*`）—— 规则 R21，
> 见 [ARCHITECTURE-RULES.md §3.1](ARCHITECTURE-RULES.md)。

---

## 1. 一条主线

```
键盘消息 ─┐
序列键   ─┼─> InputRouter ──> CommandBus::Invoke(name, arg, actor) ──> 命令处理者（App）
鼠标手势 ─┘                          │
                                     └─ audit.log（actor + 命令 + 结果）
```

- **只发命令**：InputRouter 不改任何业务状态；所有动作都在 `App::RegisterCommands()`
  注册到 `CommandBus`（`src/commands.cpp` 是命令目录，id/name/group/scope/implemented）。
- **同一命令多入口**：快捷键、序列、鼠标、菜单、命令面板都走 `InvokeByName`，
  所以"能点"就"能绑键"，不存在第二套动作实现。
- **菜单右端的键位自动派生**：菜单行只声明 `CmdId`（带参命令再带内联参数，如
  `tab.color:3` / `view.mode:3`），`Show()` 时从当前键位表解析文案；新增或改键后
  所有声明该命令的菜单行自动更新，调用点不写键名。带必填参数却没有内联参数的
  键一律不显示（按下走空参，做不出那一行）。
- **未实现命令保留在目录里**（`implemented=false`）：管理器里显示"未实现"，
  不会出现"键位静默消失"。

---

## 2. 键位（`keymap.json`）

| 项 | 值 |
|---|---|
| 默认资产 | `<仓库>/data/keymap.json`（与 ExBar mod 的 `data/defaults/keymap.json` 同格式）—— 注意 `data/` 不入库，此文件是本地真实配置；内置兜底表在 `src/keymap.gen.h`，由 `tools/gen_keymap.py` 从这里生成 |
| 用户文件 | `data\keymap.json`（直接编辑；缺失时用内置默认键位） |
| 规模 | **104 条热键 + 169 条序列**（2026-11-23 实测）。序列数从 207 降到 169 是**修缺陷**：那 38 条绑的是"有必填参数又没带内联参数"的命令，按下去必然失败，其中三条还会静默改状态（见 §3）。其中若干条热键指向 `implemented=false` 的命令（占位，`check_commands.py` 会列出），按下无反应是**预期**，不是 bug |
| 结构 | `hotkeys[] {setting, command, key}`；`sequences[] {seq, command}` |
| 解析 | `Ctrl+Shift+T` / `Alt+←` / `F5` / `Alt+P`（预览窗格）/ `Shift+Left` / `Ctrl+Shift+1` |
| 生效顺序 | 用户覆盖 → 文件默认键 → mod 的系统原生键参考表（`keymap.cpp` 的 `SystemKeyFor`） |
| 冲突 | 先到先得（与 mod 的 `orderDispatch` 同口径）；管理器把重复项标"冲突" |

> 规模数字有单测钉着：`tests/keymap_test.cpp` 的 `kExpectedHotkeys` / `kExpectedSequences`。
> 加键位后必须同步改那两个常量，否则该单测以退出码 3 失败（这条以前是硬编码 70，
> 实际早已 83，长期红着没人跑到）。

**作用域**（`CommandDef::scope`）目前用于两件事：

1. 管理器左栏筛选（标签栏 / 导航栏 / 书签栏 / 文件视图 / 文件操作 / 系统对话框 / 窗口 / 全局）。
2. **裸键保护**：`标签栏` 作用域的无修饰键（`Shift+←/→/Home/End`）只在焦点位于标签栏时生效，
   否则会抢走文件视图的 Shift 选择；焦点在 `EDIT` 时，`Ctrl+C/V/X/A/Z`、方向键、Delete 一律让给编辑框。

系统保留键（`Alt+F4`、`Win+*` 等）不参与绑定，管理器标"系统占用"。

---

## 3. 序列键

- 数据：`sequences[]`（**169 条**）。前缀按命令组分配：`b`书签 `c`配置 `d`开发 `e`外部 `f`文件 `g`前往 `n`导航 `p`路径 `q`搜索 `s`会话 `t`标签 `v`视图 `w`窗口 `x`关闭族 `y`复制族 `z`工具。
- **序列只绑无必填参数的命令。** 序列的派发路径是 `FeedSequence → InvokeByNameLegacy(cmd, L"") → ArgsFromLegacyArg(def, "") → Args::Empty()` —— 参数**恒为空**，所以带必填参数的命令绑到序列上，每按一次都注定失败。更坏的是有几条在空参下会**静默改状态**（`tab.color` → 无颜色、`win.move` → 窗口挪到左上角、`view.pane.split` → 比例重置 50%）：那不是"按了没反应"，是"按了做了别的事"。
  - 合法形态有两种：无必填参数，或命令规格带内联参数（`view.mode:3` / `ViewZoom:-1`）。
  - 2026-11-23 删掉了 38 条违例（207 → 169）。带参命令的正确入口是命令面板（按 schema 追问参数）或界面菜单，不是序列。
  - **防复发是机械的**：`check_commands.py` 规则 C4 从 `argsSchema` 的 `required` 判定，命中即报错 —— 新增命令时不需要有人记得来登记。
- 模式总览弹窗：在弹窗外按 `/ ! # @ $ & % ^ * ? < > ~` 或 `Ctrl+Shift+P` 唤起（`src/sequence_overlay.cpp`），打字过滤、↑↓ 导航、回车派发、Esc 收起。模式：`/`快速跳转 `#`书签 `@`标签 `&`历史 `<`后退 `>`前进 `~`当前目录 `$`搜索 `%`路径 `!`命令（全部已实现命令）`?`帮助。
- 状态机：`SequenceMachine`（纯逻辑，可单测）
  - 前缀匹配多条 → pending，右下角弹候选（`SequenceHint`，无激活、置顶）。
  - 命中且唯一 → 立即执行；`exact` 且仍有更长候选 → 等下一键（`x l` / `x l a`）。
  - 前缀后按错键 → 整体取消（不回落成该键自己的命令）。
  - 超时：`seqTimeoutMs`（默认 3000，200–3000），`Esc` 立即取消。
- 序列在编辑框里不启动（否则输入 `g`/`x`/`y` 会被当前缀）。

---

## 4. 鼠标规则（`mouse.json`）

| 项 | 值 |
|---|---|
| 默认资产 | `<仓库>/data/mouse.json`（**不入库**，本地真实配置；缺失时用 `mouse_rules.cpp` 内置默认规则） |
| 用户文件 | `data\mouse.json`（直接编辑；缺失时用内置默认规则） |
| 字段 | `target` / `button` / `trigger` / `mods` / `action` / `command` / `hotkey` |
| target | `unknown,empty,file,folder,tab,tab_close,new_tab,bookmark,bookmark_folder,bookmark_empty,nav,tree,address,search,toolbar,status,status_seg,view,title,overflow,group,column,preview` |
| button | `left,right,middle,x1,x2` |
| trigger | `single,double,hold,wheel_up,wheel_down,wheel_left,wheel_right` |
| mods | `1=Ctrl 2=Shift 4=Alt` |
| action | `command`（已实现）/ `none`（吞默认）/ `runcmd`、`sendkeys`（未实现，安全考虑） |

**时序语义**（`InputRouter::OnMouseDown/Up`）：

- 只有存在 `double` 规则时才延迟单击（`GetDoubleClickTime()`），否则单击立即派发。
- 长按阈值 = `max(holdDelayMs, 双击时间 + 50)`；拖拽超过系统拖拽阈值即取消长按。
- 同一物理点击只派发一次；双击命中后清空 pending。
- **DOWN/UP 必须报同一个目标**。跨目标时不继承 DOWN 的 claim，否则组件 UP 的默认行为
  会被吞掉（真实症状：点标签栏 `+` 没反应、双击才新建）。Shell 的 `MT_UNKNOWN` 是例外：
  DirectUI 的 UP 不判目标，按"沿用 pending"处理。
- 双击的尾随 UP（DOWN → UP → DBLCLK → UP）不是新的单击。标签栏用 `suppressDblUp_`
  吞掉它的默认选择；否则关闭消息之后再选中旧下标，会误选"顶上来"的标签。
- 组件（标签栏 / 书签栏 / 状态栏）在命中后把 `(target, button, mods, screen)` 交给
  `MouseSink`；返回 `true` 表示规则已接管，组件跳过默认行为。
- **Shell 文件视图**由 `src/shell_input.{h,cpp}` 接同一条 `MouseSink`：
  - Win10 `SysListView32` 用 `LVM_SUBITEMHITTEST`；Win11 DirectUI 用 UIA
    `ElementFromPoint` + 有限祖先上溯（≤6 层），判定 `empty/file/folder/group/column/tree`。
  - 子类挂在 `SHELLDLL_DefView` / `SysListView32` / `NamespaceTreeControl\SysTreeView32`，
    导航重建视图时由 `ShellView::OnViewCreated` 重装。
  - **存在性早退**：该按键/滚轮没有任何规则时，不跑 UIA（跨进程 RPC）——右键不配置规则
    就不会产生 UIA 开销；单次探测内失败会立即重试，连续 3 次探测失败才进入 500ms 冷却，
    避免首次 UIA 惰性启动吃掉第一次双击。
  - DirectUI 的点击只经 `WM_PARENTNOTIFY` 上抛，`DOWN` 也走同一套 UIA/ListView 命中测试；
    `UP` 不判目标，沿用 `InputRouter` 的 pending 目标。
  - 同窗口同键 50ms 去重；双击必须落在系统双击矩形内，并做防三击；左键第一次按下
    始终放行，保证"双击文件夹规则"不破坏第一击选中。

内置默认规则（即使配置文件为空也生效）：

| 目标区 | 手势 | 命令 |
|---|---|---|
| 文件列表空白 | 左键双击 | `NavUp` |
| 书签栏空白 | 左键双击 | `BmkToggleBar` |
| 状态栏空白 | 左键双击 | `PathCopyDir` |
| 空白/标签/导航/状态/预览等区（见 `kSideTargets`） | 侧键 X1 | `NavBack`（后退） |
| 同上 | 侧键 X2 | `NavForward`（前进） |

> 侧键默认不铺在 `file`/`folder` 项上：这两类用户可能想绑定别的动作（如"侧键=关闭"），
> 默认只覆盖无争议的后退/前进区域。要在文件项上用侧键前进/后退，在 `mouse.json` 里加一条
> `{target:file, button:x1/x2, trigger:single, action:command, command:NavBack/NavForward}` 即可。
> 四个自绘栏（标签/导航/书签/状态）与文件视图各自把 `WM_XBUTTONDOWN/UP` 的 wParam
> 经 `XButtonFromWParam` 解析成 `MBTN_X1/X2`，口径一致，侧键规则在所有区域都可达。
> 系统菜单（标题栏右键 / Alt+Space）另有「视图菜单/命令面板/首选项/快捷键管理器/退出」入口，
> 全部转投命令总线，与导航栏按钮同源。

> 不装低级鼠标钩子：窗口是我们自己的，消息直达。`SendKeys` 不迁移（安全风险）。

---

## 5. 快捷键管理器（`Alt+K`）

自绘三栏面板（**非模态**，改完可以直接回主窗口按键试）：

| 区域 | 内容 |
|---|---|
| 顶 | **通栏工具栏**：改键 / 清除 / 还原 ｜ 撤销 / 重做 ｜ 复制 id / 导出 / 导入，**右端是搜索框 + 录制按钮**。禁用态、悬停态、主操作常驻强调色 |
| 左 | 前两组是**表切换**（组合键 / 序列键），其后是筛选（已自定义 / 有冲突 / 各作用域）。项与计数**从数据长出**，不是写死的数组 |
| 中 | `命令 / 范围 / 键位 / 状态`。点列标题排序（同列再点反向），排序列标题变强调色并带箭头；键位画成键帽芯片，自定义项芯片描边为强调色 |
| 右 | 选中项的详情：命令 id、当前键位、来源（默认 / 已自定义 / 已解绑）、默认键、冲突对方、四个动作按钮 |
| 底 | 全局统计 + 当前提示（捕获/录制期间显示实时状态；悬停工具栏按钮时显示该按钮说明） |

**行高统一 28px**：中栏行、左栏筛选项、表头行、右栏键位芯片都是同一个值
（`kRowH96 = 28`）。以前是 30 / 32 / 26 三种，同一屏里三种行距，眼睛扫下来会觉得
列表是歪的。

**序列键与组合键分两张表**（左栏前两组切换）。这两类东西性质根本不同：
组合键是单个 chord、可在这里改键/清除/还原；序列键是多键状态机（前缀 + 超时 +
唯一匹配）、**整表只读**。混在一张表里时，112 条可编辑的行被 169 条只读的行埋掉，
用户滚半天点中一条才发现按钮是灰的。分表之后组合键表的每一行都可编辑。
`group_` 与 `tableType_` 必须一致，所以只允许通过 `SetGroup()` 改。

**搜索框在工具栏右端**：它原来独占中栏顶部一整行（~34px），而那一行只放了一个
输入框。搬到工具栏后列表多出整整一行高度，且"找到它 → 改它"落在同一条视觉带上。
窄窗口下（剩余宽度 < 120px）搜索框整个收起，动作按钮永远优先。

- **搜索**：命令名、命令 id、作用域、键位文本都能匹配。想找"哪些命令绑在 Ctrl+Shift 上"，
  直接敲 `ctrl+shift`。
- **录制筛选**（搜索框右侧键盘按钮，`Alt+K` 之外的第二条路）：进入后按任意组合键，
  直接按键位筛选 —— 与 VS Code 的 Record Keys 同一手法。

**序列键已独立成表**（见上）：不再需要"沉底"这种补丁。原先的沉底是权宜之计 ——
它只解决了"首屏第一行不可编辑"，没解决"滚到一半全是灰的"。

**多选**：`Ctrl+点击` 增减、`Shift+点击` 连选、`Ctrl+A` 全选可见行。多选时工具栏动作
作用于全部选中项；光标行仍用 accent 左边条，其余多选行用淡色条 —— 多选里也分得清"在看哪一行"。

**右键菜单**（行上 / `Shift+F10`）：改键、清除、还原、复制命令 id、在命令面板中打开、撤销、重做。
菜单里的每一条都调同一个成员函数，与工具栏/键盘走同一条路径 —— 不存在"菜单里能做的、
按钮上做不到"这种分裂。
- **编辑键位**：选中一行 → 回车 / 双击 / 点键位芯片 / 点右栏"编辑键位…" → 按下新组合键
  （`Esc` 取消）。捕获期间实时回显已按住的修饰键（`Ctrl+Shift+…`）。
  写入 `settings.json` 的 `hotkeys{setting: key}` 并立即 `Reload()`。
  **系统占用键与冲突键都不会静默写下去**：系统键直接拒绝并说明原因；冲突键要**再按一次**
  才确认（本程序的规则是先到先得，绑上去会让**另一条**永久按不出来）。
- **清除**（按钮 / `Delete`）：把该 `setting` 写成解绑哨兵 `-`（**显式解绑**，不会自己
  回落到默认键）。
- **还原此项默认**（按钮 / `F2`）：删掉该 `setting` 的覆盖项，回落到 `keymap.json` /
  系统原生键。与"清除"是两件事，界面上的来源列会跟着变。
- **还原全部默认**（两步确认，3 秒内不确认自动取消）：清空全部覆盖。不用系统消息框
  ——那是模态浅色框，深色主题下刺眼。
- 序列键只读：它们要改状态机（多键 + 超时 + 唯一匹配），请改 `data/keymap.json`
  的 `sequences`；面板会把四个动作按钮灰掉并写明去处。行标题也压暗一档，一眼可辨。

**撤销 / 重做**（`Ctrl+Z` / `Ctrl+Shift+Z` / `Ctrl+Y`，工具栏也有）：改键、清除、还原、
导入都会入栈，最多 100 步。撤销记的是"改动前是什么"——原本没有覆盖项就记空，
回放时**删覆盖项**（回落到默认），而不是把默认键固化成一条覆盖项。
「还原全部默认」会清空撤销栈（它指向的覆盖项都没了，回放无意义）。

**复制 id**（`Ctrl+C` / 工具栏）：把选中行的命令 id 复制到剪贴板，可直接粘进
`hotkey.set` 的 `command` 参数或命令面板。

**导出 / 导入**（工具栏）：格式与 `settings.json` 的 `hotkeys` 段同构
（`{"setting": "key", ...}`），已解绑导出成哨兵 `-`。导入时未知 setting 跳过并
**在状态栏报出跳过了几条** —— 静默丢弃最糟，用户会以为"导进去了"。

**AI / 命令面板改键时面板会自己跟上**：`hotkey.set` / `hotkey.clear` / `hotkey.reset`
三条命令执行完都会通知面板重读，所以不会出现"AI 改了键、面板还显示旧键"。

---

## 6. 配置文件

| 文件 | 用途 |
|---|---|
| `data\settings.json` | 宿主偏好与策略（栏显隐、序列参数、主题、AI 开关、超时…） |
| `state\*.json` | 程序接管的高频状态（会话/最近访问/标签/窗口几何/搜索历史/热键覆盖） |
| `data\bookmarks.html` | 书签（Netscape + `EXBAR_*`）；直接编辑该文件 |
| `data\keymap.json` | 键位（直接编辑该文件） |
| `data\mouse.json` | 鼠标规则（直接编辑该文件） |
| `logs\audit.log` / `logs\audit.jsonl` | 命令审计（actor / 命令 / 参数 / 结果），排障用 |

---

## 7. 排查

| 现象 | 看哪里 |
|---|---|
| 快捷键不生效 | `%TEMP%\ExBarBrowser.log` 的 `[key] setting -> Command`；没有该行说明键位没绑上（查冲突/未实现/作用域） |
| 序列不触发 | 日志 `[seq] token=… prefix=… result=…`；`result=0` 表示无匹配，`1` 表示等待下一键，`2` 表示命中 |
| 鼠标规则不触发 | `audit.log` 是否有 `mouse` actor；没有说明组件没把事件交给 sink 或 target 不匹配。文件视图先看日志 `[shell-input] install ...` 是否成功挂到 `def/list`，再看 `[shell-input] DOWN ... target=...` 是否把该点判成 `empty` |
| 书签没写盘 | 日志 `[bmk] add path=… id=… saved=…`；`saved=0` 查文件权限 |
