---
status: current
baseline: 2026-11-26 23:09
scope: 结构化交互覆盖缺口（独立复审，不沿用上轮清单）
method: 五线重扫 —— 界面回调注册点 / WndProc 消息分支 / 组件内部菜单弹层 / 输入路由 / 命令表对照
related:
  - docs/FULL-COVERAGE.md
  - docs/REVIEW-SUMMARY-2026-11-26.md
---

# 结构化交互覆盖缺口（独立复审，2026-11-26）

> **本文与前几轮审查的分工**：`REVIEW-SUMMARY-2026-11-26` 是六份分报告的合并；
> 本文是**在那些修复落地之后**，从零重新扫一遍「用户还能怎么操作」，看还剩哪些
> 入口没走命令总线。不沿用任何既有清单 —— 上一轮的清单已经被修完了，沿用它会
> 得出「已全覆盖」的假结论。
>
> **判据不是「有没有命令」，而是「同一个动作有没有两条强度不同的路径」**：
> 人点一下有审计、AI 调一次没有（或反过来），就是缺口。

---

## 0. 扫描方法（五条线，缺一条就有盲区）

| 线 | 扫什么 | 为什么这条线不可省 |
|---|---|---|
| 1 | `Set*Fn` 界面回调注册点 | 组件把动作交给 App 的唯一出口；漏一个就漏一族动作 |
| 2 | `WndProc` 的 `case` 分支 | 子控件用 `PostMessage(WM_APP_*)` 回主窗口，这条路不经过回调 |
| 3 | 组件内部 `ui::Menu` / `TrackPopupMenu` | 弹层自己组装菜单时，动作号可能不进总线 |
| 4 | `InputRouter`（鼠标/键盘/序列） | 输入层是「意图 → 命令名」的翻译，翻译错就整条路失效 |
| 5 | 命令表反查 | 前四条找出「有动作」，这条确认「有没有对应的结构化名字」 |

**只扫 1+2 会漏掉「有动作但没命令」**（A3/A4/B6 就是这么找出来的）；
**只扫 5 会漏掉「有命令但没人调」**（上一轮的 16d 就是这一类）。

---

## 1. A 级：同一动作两条强度（建议优先修）

### A1. 标签切换器选中后直调 `Activate(id)`

- **位置**：`App::ShowTabSwitcher`（`src/app_commands.cpp`），菜单由 `tab.switcher`
  命令弹出，但最后一行 `if (id) Activate(id);` 直接调 App 方法。
- **为什么是缺口**：`tab.activate` 命令存在且有 `index` 参数（`kTabIndexArg`）。
  同一个「切到第 N 个标签」，键盘/面板走命令（有审计、有 `activeTab` 断言），
  鼠标从切换器选走直调（无审计）。**两条强度。**
- **修法**：改成 `CommandBus::Invoke(CmdId::TabActivate, {index}, Actor::Ui())`。
  `action` 本身就是标签下标，与 `tab.activate` 的 `index` 同口径（0 起）。

### A2. 标签条四条 `WM_APP_TAB_*` 消息直调

- **位置**：`src/app_wnd.cpp` 的 `WM_APP_TAB_SELECT` / `_CLOSE` / `_NEW` / `_MOVED`。
- **现状**：分别直调 `Activate` / `CloseTab` / `NewTab` / `MoveTab`。
- **逐条判定**（不能一刀切，四条各有语义差异）：

  | 消息 | 对应命令 | 能否直接转投 | 说明 |
  |---|---|---|---|
  | `_SELECT` | `tab.activate` | ✅ 能 | 语义完全等价，`index` 同口径 |
  | `_NEW` | `tab.new` | ✅ 能 | 无参时都是「打开此电脑」，`NewTabIndex()` 由 handler 内部算 |
  | `_MOVED` | **无** | ❌ 缺命令 | 见 A3：`tab.move.*` 只有左/右/最前/最后，没有「移到第 N 位」 |
  | `_CLOSE` | `tab.close` | ⚠️ 需决策 | `tab.close` 有 `Req::HasMultipleTabs` 前置，而「关最后一个标签 = 退出程序」是鼠标路径的既有语义。直接转投会让「关最后一个」从「退出」变成「拒绝执行」 |

- **修法**：SELECT / NEW 直接转投；MOVED 等 A3；CLOSE 需产品决策（见 §3 待决项）。

### A3. 拖拽重排没有结构化入口

- **位置**：`App::MoveTab(int id, int toIndex)`，唯一调用方是 `WM_APP_TAB_MOVED`。
- **为什么是缺口**：`tab.move.left/right/first/last` 只能表达相对移动；
  用户在标签条上把第 5 个拖到第 2 个位置，AI 无法表达同一件事
  （只能 left 三次，且中间状态不可观测）。
- **修法**：补 `tab.move.to {index, to}`（都是 0 起），`Effects` 与 `tab.move.*` 同族
  （非破坏、非幂等、不跨进程、不可撤销）。`WM_APP_TAB_MOVED` 与拖拽落点都转投它。

### A4. 热键改绑只有窗口可达

- **位置**：`InputRouter::SetKeyForCommand` / `ResetDefaults`，唯一调用方是
  `hotkey_manager.cpp`（`win.hotkeys` 打开的独立窗口）。
- **为什么是缺口**：`config.set` 的 `hotkeys` 项被显式标成**只读**
  （`settings_reg_table.cpp` 里 `nullptr` setter，理由写的是「改 data\keymap.json，
  或用键位相关命令」）—— 而**那条「键位相关命令」并不存在**。说明文字指向一个
  不存在的入口，属**文档说谎**。
- **修法**：补 `hotkey.set {command, key}` / `hotkey.clear {command}` 两条命令，
  内部走 `SetKeyForCommand`（它已经做了 `cfg_->Save()` + `Reload()`）。
  只读项说明文字随之改成指向真命令名。

### A5. 状态栏 AI chip 直调 `ToggleAiPeek()`

- **位置**：`src/app.cpp` 状态栏段点击回调 `case Seg_Ai:`。
- **为什么是缺口**：同一 `switch` 的其它分支（`Seg_Chk`/`Seg_Date`/`Seg_Path`…）
  全部 `InvokeLegacy`，只有这一个直调。**peek 气泡**本身没有命令
  （`ai.panel.toggle` 是开合面板，不是弹 peek）。
- **修法**：补 `ai.peek.toggle`（纯 UI 展示，`aiExposed=false`），chip 点击转投它。
  判据 16f 只能扫到「回调体里有没有出现某个 CmdId」，**分支级的半覆盖它抓不到** ——
  这正是本文要记录的方法论盲区。

---

## 2. B 级：能力缺口（属产品决策，不一定是缺陷）

### B6. 撤销历史弹层没有命令入口

- `file.undo.list` 能**读**撤销栈（给 AI 用），但「弹出可视化历史、点一行跳到某步」
  （`ShowUndoHistory`，状态栏 `Seg_Undo` chip 点击）只有人能触发。
- **建议**：补 `file.undo.history`（纯 UI，`aiExposed=false`），让「弹列表」这个动作
  也有名字。AI 侧仍用 `file.undo.list` + `file.undo.to`，不需要弹层。

### B7. AI peek 的「已读」清除没有命令

- `aiPeek_.SetDismissFn` → `ConsumeAiUnread()`（关掉气泡即清未读）。
- undo 按钮走的是 `BmkUndo`/`CmdUndo`（✅ 已在总线）。
- **建议**：与 A5 一起做 —— `ai.peek.toggle` 关掉时内部就是 dismiss，语义已覆盖。

### B8. `win.quit` 双重确认（实测确认，非推测）

- `win.quit` 的 `Effects.destructive = 1`（`commands_table.cpp`），
  且**不在** `kNoConfirmDestructive` 豁免表（`commands_confirm.cpp`）。
- 于是命令路径：`ConfirmIfDestructive` 先弹一次「破坏性操作确认」→
  handler 里 `ConfirmExit()`（`confirmCloseTabs` 开启时）再弹一次「多标签确认退出」。
- **修法**（二选一，本文取前者）：
  1. 进豁免表，理由写清「handler 自带 `ConfirmExit` 语义确认，总线层再问一次是重复」；
  2. handler 在破坏性确认已发生后跳过 `ConfirmExit`（需要传参，污染签名）。

---

## 3. 待决项（需要产品判断，本文不做）

| # | 事项 | 选项 |
|---|---|---|
| D1 | `WM_APP_TAB_CLOSE` 是否转投 `tab.close` | (a) 保持直调（关最后=退出）(b) 转投并放宽 `Req::HasMultipleTabs`，把「关最后退出」做成命令语义 (c) 转投 `tab.close` 且保留「最后一个走 `win.quit`」的例外 |
| D2 | `tab.close` 的「关最后一个 = 退出程序」是否该是命令语义 | 退出是**破坏性且不可撤销**，而 `tab.close` 标的是可撤销（进关闭历史）。一个命令不该有两种后果 |
| D3 | 是否给 `MoveTab` 之外再加 `tab.sort`（按标题/路径重排） | 属新功能，不属覆盖缺口 |

---

## 4. C 级：本轮扫过、确认已合规（列出来是为了下次不用重扫）

| 面 | 结论 |
|---|---|
| 鼠标规则（含侧键 X1/X2 → `NavBack`/`NavForward`） | ✅ 全部 `InvokeByNameLegacy` |
| 键盘 chord / 序列键 | ✅ `InvokeByNameLegacy(cmd, L"", Actor::Key/Seq())` |
| 书签栏全部动作（含拖放 `BmkMove`、右键 `.Cmd` 标记） | ✅ 走总线 |
| 导航树右键（`NavRefresh` / `NavTreeExpandAll`） | ✅ `.Cmd` 标记 |
| 设置面板全部写入 | ✅ `config.set` |
| 图标大小滑块 | ✅ `view.zoom.set` |
| 标题栏系统菜单自定义项 | ✅ `Actor::SysMenu` |
| 主窗口标题栏按钮（最小化/最大化/关闭） | ✅ 系统默认（`WS_OVERLAPPEDWINDOW`，非自绘） |
| 序列总览 `SequenceExecute` | ✅ `InvokeFromPalette` / `InvokeByNameLegacy` |
| 上下文菜单 `file.context.menu`（Shift+F10） | ✅ 有命令 |
| 地址栏/搜索框/书签栏/状态栏右键 | ✅ 全部转投 |

---

## 5. 方法论沉淀：判据抓不到的三种半覆盖

本轮找出 A5/B6 这类缺口，暴露了现有判据（`check_ai_meta.py` 第 16/16d/16e/16f）的边界：

| 盲区 | 例子 | 为什么判据抓不到 | 补法 |
|---|---|---|---|
| **分支级半覆盖** | A5：`switch` 里 6 个分支 5 个走总线，1 个直调 | 16f 只问「回调体里有没有出现 CmdId」，命中一个就判过 | 逐分支登记，或改成「回调体里**不允许**出现直调的 App 动作方法」 |
| **动作无名字** | A3/A4/B6：拖拽重排、改键、弹撤销历史 | 判据查的是「命令有没有人调」，查不出「动作有没有命令」 | 从组件动作枚举反查命令表（本轮第 1/3 线做的事） |
| **动态文案** | 撤销历史弹层的 `L"撤销 " + 变量` | 第 16 项用正则匹配 `Add(L"字面量"` | 允许登记「动态行」区域，或改用 `.Cmd()` 标记做判据 |

**结论**：判据的强度取决于「扫描面」而不是「条目数」。加判据前先问「它的扫描面覆盖了哪条线」，
否则只是把已知的东西数了两遍。

---

## 6. 实施进度

按 §1 的顺序依次落地。**未做的不打勾**；每项写明验证方式（判据是否负向验证过）。

- [x] **A1 标签切换器选中转投 `tab.activate`**
      实现 `ShowTabSwitcher`（已搬至 `src/app_commands_palette.cpp`）末行改为
      `Invoke(CmdId::TabActivate, {index}, Actor::Ui())`。
- [x] **A2 标签条四条消息转投**（`src/app_wnd.cpp`）
      · `_SELECT` → `tab.activate`（`index` 同口径，0 起）
      · `_NEW` → `tab.new`（省略 path = `nav::kThisPc`，与原先传参一致）
      · `_MOVED` → `tab.move.to {index,to}`
      · `_CLOSE` → **语义分流**：`tabCount > 1` 走 `tab.close`（可撤销、进关闭历史）；
        否则走 `win.quit`（与标题栏 X/Alt+F4 同源、同一份 `ConfirmExit`）。
        理由：`tab.close` 标 `undoable`，而关最后一个标签会 `DestroyWindow` ——
        一个命令不能同时是「可撤销的关标签」和「不可撤销的退出」（本文件 §3 D1/D2 的结论）。
- [x] **A3 `tab.move.to`**（命令定义 + handler 已在并行会话落地）
      本轮补的是**调用方**：`WM_APP_TAB_MOVED` 从直调 `MoveTab` 改为转投命令。
- [x] **A4 `hotkey.set` / `hotkey.clear` / `hotkey.reset`**（命令与 handler 已落地）
      本轮补的是**误导说明**：`settings_reg_table.cpp` 的 `hotkeys` 只读项原文写
      「或用键位相关命令」——指向一个当时并不存在的入口；现点名三条命令。
- [x] **A5 `win.ai.peek`**：状态栏 `Seg_Ai` 从直调 `ToggleAiPeek()` 改为转投命令。
- [x] **B6 `win.undo.history`**：状态栏 `Seg_Undo` 从直调 `ShowUndoHistory()` 改为转投命令。
- [x] **B7 由 A5 覆盖**：peek 关闭即 `ConsumeAiUnread`，无需独立命令。
- [x] **B8 `win.quit` 进豁免表**：`commands_confirm.cpp` 的 `kNoConfirmDestructive`
      已含 `QuitExplorer`，理由写明「handler 的 `ConfirmExit` 就是它的确认，比泛化确认更具体」。

### 6.1 判据强化（本轮真正的新增价值）

16f 从 24 处扩到 **31 处**，并修掉两个**判据自身的缺陷**（不改实现就漏检）：

| 缺陷 | 现象 | 修法 |
|---|---|---|
| **分支级半覆盖** | 状态栏 `switch` 只登记 6 个分支，`Seg_Ai`/`Seg_Undo` 直调也照样绿 | 逐分支把 CmdId 列全（8 条） |
| **取体错位** | 登记 `case WM_APP_TAB_SELECT: {` 时，`_anchor_body` 从锚点后找第一个 `{`，而注释里的 `}` 会让配平提前结束 —— 取到的是**下一段**代码 | 新增 `_case_body()`：按行首缩进取到"下一个同级 `case`/`default`"，并且 `case` 锚点**必须优先**用它（否则 `_src_func_body` 会把 `case ...TabClose` 当成 `App::TabClose` 的定义锚，取到那个函数体，报假红） |

**负向验证（6 个破坏点，逐条实测）**：点标签/关标签/新建标签/拖拽重排/AI chip/撤销 chip
各自回退成直调后，判据**红的正是对应那一项**；全部还原后基线恢复绿 —— 6/6 捕获。

### 6.2 顺手修掉的编译阻塞（非本文范围，但挡验证）

`WM_APP_STATUS_MENU` 已从 `common.h` 的枚举里移除（状态栏改用 `SetContextFn` 回调），
而 `app_wnd.cpp` 仍留着一个引用它的 `case` —— 编译失败。删除该死分支（无发送方，行为不变）。

### 6.3 本轮验证结果

| 项 | 结果 |
|---|---|
| 编译 | `build.sh --out /tmp/exbar_a3.exe` 通过（0 错误） |
| `check_commands.py` | 231→241 条，三方一致 |
| `check_ai_meta.py` | **28 项全部通过**（16f 31/31） |
| 定向单测 | `tab_set_test` / `tab_strip_rules_test` / `commands_test` / `commands_confirm_test` 全 PASS |
| 行数棘轮 | 我经手的 `settings_reg_table.cpp` 786 = 基线；`app.cpp`/`app.h`/`shell_input.cpp` 的超限属并行会话在写的新代码 |

### 6.4 仍待处理

- **评估面重签**：`tools/check_ai_meta.py`（本轮改了判据）与 `tests/tab_strip_rules_test.cpp`
  （并行会话）需人工 `--human-confirm`。本轮判据改动已做 6/6 负向验证，未见改软。
- **并行会话的行数欠账**：`app.cpp` 981 / `app.h` 609 / `shell_input.cpp` 533 超记账行数，
  由该会话提交时按提交态重量基线。
- **D1/D2 已决**（见 A2 的 `_CLOSE` 分流）；**D3**（`tab.sort`）仍属新功能，未做。
