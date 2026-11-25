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

- 左栏按作用域筛选，右栏列 `命令 / 范围 / 键位 / 状态`（可用 / 冲突 / 系统占用 / 未实现）。
- **编辑键位**：选中一行 → 点"编辑键位…" → 直接按下新组合键（`Esc` 取消）；
  写入 `settings.json` 的 `hotkeys{setting: key}` 并立即 `Reload()`，关闭后即生效。
- **清除**：把该 `setting` 覆盖为空串（显式禁用）。
- **恢复默认**：清空全部覆盖，回到 `keymap.json`。
- 序列键不在此编辑（需要多键状态机与超时语义，直接改 `keymap.json` 的 `sequences`）。

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
