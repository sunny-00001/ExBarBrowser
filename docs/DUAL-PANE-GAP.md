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

### P0-2 `file.copy.pane` / `file.move.pane`（对侧即目标）✅（已落地）

**根本改进**：此前复制/移动必须显式给 `to` 路径（`fs.copy.to/fs.move.to`）或弹
目录选择框（`file.copy.to`），双栏的核心价值「选中即发往对侧、零路径输入」缺失。

**实现（复用，不造新轮子）**：
- 新命令 `file.copy.pane` / `file.move.pane`（无参）。【命名说明】曾用
  `fs.copy.pane`，但 check_ai_meta 第 9 项强制 `fs.*` 组「目标由参数点名、
  绝不读选中」，而这两条作用于当前选中 → 语义归属 `file.*` 组，改名才是正解
  （枚举名 `FsCopyToPane` 不改：CmdId 的值一经分配不可变，见 commands_ids.h 注释）。
- 实现落在 `app_commands.cpp`：`TransferTargets` 的「选中→转移→撤销记录→刷新」
  链路抽成 `TransferSelTo` 唯一公共件；`TransferToOtherPane` 取活动窗格选中集
  （`shellops::SelectedPaths`）+ 对侧窗格 `path()` 为目标，汇入同一链路。
- `file.move.pane` 让源消失 → `destructive=1`，自动进总线层
  `ConfirmIfDestructive` 人工确认门（人侧弹一次确认；AI 侧走危险双门）——
  正好落实调研的「移动=强确认」准则。`file.copy.pane` `destructive=0`。
- 两者 `aiExposed=true` + `needsSelection=true` + `Requirements{Req::HasSelection}`：
  无选中时前置拒绝，不猜意图。
- 键位：**F7 = 复制到对侧、F8 = 移动到对侧**（`data/keymap.json` setting
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
  实现文件：`src/app_panes_swap.cpp`（独立成文件：协同导航语义与「双栏编排」
  分属两种职责，按 AGENTS.md §3「按作用域拆」落地）。
- `view.pane.mirror`（无参）：对侧导航到活动栏当前目录（活动栏不动）。
  对齐 TC cm_MatchSrc / Ctrl+←→ 的目录版、DC Ctrl+←/→。单栏时先开双栏，
  效果即「对侧打开同目录」= `fs.open.pane` 无 path 版。
  实现复用 `OpenPathInOtherPane`（「单栏先开双栏 + 不动焦点 + push 进对侧
  历史」的前置整套已有，一行调用）。
- 两者都是导航（异步落地）→ `assertionExempt = kExemptAsyncNav`，aiExposed=true。

### P1.5 三处界面入口（发现性问题）✅（已落地）

**问题**：P0/P1 落地后，8 条双栏命令**没有任何可点的界面入口** —— 只有键位
（"记得住才用得上"）与命令面板（"知道名字才用得上"）。而双栏是"低频切换 +
高频协同"的混合体，两种入口都不匹配。

**分三处放，按使用频率与作用对象归位**（不塞进同一处）：

| 放哪 | 放什么 | 为什么是这里 |
|---|---|---|
| **分割条右键 + 双击** | 双栏开关 / 上下堆叠 / 互换两栏 / 对侧打开 / 三档比例；双击 = 比例归中 | 零新增界面面积；用户想动双栏时**视线本来就在分割条上**；`SplitBar` 已是真窗口，`CS_DBLCLKS` 与鼠标通路现成 |
| **状态栏 chip** | 复制到对侧 / 移动到对侧 | 高频协同动作需常驻可见；单栏或无选中时**自动消失**（空文本段不占位），这本身就是"为什么现在不能按"的教学 |
| **导航栏视图菜单** | 双栏 / 上下堆叠 / 分栏 30-50-70% | 与已有的"显示导航窗格 / 预览窗格"同组 —— 都是**布局姿态**，低频 |

**不做**：导航栏加常驻双栏按钮（挤压地址栏，而地址栏是导航栏最该保的）；
新开一条工具栏（chrome 已四层，收益远小于成本）。

**实现落点**：`app_pane_chips.cpp` 一个文件装下三处的"界面落点"（`paneui::`
自由函数：`WireMenu` / `FillState` / `ShowBarMenu` / `SyncChips`）。为什么是
自由函数而不是 App 成员：`app.h` 已在行数棘轮上（591 行），四条接线不值得
再涨；且它们只需 `StatusBar&` / `NavBar&` 两个显式依赖，写成参数反而更清楚。

**两个易错点（都已处理）**：
- chip 同步必须放在 `RefreshStats` 的**轻量短路之前** —— 可用性只看"是否双栏 +
  选中数"，而短路的判据里选中数只是其中一个条件；放在后面会漏掉"只改了选中项"
  的那一帧。
- 菜单行必须用 `menu.AddChecked(...)` 拿到**行号**再 `menu.Cmd(行号, ...)` ——
  `AddChecked` 返回 `int` 不是 `Menu&`，链式 `.Cmd` 编不过。

### P2（本次不做，防蔓延）

- **同步导航（NavLock）**：需要导航事件镜像 + 失同步提示 + 一键解除 + 标签
  链接字段，语义面大；事件源（`OnTabNav`）已就绪，待 P0/P1 验收后单独立项。
- **目录比较**：TC Sync Dirs 是对话框形态（枚举两侧 + 报告差异），适合本架构；
  但涉及新 UI 面（对话框/结果列表），独立立项，不塞进本轮。
- **Ctrl+Q 对侧预览**：需要自建预览控件进对侧，与现有预览窗格
  （`shell_view_preview.cpp`）关系待理，不并轮做。

## 3. 验收口径（2026-11-26 实测结果）

- 构建 ✅（`bash build.sh` 链接成功）、单测 ✅ **74/74 全过**
  （`tests/run_all.sh`：通过 74 / 无法运行 0 / 运行失败 0 / 编译失败 0）、
  三静态检查 ✅（check_commands 三方一致 241 条 / check_ai_meta 全过 /
  check_components：本轮的规则全过，余下 2 条属并行会话的 `hotkey_manager_*`）。
- P0-1：双栏下切换活动栏（Alt+F6 / 点击另一栏），分割条着色侧随动（accent 条
  在活动侧）；单栏分割条隐藏无残留。**界面观感待用户复核**（自动化不发合成
  输入，着色侧随动需人工看一眼）。
- P0-2：双栏下选中文件按 F8（移动到对侧）→ 确认门 → 文件出现在对侧、源侧
  消失，`file.undo` 可撤；按 F7（复制到对侧）源侧保留。无选中时前置拒绝
  （Req::HasSelection）。
- P1：`view.pane.swap` 后两栏路径互换且各自历史栈各多一条（可各自后退）；
  `view.pane.mirror` 后对侧=活动栏原目录、活动栏不变。
- P1.5 界面入口（需人工点一次，自动化不发合成输入）：
  - 双栏下右键分割条 → 菜单六组齐全，当前姿态（双栏/上下堆叠/比例档）打勾；
    点「互换两栏目录」两栏路径互换；双击分割条比例归中 50%。
  - 双栏下选中文件 → 状态栏出现「复制到对侧」「移动到对侧」两个 chip，tip 带
    目标目录与条数；单栏或取消选中 → 两个 chip 消失。
  - 导航栏右键 → 视图菜单里「双栏 / 上下堆叠 / 分栏 30-50-70%」五行为双栏族。

## 4. 实现落点（符号级，不写行号）

| 改动 | 文件 | 复用 |
|---|---|---|
| 活动侧着色 | `split_bar.{h,cpp}`（`SetActiveSide`/Paint 扩展）、`app_tabs.cpp`（SyncChrome 链通知）、`app_layout.cpp`（几何不变） | SplitBar 真窗口、`theme_.accent` |
| 对侧即目标 | `app_commands.cpp`（`TransferSelTo` 公共件 + `TransferToOtherPane`）、`app_commands_fs.cpp`（注册）、`app.h`（声明）、`commands_ids.h` + `commands_table.cpp`（命令表） | `shellops::TransferToFolder`、`shellops::SelectedPaths`、`fileundo`、`ConfirmIfDestructive` |
| swap/mirror | `app_panes_swap.cpp`（SwapPanes/MirrorPane）、`app_commands_panes.cpp`（注册）、命令表同上 | `NavigateInPane`、`EnsurePaneView`、`OpenPathInOtherPane` 的「单栏先开双栏」模式 |
| 键位 | `data/keymap.json`（hotkeyCopyToPane=F7 / hotkeyMoveToPane=F8） | 既有 hotkey setting 链路（`keymap.gen.h` 由 `gen_keymap.py` 生成） |

## 4b. 过程备注（并行会话协作）

本轮实现期间检测到**另一会话在同一工作树并行改动**（UIA 无障碍子系统、
图标模块拆分、弹层入口命令化）。交界处理：
- `SwapPanes/MirrorPane` 被并行会话从 `app_panes.cpp` 搬进专门的
  `app_panes_swap.cpp`（符合「一文件一职责」），本会话补写时造成短暂重复定义，
  已撤回原位版本、保留拆分版。
- 命令名被并行会话从 `fs.copy.pane` 改为 `file.copy.pane`（check_ai_meta 第 9 项
  对 fs.* 组「绝不读选中」的组级判据），keymap 与注释同步 —— 这是正确的修正，
  予以保留并记录命名理由（见 commands_ids.h 注释）。
- `keymap_test` 的 112 热键断言与 `eval-surface.json` 重签由提交统一收口。

## 5. 来源（主要）

- DOpus 官方帮助（source_and_destination / linked_tabs / toolbars）/ kancloud 中文手册
- TC：ghisler 官方论坛与功能页、cm_ 命令体系整理
- Double Commander 官方文档（shortcuts / syncdirs）
- Files（files-community）v3.6–v3.9 release notes（焦点修复教训）
- XYplorer Dual Page / xplorer² / Captain's Deck / Q-Dir 官方资料
- OFM 范式与 UX 分析（Norton Commander 传统、Krusader 手册、设计评论）
