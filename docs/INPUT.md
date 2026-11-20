# 输入端：键盘 / 序列 / 鼠标

> 本文描述 ExBarBrowser 当前生效的输入系统（`src/input_router.*`、`src/chord.*`、
> `src/keymap.*`、`src/sequence.*`、`src/mouse_rules.*`）与它读写的数据文件。
> 目标形态见 [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md) §4，迁移规格见
> [MIGRATION-PLAN.md](MIGRATION-PLAN.md) §2.5–2.7。

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
- **未实现命令保留在目录里**（`implemented=false`）：管理器里显示"未实现"，
  不会出现"键位静默消失"。

---

## 2. 键位（`keymap.json`）

| 项 | 值 |
|---|---|
| 默认资产 | `<仓库>/data/keymap.json`（与 ExBar mod 的 `data/defaults/keymap.json` 同格式）—— 注意 `data/` 不入库，此文件是本地真实配置；内置兜底表在 `src/keymap.gen.h`，由 `tools/gen_keymap.py` 从这里生成 |
| 用户文件 | `data\keymap.json`（直接编辑；缺失时用内置默认键位） |
| 结构 | `hotkeys[] {setting, command, key}`；`sequences[] {seq, command}` |
| 解析 | `Ctrl+Shift+T` / `Alt+←` / `F5` / `Shift+Left` / `Ctrl+Shift+1` |
| 生效顺序 | 用户覆盖 → 文件默认键 → mod 的系统原生键参考表（`keymap.cpp` 的 `SystemKeyFor`） |
| 冲突 | 先到先得（与 mod 的 `orderDispatch` 同口径）；管理器把重复项标"冲突" |

**作用域**（`CommandDef::scope`）目前用于两件事：

1. 管理器左栏筛选（标签栏 / 导航栏 / 书签栏 / 文件视图 / 文件操作 / 系统对话框 / 窗口 / 全局）。
2. **裸键保护**：`标签栏` 作用域的无修饰键（`Shift+←/→/Home/End`）只在焦点位于标签栏时生效，
   否则会抢走文件视图的 Shift 选择；焦点在 `EDIT` 时，`Ctrl+C/V/X/A/Z`、方向键、Delete 一律让给编辑框。

系统保留键（`Alt+F4`、`Win+*` 等）不参与绑定，管理器标"系统占用"。

---

## 3. 序列键

- 数据：`sequences[]`（默认 38 条，`g g`、`g t`、`x l a`…）。
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
| `data\settings.json` | 栏显隐、序列参数、窗口几何、最近访问、会话、状态栏段、键位覆盖 |
| `data\bookmarks.html` | 书签（Netscape + `EXBAR_*`）；直接编辑该文件 |
| `data\keymap.json` | 键位（直接编辑该文件） |
| `data\mouse.json` | 鼠标规则（直接编辑该文件） |
| `data\audit.log` | 命令审计（actor / 命令 / 参数 / 结果），排障用 |

---

## 7. 排查

| 现象 | 看哪里 |
|---|---|
| 快捷键不生效 | `%TEMP%\ExBarBrowser.log` 的 `[key] setting -> Command`；没有该行说明键位没绑上（查冲突/未实现/作用域） |
| 序列不触发 | 日志 `[seq] token=… prefix=… result=…`；`result=0` 表示无匹配，`1` 表示等待下一键，`2` 表示命中 |
| 鼠标规则不触发 | `audit.log` 是否有 `mouse` actor；没有说明组件没把事件交给 sink 或 target 不匹配。文件视图先看日志 `[shell-input] install ...` 是否成功挂到 `def/list`，再看 `[shell-input] DOWN ... target=...` 是否把该点判成 `empty` |
| 书签没写盘 | 日志 `[bmk] add path=… id=… saved=…`；`saved=0` 查文件权限 |
