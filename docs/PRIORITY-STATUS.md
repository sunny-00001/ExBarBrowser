---
status: current
baseline: 2026-11-24
method: 实跑核对——CHANGELOG 修复记录 + tools/check_commands.py + src 现状 grep + agentctl tools(173 条)；
        与 docs/COMPETITIVE-GAP-2026-11-21.md 的原始 E/M 编号逐条对账
---

# 缺口优先级与真实状态（2026-11-24 复核）

> `COMPETITIVE-GAP-2026-11-21.md` 列了 E1–E8 / M1–M8，但那是**11-21 快照**。
> 此后多轮提交已修掉大半。本文逐条核对"现在到底还缺什么"，供「按优先级依次改进」排期。
>
> **口径**：以 CHANGELOG 修复条目 + 实跑（命令表 / 静态检查 / 源码 grep）为准，
> 不采信 gap 文档的旧状态标注。数字是快照。

---

## 1. 结论：原始清单的状态重排

| 编号 | 原优先级 | 原缺口 | 复核状态 | 依据 |
|---|---|---|---|---|
| E1 | — | 编译不过 | **已修** | gap 文档自标 |
| E2 | P0 | 导航失败弹模态框 → AI 10s 超时 | **已修** | CHANGELOG「P0：模态框开着仍派发 AI 命令」→ `modal::Depth()/Scope` 拦重入 |
| E3 | P0 | 连续反斜杠解析失败 | **已修** | `src/path_util.cpp` `ToShellPath` 折叠连续分隔符、保 UNC 前缀 |
| E4 | P0 | AI 拒绝不写审计 | **已修** | CHANGELOG P0-6 审计结构化 JSONL，被拒调用也留痕 |
| E5 | P0 | `agentctl call` 丢弃 errorCode | **已修** | CHANGELOG「结构化 errorCode 10 类」+ `gap` 直接读产品 errorCode |
| E6 | — | 键位冲突无人拦 | **已修** | `tools/check_commands.py` `find_chord_conflicts`（三处冲突源全覆盖） |
| E7 | — | 17 文件超 500 行无检查 | **已修** | `check_components.py` FILE_LENGTH_MAX + 棘轮基线 |
| E8 | P0 | 文档数字过期 | **部分** | README 139 工具 vs 实跑 173；本次已订正索引，但正文数字仍是快照 |
| M1 | P1 | 无标准 MCP 端点 | **已修** | CHANGELOG「MCP over stdio：`--mcp-stdio`」+ `src/mcp_stdio.*` |
| M2 | P2 | 撤销不可用 | **已修** | `src/file_undo.*` 自建逆操作栈（上限 20，人机边界），`app_commands_files.cpp` 记 `NoteFileOp` |
| M3 | P1 | 长任务无进度/取消 | **已修** | CHANGELOG「M3：五条长任务命令作业化」工作线程 + job 命令族 |
| M4 | P2 | 显示偏好配置缺一大块 | **真缺** | 字体/字号/网格间距/图标大小仍无（但工作区有 `settings_dialog.*` 正在做，见 §3 阻断） |
| M5 | P3 | 归档/远程/剪贴板历史全无 | **真缺** | 需引第三方或自研，违反零依赖原则，先论证 |
| M6 | P2 | 命令行 `/select` 不选中文件 | **已修（本轮）** | `main.cpp` 解析选中目标 → `App::Run` → 复用 pendingSel 机制；转发路 select 作变长尾随数据，`CopyDataPayload` 零改动 |
| M7 | P2 | 目录变更无自动监听 | **已修（本轮）** | 复用 NavPane 的 `SHChangeNotifyRegister`，文件级事件回投 `WM_APP_VIEW_AUTO_REFRESH`，App 侧 300ms 去抖刷活动视图 |
| M8 | P3 | Shell 集成入口为零 | **真缺（可选）** | 与"不注入"不冲突（注册 ≠ 注入），属增量 |

---

## 2. 建议的改进顺序（只列真缺项）

| 序 | 项 | 理由 | 前置 |
|---|---|---|---|
| ~~1~~ | ~~M6~~ `/select` 选中语义 | **本轮已完成** | — |
| ~~2~~ | ~~M7~~ 文件视图自动刷新 | **本轮已完成** | — |
| 3 | **M4** 显示偏好 | 工作区已有人在做（settings 系列文件），**不要抢** | 等并行会话结束 |
| 4 | **M8** Shell 集成 | 可选增量，写 `HKCU`，不破坏定位 | 先论证 |
| 5 | **M5** 归档/远程 | 违反零依赖原则，投入产出比最低 | 先论证是否引依赖 |

---

## 2b. 结构化交互缺口（2026-11-26 独立复审，按优先级依次进行）

> 复审方法：从「全部界面回调注册点 → WndProc 消息分支 → 各组件内部菜单/弹层 →
> 输入路由 → 命令表对照」五条线独立重扫，不沿用上一批清单。
> 上一批已修：61 条菜单动作 + 24 处界面回调走总线（判据 16d/16e/16f，全部负向验证）。
> 本表是**剩余**缺口。状态列随实施更新。

### A 级：同一动作两条强度 / 人机不对等

| # | 缺口 | 位置 | 修法 | 状态 |
|---|---|---|---|---|
| A1 | 标签切换器选中直调 `Activate(id)` | `App::ShowTabSwitcher` 末尾 | 转投 `tab.activate`（index 参数改造已就绪） | ✅ 已完成 |
| A2a | `WM_APP_TAB_SELECT` 直调 `Activate` | `app_wnd.cpp` | 转投 `tab.activate`（语义完全等价） | ✅ 已完成 |
| A2b | `WM_APP_TAB_CLOSE/NEW` 直调 | `app_wnd.cpp` | `tab.close` 有 `HasMultipleTabs` 前置，走总线会改变「关最后标签=退出程序」——**需设计决策** | 暂缓 |
| A3 | 拖拽重排无命令 | `App::MoveTab(id,toIndex)` | 补 `tab.move.to {index,to}`（AI 现在无法表达「移到第 2 位」） | ✅ 已完成 |
| A4 | AI 改不了键位 | `InputRouter::SetKeyForCommand` 仅热键管理器可达；`config.set` 的 `hotkeys` 项只读 | 补 `hotkey.set/clear/reset` 命令族 | ✅ 已完成 |
| A5 | 状态栏 AI chip 直调 `ToggleAiPeek()` | `statusBar_.SetClickFn` 的 `Seg_Ai` 分支 | 新命令 `win.ai.peek {无参}`（handler=ToggleAiPeek，含 ConsumeAiUnread），chip 点击转投总线 | ✅ 已完成 |

> A1–A4 实施记录（2026-11-26）：枚举尾插 4 条（`TabMoveTo` / `HotkeySet` / `HotkeyClear` /
> `HotkeyReset`），命令表带 schema + Effects + 断言（hotkey 三条曾因第 23 项"断言与豁免
> 互斥"去掉 `kExemptConfig`，改为给 `StateSnapshot` 补 `hotkeyOverrides` 可回读字段——
> ai_api.h / ai_api_json.cpp / app_state.cpp 三处联动）；`TabMoveTo` handler 越界
> NoteFailure；`app_commands_tabs.cpp` 压注释回棘轮 516。人侧参数来源已登记
> `HUMAN_PATH`（tab.move.to=拖拽落点；hotkey.set/clear=快捷键管理器「编辑/清除」按钮，
> 与命令同一份 `SetKeyForCommand` 实现；hotkey.reset 无参走命令面板自动满足）。
> `check_ai_meta.py` 全绿（退出码 0，第 23 项 188 暴露/105 断言/83 豁免）。
>
> A5/B1/B2/B3 实施记录（同日）：枚举再尾插 2 条 `WinUndoHistory` / `WinAiPeek`
> （`win.undo.history` / `win.ai.peek`，无参、kExemptNoReadback——弹出与否不在
> StateSnapshot 里）；handler：undo history 在 `app_commands_undo.cpp`（RegisterUndoCommands
> 末尾，调 ShowUndoHistory），ai peek 在 `app_commands_misc.cpp`（调 ToggleAiPeek，
> 开合与 ConsumeAiUnread 不拆）；`app.cpp` 状态栏 SetClickFn 的 Seg_Ai/Seg_Undo 两分支
> 直调 → 转投总线（Actor::Ui()）。B3=`commands_confirm.cpp` 豁免表加 `QuitExplorer`
> （理由写在表内注释：ConfirmExit 是更高质量确认，Alt+F4 同一问）。命令基线 241→243。
>
> **判据红项归属说明（2026-11-26）**：`check_ai_meta.py` 此刻的非书签红项
> （`fs.copy.pane` / `fs.move.pane` 5 条 + UNDO_MECHANISM 2 条 + `bookmark_bar_itemmenu.cpp`
> 未登记 AREA_OF_FILE）全部来自**并行会话**的双栏协同/书签项菜单工作流，
> 不在本轮 §2b 范围。本轮新增 6 条命令的判据全部通过；全量单测 73/73（B1/A5 前基线）。
>
> **待提交**：工作区 60+ 改动混有并行会话文件（undo_history / ctxmenu / 双栏 pane 等），
> 需两条线都完工后一起提交；提交前 `dev.build/dev.test` 因 eval-surface.json 重签
> 未提交而 fail-closed，属项目自保机制，勿绕过。

### B 级：能力缺口（属产品决策）

| # | 缺口 | 说明 | 状态 |
|---|---|---|---|
| B1 | 撤销历史弹层无命令入口 | `file.undo.list` 可读列表，但「弹可视化历史并跳步」（chip 点击）人独占 | ✅ 已完成：新命令 `win.undo.history`（弹层内点行本就转投 `file.undo.to`/`file.redo`），chip 点击转投总线 |
| B2 | AI peek 的 dismiss 无命令 | `ConsumeAiUnread` 清未读人独占；undo 按钮已走总线 | ✅ 已完成（随 A5）：`win.ai.peek` 的 handler=ToggleAiPeek，开合与清未读是同一动作的两面，不拆开 |
| B3 | `win.quit` 双重确认（实测确认） | destructive=1 且不在 `kNoConfirmDestructive` 豁免表 → 命令面板执行先弹破坏性确认，`ConfirmExit()` 再弹一次 | ✅ 已完成：进豁免表（方案一）——`ConfirmExit` 是更高质量的确认（报标签数/默认「否」/受 `confirmCloseTabs` 控制），Alt+F4 走同一问；泛化确认在 `confirmCloseTabs=false` 时违背用户表态 |

### C 级：确认已合规（本轮扫过，无需动）

鼠标规则（含侧键前后退）、键盘 chord、序列执行、书签栏全部动作（含拖放 `BmkMove`）、
导航树右键（`NavRefresh` / `NavTreeExpandAll`）、设置面板全走 `config.set`、
图标滑块 `view.zoom.set`、系统菜单自定义项（`Actor::SysMenu`）、标题栏按钮（系统默认）、
序列总览 `SequenceExecute`、`file.context.menu`（Shift+F10）—— **全部在总线上**。

**实施纪律**：每改一项跑 `check_commands` / `check_ai_meta` / `check_components` + 相关单测，
新命令进表必须带 schema / Effects / 断言或带理由豁免；破坏性语义变化（A2b）不动手，留给用户决策。

---

## 3. 当前阻断（已解除，2026-11-26 晚）

- ~~并行会话冲突~~：评估面已重签（提交 `3b59658`，含 16f 判据与 undo 测试扩充），
  `check_components.py` 全绿。§2b 表格自此可实施。
- ~~`popup_list_input.cpp` 超棘轮~~：并行会话已自行压回。

---

## 4. 与 Everything 的关系

Everything 集成（`search.everything` 命令位 `implemented=false`）**不在本轮优先级内**——
用户已明确"先不管 Everything"。其显示方案的约束（海量结果会使 shell 视图 / NSE 虚拟文件夹卡死，
必须走"只做查询后端 + 定位"或"自建 `LVS_OWNERDATA` 虚拟列表"）记录在
`docs/REFERENCE-ASSETS-MAP.md` §5 与本节之前的工作结论中，待排期时再决。
