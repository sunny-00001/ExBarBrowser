# 双栏差距与改进计划（DUAL-PANE-GAP）

> 2026-11-26。依据：4 路并行调研（Directory Opus / Total Commander 权威帮助文档、
> Double Commander / Files / OneCommander / Q-Dir 等开源与现代派、OFM 交互范式与
> 用户痛点、本地代码逐行核实）。
> 本文取代 `DOPUS-NOTES.md` §3「双栏 → 不抄」的过期决策（`DOPUS-PARITY.md` 的
> 时效修正条目顺延有效）。

## 0. 结论

双栏**骨架已落地**（开关/切活动栏/比例拖动/方向/焦点真源/槽位持久化），不是空白。
对标业界后的差距集中在三类，按「复用已有基础设施、不重复造轮子」原则分级：

| 级别 | 缺口 | 对标来源 | 复用的现有设施 |
|---|---|---|---|
| P0-1 | 活动窗格无视觉指示（两栏外观完全一致） | DOpus「双栏必须显示 border 区分源/目标」；xplorer²/XYplorer 背景明暗差；Captain's Deck 多重指示 | `SplitBar::Paint` 真窗口绘制 + `theme_.accent`；`SyncChrome()` 外壳跟随链 |
| P0-2 | 无「对侧即目标」：`fs.copy.to/fs.move.to` 必须显式传 `to` | TC F5/F6（行业标准）；DOpus Ctrl+1/2；OFM「对侧即目标」核心范式 | `TransferTargets()` 全链路（选中→转移→撤销记录→刷新） |
| P1 | 无 `pane.swap`（交换两栏）/ `pane.mirror`（对侧跟随） | TC Ctrl+U（Exchange）；TC cm_MatchSrc、Ctrl+←/→；DOpus NavLock 的手动一步版 | `NavigateInPane()`（指定窗格导航）+ `nav::History` |
| P2（后续） | 同步导航（NavLock/cm_SyncChangeDir）、目录比较报告 | DOpus Linked Tabs / TC Sync Dirs | `OnTabNav` 导航事件源；TC 的对话框形态与本架构天然契合 |
| 不做 | 键盘选择体系（Space/Ins/+−*/反选）、列视图（Miller）、差异标色进视图 | OFM/TC 的自绘列表红利区 | —— 违反「视图不自绘」产品定位（`AGENTS.md` §0） |

## 1. 调研要点摘录（只留对决策有用的）

- **活动栏指示**：业界一致要求「多信号冗余」（背景明暗差 + 边框/标题染色）；
  非活动栏**降饱和但保持可交互**，禁止禁用态灰化。DOpus 源栏标题染蓝/目标栏灰，
  且官方文档明确「双栏必须显示 border，单栏可关」。
- **对侧即目标**：TC F5=复制、F6=移动，目标恒为对侧当前目录，**不弹路径对话框**；
  移动比复制需要更强的确认反馈（UX 调研：move 词义混淆是新手事故源）。
- **每栏独立标签栈**是业界默认（TC/DOpus/XYplorer/xplorer²），本项目「每标签 2 窗格」
  与 Files 模型（双窗格是标签内部布局）一致，是合法选择，不必改成每栏独立标签条。
- **焦点是双栏最大工程坑**（Files v3.9.x 连发多个焦点修复）；本项目已有
  `FocusedPaneIndex` 唯一真源 + EVENT_OBJECT_FOCUS 观察，起点正确。
- **同步导航必须可一键解除**（DOpus 论坛抱怨：失控感）；比较功能业界共识是
  「对话框报告差异」而非把标色塞进系统视图 —— 后者在本架构下不可行。

## 2. 改进项（按优先级实现）

### P0-1 活动窗格视觉指示 ✅（已落地）

**根本改进**：双栏的「活动窗格」此前只有地址栏/标签条文字跟随（弱信号），
栏本身无任何区分 —— 用户分不清命令会打到哪一栏（调研确认的新手第一困惑）。

**实现（复用，不造新轮子）**：给双栏分割条（`panesBar_`，已是真窗口、已有
`SplitBar::Paint` 双缓冲绘制）新增「活动侧着色」：沿分割线的**活动侧**画一条
`theme_.accent` 纵深条（约 2px），活动侧切换时由 `App::SyncChrome()` 链通知
`SplitBar::SetActiveSide()` → 局部重绘。不新增颜色令牌（accent 已是「强调」唯一
取值），不新增窗口、不新增第二套绘制。

- 语义：分割线两侧各属于一栏；活动侧着 accent =「命令/导航打到这一栏」。
- 与悬停/拖动态并存：悬停仍是中线变粗 accent（交互提示），活动侧条是常驻
  状态提示，二者不冲突。
- 单栏时分割条整条隐藏，无此问题。

**键位现状备注**：切活动栏仍是 Alt+F6（`view.pane.next`）；F6 维持
`win.focus.next`（焦点循环）不变 —— 业界 Tab 切栏与本项目键位表已满冲突，
不迁键（避免破坏现有肌肉记忆），靠常驻着色补识别性。

### P0-2 `fs.copy.pane` / `fs.move.pane`（对侧即目标）✅（已落地）

**根本改进**：此前复制/移动必须显式给 `to` 路径（`fs.copy.to/fs.move.to`）或弹
目录选择框（`file.copy.to`），双栏的核心价值「选中即发往对侧、零路径输入」缺失。

**实现（复用，不造新轮子）**：
- 新命令 `fs.copy.pane` / `fs.move.pane`（无参），实现落在 `TransferTargets` 同一
  TU 的相邻函数：取活动窗格选中集（`shellops::SelectedPaths`）+ 对侧窗格
  `path()` 为目标，直接调 `shellops::TransferToFolder`，撤销记录走既有
  `fileundo` 链路（与 `TransferTargets` 同一份写法，抽公共件 `TransferSelTo`）。
- `fs.move.pane` 让源消失 → `destructive=1`，自动进总线层
  `ConfirmIfDestructive` 人工确认门（人侧弹一次确认；AI 侧走危险双门）——
  正好落实调研的「移动=强确认」准则。
- `fs.copy.pane` `destructive=0`，不打扰。
- 两者 aiExposed=true：AI 拿到「把当前选中复制/移动到另一窗格」的确定式入口。
- 键位：**F7 = 复制到对侧、F8 = 移动到对侧**（`data/keymap.json` 新增 setting
  `hotkeyCopyToPane`/`hotkeyMoveToPane`）。TC 的 F5 在本项目已被
  `NavRefresh` 占用（`keymap.cpp` 系统默认键表兜底）、F6 被焦点循环占用，
  均不迁键；F7/F8 裸键空闲（序列 `f 7`/`f 8` 是两段序列，与裸功能键不同 chord）。
- 单栏时执行：与 `fs.open.pane` 同语义 —— 先自动开双栏再转移（OFM 语义：
  双栏是对侧即目标的前置；用户体验为「一按就出现对侧并复制过去」）。

### P1 `view.pane.swap` / `view.pane.mirror` ✅（已落地）

- `view.pane.swap`（无参）：两栏目录互换（活动栏 ⇄ 非活动栏各自导航到对方
  的当前目录）。对齐 TC Ctrl+U（cm_Exchange）。实现复用 `NavigateInPane`
  （指定窗格导航），两条导航各自进自己栏的历史栈。单栏时先开双栏
  （此时两栏同目录，交换后仍同目录 —— 合法幂等）。
- `view.pane.mirror`（无参）：对侧导航到活动栏当前目录（活动栏不动）。
  对齐 TC cm_MatchSrc / Ctrl+←→ 的目录版、DC Ctrl+←/→。单栏时先开双栏，
  效果即「对侧打开同目录」= `fs.open.pane` 无 path 版。
- 两者都是导航（异步落地）→ `assertionExempt = kExemptAsyncNav`，aiExposed=true。

### P2（本次不做，防蔓延）

- **同步导航（NavLock）**：需要导航事件镜像 + 失同步提示 + 一键解除 + 标签
  链接字段，语义面大；事件源（`OnTabNav`）已就绪，待 P0/P1 验收后单独立项。
- **目录比较**：TC Sync Dirs 是对话框形态（枚举两侧 + 报告差异），适合本架构；
  但涉及新 UI 面（对话框/结果列表），独立立项，不塞进本轮。
- **Ctrl+Q 对侧预览**：需要自建预览控件进对侧，与现有预览窗格
  （`shell_view_preview.cpp`）关系待理，不并轮做。

## 3. 验收口径

- 构建过、`tests/run_all.sh` 全绿、三静态检查（check_commands / check_ai_meta /
  check_components）全绿。
- P0-1：双栏下切换活动栏（Alt+F6 / 点击另一栏），分割条着色侧随动；
  单栏/收窄时无残留。
- P0-2：双栏下选中文件按 F7，确认后文件出现在对侧、源侧消失，且
  `file.undo` 可撤；按 F5 复制，源侧保留。
- P1：`view.pane.swap` 后两栏路径互换且各自历史栈各多一条；
  `view.pane.mirror` 后对侧=活动栏原目录、活动栏不变。

## 4. 实现落点（符号级，不写行号）

| 改动 | 文件 | 复用 |
|---|---|---|
| 活动侧着色 | `split_bar.{h,cpp}`（SetActiviveSide/Paint 扩展）、`app_panes.cpp`（SyncChrome 链通知）、`app_layout.cpp`（几何不变） | SplitBar 真窗口、`theme_.accent` |
| 对侧即目标 | `app_commands_fs.cpp`（注册）、`app.h`（声明 TransferSelTo）、`commands_ids.h` + `commands_table.cpp`（命令表）、`commands_table.h`（若需共享 schema 常量） | `shellops::TransferToFolder`、`shellops::SelectedPaths`、`fileundo`、`ConfirmIfDestructive` |
| swap/mirror | `app_panes.cpp`（SwapPanes/MirrorPane）、`app_commands_panes.cpp`（注册）、命令表同上 | `NavigateInPane`、`EnsurePaneView`、`OpenPathInOtherPane` 的「单栏先开双栏」模式 |
| 键位 | `data/keymap.json`（新增 4 行：F5/F7 + 2 条序列键若有合适位） | 既有 hotkey setting 链路（`keymap.gen.h` 由 `gen_keymap.py` 生成） |

## 5. 来源（主要）

- DOpus 官方帮助（source_and_destination / linked_tabs / toolbars）/ kancloud 中文手册
- TC：ghisler 官方论坛与功能页、cm_ 命令体系整理
- Double Commander 官方文档（shortcuts / syncdirs）
- Files（files-community）v3.6–v3.9 release notes（焦点修复教训）
- XYplorer Dual Page / xplorer² / Captain's Deck / Q-Dir 官方资料
- OFM 范式与 UX 分析（Norton Commander 传统、Krusader 手册、设计评论）
