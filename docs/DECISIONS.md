---
status: current
baseline: 2026-11-22 20:25
---

# 架构决策记录（ADR）

> 格式：背景 → 决策 → 后果（正/负）→ 被否的替代方案 → 复检条件。
> **想推翻某条决策时，先读它的"复检条件"**：如果条件还没出现，说明当时的选择仍然成立。
>
> 状态取值：`accepted`（生效）、`provisional`（临时，已知要改）、`superseded`（已被取代）。

| 编号 | 决策 | 状态 |
|---|---|---|
| D1 | 独立进程，不注入 explorer | accepted |
| D2 | 用 `IExplorerBrowser` 托管真实 Shell 视图 | accepted |
| D3 | chrome 用 GDI 自绘，暂不上 WebView2 | accepted（2026-11-22 收窄 WebView2 适用范围：只承载富文本/流式；同日补第三路"原生富文本"） |
| D4 | 多实例 + 激活优先（原单实例，2026-11-21 修订） | accepted |
| D5 | P1 不做配置持久化 | **superseded（2026-11-27，P2-0 便携模式落地）** |
| D6 | 与 ExBar mod 共享设计令牌与数据格式，不共享代码 | accepted |
| D7 | 不引入构建系统 | accepted |
| D8 | 从 Windhawk 主仓独立为自包含仓库 | accepted |
| D9 | 品牌串与窗口类名收编到 `identity.h`，命名按领域 | accepted（部分待办，见 NAMING.md §7） |
| D10 | AI 面板内嵌外部 agent，走 ACP；不内置 LLM 客户端 | **superseded（2026-11-22 21:54，被 D13 取代）** |
| D11 | agent 的动作粒度：检索层、多步编排、计划级确认 | provisional（2026-11-22 记录形状与待验证项；**三项均不排期，等 P4-b 跑完再定**） |
| D12 | AI 面板的宿主：同一客户区最右一列，squeeze，快捷键启停 | accepted（2026-11-22） |
| D13 | AI 面板走**形态 A**：本工程自带模型客户端（WinHTTP + SSE），密钥进 DPAPI；不内嵌外部 agent | accepted（2026-11-22 21:54，取代 D10 的形态选择） |
| D14 | AI 面板的渲染层用 **WebView2**（`WebView2Loader.dll` 运行时动态加载），服务是**主进程的子进程**（Node），随主进程启停 | accepted（2026-11-23；**代价**：引入两个运行时依赖，与 §1.2「单 exe 零依赖」冲突，由本决策显式豁免） |
| D15 | 高频、无歧义、无破坏性的文件管理请求走**本地意图直调**，不先问模型 | accepted（2026-11-23；未命中仍走模型 + 审批门） |
| D16 | 项目知识前置：常驻知识卡 + 本地召回 + prompt cache 分段 | accepted（2026-11-23；Anthropic 走 `cache_control`，OpenAI 兼容线路抹掉内部标记） |
| D17 | 本地 HTTP 不是可信边界：token + Host/Origin + 回合快照 | accepted（2026-11-23） |
| D18 | 回合是身份、确认是事实：单调 `turnId` + 同会话单飞 + 事件归属 | accepted（2026-11-23） |
| D19 | WebView2 页面 → 宿主只走窄动作白名单，子窗口不拖整窗 | accepted（2026-11-24） |
| D20 | 撤销组先定元数据契约与纯模型，不把多步 AI 操作伪装成单步撤销 | accepted（2026-11-24） |
| D21 | 活动流与回答台账共享服务端 turnId，轮次可反查回答 | accepted（2026-11-24） |
| D22 | 重试是分叉新回合，不覆盖旧尝试；普通上下文只看活动分支 | accepted（2026-11-24） |
| D23 | 活动分支是持久选择，父链决定哪些后继回合进入上下文 | accepted（2026-11-24） |
| D24 | 面板收起不是消失：状态源独立于 chip，peek 只呈现结果（2026-11-24） | accepted |
| D25 | 未读是用户动作的结果：终态点亮，展开或消费 peek 才清（2026-11-24） | accepted |
| D26 | 工具回退只重探连接，不绕过边界（2026-11-24） | accepted |
| D27 | 页面未上报前，状态栏显示宿主原生启动态（2026-11-24） | accepted |

---

## D1 独立进程，不注入 explorer

**背景**：ExBar mod 走的是注入路线（Windhawk mod 在 `explorer.exe` 内挂钩子、改 DUI 树）。这条路线的
代价已经在主仓文档里量化过：单 TU 约 7.7 万行、UI 迭代一轮几分钟、改样式要理解未文档化的 DUI 树、
一次崩溃连带 explorer。

**决策**：ExBarBrowser 是普通 Win32 进程，自己的窗口、自己的标签栏，**不注入、不挂钩子、不注册 Shell 扩展**。

**后果（正）**

- 崩溃隔离：本程序崩了只影响自己；explorer 崩了本程序照常。
- 无 Windhawk 约束：真多文件、真命名空间、可以用标准调试器。
- 卸载干净：删 exe + 两个 HKCU 动词即可，系统里不留东西。
- 迭代快：改完编译运行，秒级看到结果，不需要重启 explorer。

**后果（负）**

- 改不了系统 explorer 窗口：用户双击文件夹仍进 explorer（除非 `-Takeover`）。
- 拿不到 explorer 的内部状态（标签、视图、选中），一切要靠 Shell API 重新取。
- 与 mod 的功能不能互相调用，只能靠共享文件（书签/会话）间接协同。

**被否的替代方案**

- *继续在 mod 里加功能*：受单 TU 与 DUI 约束，正是要逃离的东西。
- *Shell 扩展 DLL（Namespace/ContextMenu）*：进程内加载到 explorer，等于把崩溃面又带回去；
  且调试成本高。留作 P3 的可选增量（按需加载、不常驻），不作为主体。

**复检条件**：若"用户必须能在系统 explorer 窗口里看到 ExBar 的标签栏"成为硬需求，则本决策不成立——
那时应该回到 mod 路线，而不是在本工程里造跨进程窗口嵌入。

---

## D2 用 `IExplorerBrowser` 托管真实 Shell 视图

**背景**：要在自己的窗口里显示文件列表，有三条路：①自绘视图（枚举 + 画图标 + 自己做右键菜单/拖放/重命名）；
②`IExplorerBrowser`（微软文档化的"在自有窗口托管资源管理器视图"接口）；③跨进程 `SetParent` 把 explorer
窗口塞进来（WindowTabs 路线，已知焦点/白屏/卡死问题）。

**决策**：每个标签一个 `IExplorerBrowser`，用 `EBO_SHOWFRAMES` 打开 shell 原生导航窗格。

**后果（正）**

- 右键菜单、拖放、就地重命名、缩略图、排序分组、命名空间、导航树**全部免费**，且与系统行为一致。
- 不需要维护"文件列表"这个最复杂的组件。
- 新增 shell 能力（如 Win11 的新菜单）自动跟随。

**后果（负）**

- 视图的很多行为不可编程：预览窗格、复选框、视图模式切换没有公开接口。
- 依赖 shell 的内部实现细节（窗口类名、`WM_THEMECHANGED` 反应），系统升级可能破坏主题联动。
- mingw 头文件与真实 ABI 不一致，需要 vtable 补丁（见 `ARCHITECTURE.md` §10）。
- 每个标签一个浏览器实例，内存随标签线性增长。

**被否的替代方案**

- *自绘视图*：等于重写 explorer 的文件列表，工作量以人年计，且永远追不上系统行为。
- *跨进程 `SetParent`*：Raymond Chen 明确警告；焦点、z-order、白屏、宿主卡死传染，且我们要的是
  "自己的窗口"而不是"借来的窗口"。

**复检条件**：若某天必须支持"预览窗格 + 复选框 + 可编程视图模式"，而 `IPreviewHandler` 自绘仍不能满足，
才考虑自绘视图（只自绘"列表"，其余仍走 shell）。

---

## D3 chrome 用 GDI 自绘，暂不上 WebView2

**背景**：主仓的 `ExBar换栈总体方案.md` 决定把 explorer 内嵌面板搬到 WebView2（`ExBarUI.exe`），
理由是"AI 改得动、看得见"。本工程的 chrome（标签栏/导航栏/状态栏）只有几百行绘制代码。

**决策（2026-11-22 收窄 WebView2 的适用范围）**：chrome 保持 GDI 自绘 + `Theme` 令牌；
WebView2 **不承载标签栏/导航栏**，且**只承载"富文本/流式"内容**——
即 AI 面板的 agent 消息区、预览面板。

**附带的决策：面板按内容类型分流，不按"是不是面板"分流。**

| 内容类型 | 载体 | 判据 |
|---|---|---|
| 结构化数据 | **原生自绘**（`Panel` + `ListRow` + `ui_draw.h`） | 每行都是"图标 + 文本 + 徽章"这种定长格子，命中/滚动/键盘全在自己手里 |
| 富文本 / 流式 | **原生富文本**（`RichPanel`：DirectWrite 排版 + 自写 Markdown 子集）<br>**或** **WebView2**（`WebPanel`）<br>（二选一，2026-11-22 待定，见下） | 要渲染 Markdown、代码块、diff、流式增量，且内容长度不可预知 |

**2026-11-22 修正：富文本那一格不是只有 WebView2 一条路。**

原稿直接写"富文本/流式 → WebView2"，这个判断下得太早，被 `jimmgreen/pulse`
（一个在售的 Windows 文件管理器）的源码证伪：Pulse 的 AI 聊天面板**全仓
`webview` / `webview2` / `cef` 命中数为 0** —— 它**没有任何 Web 化内容**，
富文本照样做出来了，链路是：

| 环节 | Pulse 的实现 | 行数 |
|---|---|---|
| Markdown → 块 + 行内 run | `src/ui/ai_rich_text.cpp`（复用 md4c 的解析结果） | 472 |
| 排版 + 绘制 + 命中/选择 | `src/ui/ai_panel_rich.cpp`（`IDWriteTextLayout` 逐段排版 + Direct2D） | 1,460 |
| 代码高亮 | `src/ui/syntax_highlight.cpp` | 654 |
| 合计（自研部分，不含 md4c） | | **≈ 2,600** |

LumaText（`third_party/lumatext`，内含 FreeType/HarfBuzz）在 Pulse 里是
**可选加速件**（`delayimp` 延迟加载，`optional_lumatext.h`），文件不在时直接回落 DirectWrite；
`ai_rich_text.h` 的注释写得很清楚："No drawing here: ai_panel_rich.cpp lays the blocks out"。

**这不等于本项目照抄。** Pulse 的自研部分 ≈2,600 行（单文件上限 500 行 ⇒ 拆 5–6 个文件），
且它**自带 md4c**（`third_party/md4c`，BSD）—— 本项目禁第三方库，
Markdown 解析要自己写一个子集（段落/标题/列表/引用/行内 code/粗斜体/链接/代码块/表格，
估 400–600 行）。DirectWrite 是**系统库**（`dwrite.dll`，随 Windows 自带），
按 §1.2 只需在 `build_fast.py` 的 `LIBS` 加一项，不违反"单 exe 零运行时依赖"。

**本项目富文本的真实代价是三笔，不是一笔**：

1. Markdown 子集解析器（自写，≈500 行）—— 这是新增依赖里最实的一笔。
2. DirectWrite 与现有 GDI chrome 的混用：DirectWrite 不能直接往 HDC 画，
   要走 `ID2D1DCRenderTarget` 互操作，或把富文本区单独渲染到自己的位图再贴回 HDC。
   Pulse 没这问题（它整窗 Direct2D）。
3. 失去"改 CSS 秒级迭代"——本项目原生绘制要重编译。

**因此富文本载体在 2026-11-22 仍是待定项**，见 ROADMAP P4-b 的两条路线与各自的验收口径。
本决策只做一件事：**把选项从"一个"纠正成"两个"**，并记下第三份实证依据。

**附带的决策（2026-11-22）：P4-b 的消息区只做纯文本流式，渲染层后置。**
理由：ACP 客户端、stdio 桥接、审批映射这三块的正确性还没验证过，
先把链路跑通再谈渲染；且两条富文本路线各自都要一笔不小的投入
（原生 ≈2,600 行 + Markdown 子集解析器；Web = 唯一可选运行时）。
于是 **P4-b 不引入 WebView2，也不引入 DirectWrite/Direct2D** ——
`build_fast.py` 的 `LIBS` 本期不动，零新增运行时。
等 P4-b 跑通、且真的看到 agent 连发 Markdown / 代码块 / diff 时再在两条路线里选。

这条分流不只为了省内存。260 MB 那个数字是**整个 WebView2 实例**的私有内存，不是"每多一个面板加 260 MB"；
但它是**一次性**成本 —— 只要窗口里出现一个 WebView2，这 260 MB 就已经付掉了，而本项目 90% 的 AI 面板内容
（活动流、审批块、命令结果）是结构化的，恰恰是最不该 Web 化的那一类。

**为什么"AI 面板整体 Web 化"是错的**：AI 面板里"审批危险操作"是**安全边界**，不是显示问题。
审批块必须与命令总线同源、同生命周期、能在系统忙时可靠降级 —— 而 WebView2 的
document/DOM 模型给不了这个保证（进程被杀、渲染线程卡住、页面 navigate 都能让审批态失联）。
让安全关键路径跑在一个**可选依赖**（Win10 部分机器没装 Runtime）上，是把护栏建在沙子上。

**由此定下的结构约束（不可协商）**：

- **审批块不属于 `WebPanel`**。它是**面板级固定条**，画在原生层，
  永远在输入区之上可见 —— 不随「对话 / 活动」页签切换而消失，
  也不因 WebView2 Runtime 缺失而丢失。这两条是硬要求，不是"最好有"。
- **审批块只有一份**。对话流里的工具调用摘要**不得**再渲染一份可点的批准按钮
  （否则会出现两个批准入口，状态无法保持一致）。对话流里只显示"此操作待批准"的提示，
  批准动作统一由固定条承担。
- 降级路径同理：WebView2 不可用时，消息区退为纯文本，
  但**活动视图与审批条完全不受影响**。

**后果（正）**

- 启动快：无 WebView2 环境初始化（实测单个空白 WebView2 ≈260 MB 私有内存，见主仓 §4.3）。
- 零依赖：单 exe，不需要 WebView2 Runtime（Win10 部分机器没装）。
- 自绘的交互（悬停、命中、右键菜单）完全可控，不受 HTML 布局/焦点模型影响。

**后果（负）**

- 视觉迭代慢：改一个间距要重编译（对比改 CSS 秒级）。
- 与 `ui/theme/tokens.css` 存在"两份色值"的漂移风险，只能靠约定与代码评审约束。
- 复杂面板（富文本、流式消息）不适合用结构化自绘 —— 但**不等于必须 Web 化**：
  原生富文本（DirectWrite）是可选项，代价见上（≈2,600 行 + Markdown 子集解析器 + D2D/GDI 混用）。
  所以 P4 的形态是"原生外壳 + 一块内容区"，内容区是原生富文本还是 Web，尚未拍板。
- **Web 化边界从此有了一条线要守**：一旦有人提"这个面板也用 Web 吧"，得先过上面那张分流表，
  否则范围会慢慢滑回"整个面板都是 Web"。

**被否的替代方案**

- *整个窗口 WebView2*：省了自绘，但 260 MB 起步、启动慢、标签栏这种 32 px 高的控件用 HTML 反而更难做准。
- *Direct2D / DirectComposition*：视觉更好，但引入新 API 面与调试成本，当前收益不足。

**附带的决策：下拉菜单也自绘。** 系统 `TrackPopupMenu` 带不了图标，行高与配色也跟不了主题，
所以统一走 `popup_list`（`WS_POPUP` + owner-draw `LISTBOX`，见 `ARCHITECTURE.md` §4.1）。
代价是多一个模态嵌套消息循环（重入风险、`rows` 生命周期约束）。

**附带的复检条件**：若弹层需要多级子菜单、键盘增量搜索、分组标题等系统菜单本就支持的能力，
先评估"`TrackPopupMenu` + `MFT_OWNERDRAW`"是否够用，而不是继续往自绘里加逻辑。

**复检条件**：若 chrome 的自绘代码超过约 3,000 行，或需要动画/圆角/阴影等 GDI 难做的效果成为常态，
则重新评估"chrome 也 Web 化"。

**附带的复检条件（2026-11-22 补）**：若结构化面板的自绘代码超过约 2,000 行，
或需要"行内富文本 + 结构化混排"（如命令结果里嵌一段 Markdown 错误说明），
则该面板按上表分流到"内容区"（原生富文本 `RichPanel` 或 `WebPanel`，二选一），
而不是继续加原生绘制代码。**分流到内容区 ≠ 分流到 WebView2** —— 见上节 Pulse 实证。

---

---

## D4 多实例 + 激活优先（原"单实例 + WM_COPYDATA 转发"，已按复检条件修订）

**背景**：旧决策用命名互斥量 `Local\ExBarBrowser.Instance` 把程序锁成单实例，第二个进程只能转发路径
后退出，"在新窗口打开"（`NewWindow` / `CmdOpenInNewWindow`）两条命令因此不可实现——这正是当初记下的
复检条件："若用户明确需要并排双窗，加开关绕过互斥量即可，本决策不需推翻。"用户现已明确要求允许多实例。

**决策（2026-11-21 修订）**：

- **删除互斥量**。不存在任何"已在运行 → 拒绝启动 / 弹框退出"的分支。
- **默认启动 = 激活优先**：找得到已有主窗口就把路径经 `WM_COPYDATA`（魔数 `0xEBB1` 保持不变，理由见 D9）
  转发过去开新标签并还原/前置该窗口，随后新进程退出——保留 360 式"一个程序、多个标签"的直觉。
- **显式多开 = 新起进程**：`--new-window`（"在新窗口打开"命令启动子进程时自动带上，共享同一份用户数据）
  或 `EXBARBROWSER_MULTI`（同时落到 profile 根 `profiles\multi`，用于与正跑着的实例隔离测试）。
- 转发用 `SendMessageTimeout(SMTO_ABORTIFHUNG, 2s)`，窗口卡死时不拖死新进程；找不到窗口（最多等 400ms
  吃掉双击竞态）就当没有实例，直接新开。

**为什么现在安全**：旧决策锁单实例的真实动机是"两个进程同写 settings.json，后写覆盖前写"。现在
`tf::WriteText` 已是原子写（`<file>.<pid>.tmp` + `FlushFileBuffers` + `MoveFileEx(REPLACE_EXISTING)`），
书签侧另有"拒绝覆盖 + 另存 .conflict"护栏；共享数据的双实例不再互相撕文件。会话/几何由"最后退出的
实例"胜出，属可接受的语义。

**后果（正）**

- "在新窗口打开"从不可实现变为天然成立：`OpenPathInNewWindow` 带 `--new-window` 启动子进程即可。
- 并排比较两个目录不再需要先关掉已有窗口。

**后果（负）**

- 双进程各持一个 `IExplorerBrowser`，内存占用翻倍（用户自己选择多开时的合理代价）。
- 共享 profile 的实例退出顺序决定最后的会话/几何快照。

**被否的替代方案**

- *恢复互斥量*：回到"要并排先关窗口"，与用户明确要求冲突。
- *DDE / 命名管道转发*：比 `WM_COPYDATA` 复杂，没有收益（同机同会话，`WM_COPYDATA` 足够）。

---

## D5 P1 不做配置持久化（superseded，2026-11-27 复核）

**背景**：P1 的目标是验证"独立进程 + 托管视图"这条路能走通。配置体系（Schema、原子写、跨进程一致、
与 mod 共用目录）本身是一个独立工程。

**决策**：P1 全部硬编码——窗口 1100×760、默认路径 `FOLDERID_Profile`、主题跟随系统、快捷键写死在 `ACCEL` 表。
**不写任何文件**（除诊断日志）。

**后果（正）**

- P1 没有"配置文件格式定错了以后要迁移"的包袱。
- 用户目录不被污染，卸载零残留。

**后果（负）**

- 每次启动从零开始：窗口几何、标签组、视图设置都不记忆。
- 无法做"用户可改的快捷键"，`HotkeyManager` 与 `Preferences` 被阻塞。

**取代情况（2026-11-27 复核）**：P2-0 已落地并**没有**照原计划用 `%APPDATA%\ExBar` 的 Schema，
而是选了便携模式 `<exe>\data`（理由见 `docs/ROADMAP.md` §3.1"现状"：程序目录可整体拷走、
不与 ExBar mod 共写同一份 bookmarks.html）。本条按当时的"复检条件"标记 `superseded`。

---

## D6 与 ExBar mod 共享设计令牌与数据格式，不共享代码

**背景**：两个程序服务同一批用户，视觉与数据如果分叉，用户会觉得"这不是一个产品"。
但代码共享会重新引入单 TU 约束（mod 必须单文件）与构建耦合。

**决策**：

- **共享**：颜色/间距/字号的取值口径（`Theme` ↔ `ui/theme/tokens.css`）、面包屑与标签的交互语义、
  默认快捷键、`bookmarks.html` / `sessions.txt` / `keymap.json` 覆盖层的**文件格式与路径**。
- **不共享**：任何 `.cpp/.h`、任何构建产物、任何进程内状态。

**后果（正）**

- 用户在两个程序里看到同一套视觉、同一套键位、同一份书签。
- 任一程序可以独立编译、独立卸载、独立发版。

**后果（负）**

- 色值/交互有两份实现，靠人工同步；改一处忘另一处就会漂移。
- 数据文件并发写有覆盖风险（已在 [ROADMAP.md](ROADMAP.md) §6 记为待决策）。

**缓解**：`docs/PARITY.md` §6 的快捷键对照表 + `ARCHITECTURE.md` §13 的模块对应表，
每次改动交互前先对照；数据文件 P2 只读，写入等锁方案定稿。

**复检条件**：若出现第三次"改了一边忘了另一边"的实际事故，就应该把令牌生成成代码
（一个 `tokens.h` 由 `tokens.css` 生成），而不是继续靠约定。

---

## D7 不引入构建系统

**背景**：19 个源文件（10 个 `.cpp` + 9 个 `.h`）、零第三方依赖、全量编译秒级。

**决策**：`build.sh` / `build.bat` 各一条 clang 命令；不引入 CMake / Make / Meson / vcpkg。

**后果（正）**

- 没有"构建系统配置"这一层需要维护，也没有 generator 缓存导致的"改了头文件没重编"。
- 任何人（包括 AI）读一个文件就知道怎么编。

**后果（负）**

- 新增源文件要同时改两个脚本（已在 `DEVELOPMENT.md` §3.1 写成检查项）。
- 没有依赖图，全量编译；文件多了会慢（当前不构成问题）。
- 没有 install/test target，靠文档里的检查清单。

**复检条件**：源文件超过约 30 个，或编译时间超过 30 秒，或需要外部依赖（届时先考虑"自写 200 行"是否可行）。

> **复检已触发（2026-11-20，2026-11-21 复核仍在阈值之上）**：源文件目前已 60 个 `TU`，
> 超过本条写下的 30 个阈值。但触发后的处置不是引入 CMake，而是把"清单"从手写改成自动扫描
> （`discover_sources()`），并把 PCH + 分 TU 增量 + 并行加进同一个小脚本。
> 全量 10.5s、改一行 ~1s，仍在本条要求的量级内，所以决策维持 `accepted`，
> 只把"新增源文件要改两个脚本"这条负面后果消除掉。

---

## D8 从 Windhawk 主仓独立为自包含仓库

**背景**：本工程 2026-11-20 之前寄居在 `D:\Windhawk` 主仓里，靠根 `.gitignore` §8 的白名单放行
（`!ExBarBrowser/**`）。这带来三个具体代价：

1. **提交要跨目录**：改一个分片得在主仓里 `git add ExBarBrowser/`，与 ExBar mod 的改动共用一条时间线。
2. **历史不可读**：`git log` 里 7.7 万行的 mod 改动与 1.8 万行的独立工程交错，回溯"这个文件为什么这么写"要跨项目理解。
3. **工作区搬迁后变幻影**：工作区迁到 `E:\ExBarBrowser` 后，主仓 `git status` 立刻出现 95 条 `D`（文件"被删除"），
   而实际文件都在，只是不在主仓的路径下。

**决策**：在 `E:\ExBarBrowser` 建独立仓库（分支 `main`），与主仓互不引用。主仓里那 95 条删除由操作者
自行决定何时清理 —— 两边已无引用关系，先后顺序不影响任何一边。

**后果（正）**

- 提交边界清晰：`git log` 只包含本工程的演进。
- 克隆即构建：`src/` + `tests/` + `tools/` + `docs/` + 构建入口构成完整闭环，不依赖主仓任何文件。
- 可以按自己的节奏打 tag / 切版本（0.2.1 起）。

**后果（负）**

- **`data/` 不入库的代价变实**：以前寄生在主仓，用户配置至少还在同一台机器的同一个仓库里；
  现在新克隆首次启动没有 `bookmarks.html` / `settings.json`，走内置默认表 + 空书签树。
  这是刻意的取舍（书签/键位/窗口几何是个人数据），已写进 README §3.4。
- **`src/keymap.gen.h` 必须入库**：它是 `data/keymap.json` 的生成物，但也是"配置缺失时"的兜底默认表。
  不入库会让新克隆在无 `data/` 时一个热键都没有。由 `check_commands.py` 保证两者一致，
  所以"生成物入库"在这里不是坏味道。
- **必须自己维护 `.gitignore` 的全集**：不再有主仓根 `.gitignore` 兜底产物。

**被否的替代方案**

- *在主仓里改根 `.gitignore` 指到 E 盘* —— git 不支持跨仓库路径，只能靠符号链接，Windows 下脆。
- *用 submodule 挂进主仓* —— 主仓并不需要它，加 submodule 只是把耦合换个形式保留。

**复检条件**：若出现"必须与 ExBar mod 同批提交才能保证一致"的实际需求（例如共享 `tokens.css` 的生成），
则考虑用 submodule 或 subtree 重新挂载；否则保持独立。

---

## D9 品牌串与窗口类名收编到 `identity.h`，命名按领域

**背景**：独立（D8）之后，"为了和 ExBar mod 共存"这个命名理由全部消失，但名字还在，于是三类问题同时存在：

1. **归属错误**：`namespace eb`（= Ex**B**ar）是**上一个项目**的前缀，却写在独立工程的每个 UI 文件里。
2. **一物多名**：品牌串三套 —— `ExBar`（窗口标题）/ `ExBarBrowser`（类名/管道/日志）/ `EXBAR_*`（书签属性）。
3. **改名成本**：同一个 `L"ExBarBrowser"` 字面量散在 9 个文件里（类名 11 处），改名要动 9 处。

**决策**：

- 品牌串、11 个窗口类名、互斥体、管道前缀、日志文件名、`WM_COPYDATA` 魔数**全部收编**到 `src/identity.h`
  的 `namespace id`；其他文件一律用常量，不写字面量。
- `WM_COPYDATA` 魔数**保留** `0xEBB1`。曾改为 `0x45584242`，随后撤销：新旧版本窗口类名相同
  （`ExBarBrowser_Main`），升级后旧进程仍可能在跑；新进程用新魔数发消息会被旧进程拒绝，
  表现为"已有一个 ExBar 窗口未响应"——而那个进程是好的。独立确实免除了"与 mod 兼容"的义务，
  但同一程序的前后版本之间仍要兼容，这是当时漏掉的一层。
- 命名空间与文件名按**领域**，不按来源项目（`nav` / `ico` / `str` / `win` / `shellops` … 已合规，
  `eb` 是唯一例外，拆解计划见 [NAMING.md](NAMING.md) §3.2）。
- 命令主名朝点分名（`win.quit`）演进，旧名进 `legacyName` 永不删（规则 C1/C2）。

**后果（正）**

- 改名只需动一个文件；`tools/check_components.py` 新增 3 条规则把它变成机械约束
  （品牌串、显示名、类名前缀只能出现在 `identity.h`）。
- 第一条命令已完成主名迁移：`QuitExplorer` → `win.quit`（表里的 `name` 改了，`CmdId` 枚举名保留 —— 规则 C1）。

**后果（负）**

- **迁移期的双名状态**：命令表里同时存在旧名（`TabClose`）与点分名（`win.quit`）。任何读命令名的工具
  都必须两个方向都认 —— 这一轮就因此暴露了两个检查脚本的缺陷（`check_commands.py` 与 `check_ai_meta.py`
  的正则只认 `\w+`，改名后的条目被整行跳过，误报"引用不存在的命令"）。
- `eb` 命名空间仍在，拆解是待办而非已完成。

**复检条件**：`docs/NAMING.md` §7 的待办表全部转 ✅ 后，本条可标记为 `superseded`（由命名规范接管）。

---

## D10 AI 面板内嵌外部 agent，走 ACP；不内置 LLM 客户端

> **⚠️ 本决策的"形态选择"已被 D13 取代（2026-11-22 21:54）。**
> 下文关于形态 B（ACP）的论证保留原文，作为**被否方案的完整记录** —— 它记录了
> "为什么一度选了 B"以及 B 的真实代价，将来若重开这个议题，从这里读起。
> **当前生效的是 D13（形态 A：自带模型客户端）**，D10 里与形态无关的部分
> （审批通道的必要性、"不把 fs/terminal 原语直通给 AI"的安全底线）继续有效。

**背景**：P4 的目标是"AI 面板能在窗口内对话"。但本工程现状是**纯模式 C**——
`src/ai_api.h` 明言"对外的 B 就是**任意本地客户端**（AI 宿主、脚本、MCP 服务器都行）"，
`src/ai_server.cpp` 的方法全集里**没有任何"把消息送给客户端"的方法**，
唯一 server→client 通道是 `events/subscribe` 的只读事实流。
所以人在窗口里**没法对 AI 说一个字** —— 这是架构缺口，不是 UI 缺口。

业界把"人怎么把指令送给模型"做成了三种形态：

| 形态 | 谁连模型 | 密钥谁管 | 代表 |
|---|---|---|---|
| A. App 自带模型 | App 自己发 HTTP | App 存 key | VS Code Copilot、Obsidian Copilot v3（Quick Chat） |
| B. 内嵌外部 agent | App 拉起 agent 子进程，**借用 agent 的登录态** | agent 自己管，App 不碰 | Obsidian Copilot v4 Agent Chat、Zed Agent Panel、JetBrains AI |
| C. 外部 agent 驱动 App | agent 在外面，App 只提供工具 | agent 侧 | **本工程现状** |

形态 C 解决不了"人在窗口里下指令"。

**决策（2026-11-22）**：AI 面板的输入通道走 **ACP（Agent Client Protocol）**——
本工程当 ACP **客户端**，把外部 agent（Claude Code / Codex / OpenCode）作为子进程拉起，
用 JSON-RPC 2.0 over stdio 与之对话；agent 反过来把本工程已有的 MCP 服务端当工具用。

**为什么是 ACP 而不是自建**：ACP 是 Zed 2025-08 发布、Spec 1.0 已在开放治理下冻结
（Apache-2.0，背书 Zed / JetBrains / GitHub / Google / Cognition）的开放标准，
四个原语（`dispatch` / `observe` / `interrupt` / `collect`）与
`session/request_permission` 恰好覆盖本工程需要的一切，且**不需要第三方 SDK**
（本工程禁第三方库，wire protocol 自己实现，约 400 行）。

**为什么不是形态 A**：形态 A 要给本工程新增出网能力（HTTP 客户端）+ 密钥存储 + 密钥进价 UI，
与"单 exe 零依赖、零出网"两条现状直接冲突。形态 B 借用 agent 自己的登录态
（Obsidian 官方口径："Claude uses the account held by the Claude Code CLI.
**You do not paste that account's key into Copilot.**"），本工程一个字节的密钥都不用碰。

**为什么不是"给 MCP 加反向通道"**：MCP 是 client→server 的请求模型，
加反向通道等于自己发明一个私有协议，还要维护会话/流式/取消/审批四套语义 ——
ACP 把这些都标准化了。

**审批：现有双门不够用，要加一条"宿主主动询问"通道**（2026-11-22 修正）。

现有双门是**客户端声明式**的 —— `src/ai_api.cpp` 的判定是：

```cpp
if (spec.effects.destructive) {
    if (!Config::Get().s().aiAllowDanger) { ... reject("danger_disabled"); }  // 设置里的静态开关
    if (!approve)                           { ... reject("need_approval"); }   // 调用方带 approve=true
    auditlog::SetApproval(L"granted", L"ai");   // 批准是**调用方声明**的
}
```

而宿主唯一那个"弹框问人"的回调 `confirmFn_`（`src/commands_confirm.cpp`）
**只对 `ActorKind::Human` 生效**（`if (actor.kind != ActorKind::Human) return true;`），
AI actor 走不到；而且它是**同步返回 bool** 的。

ACP 的 `session/request_permission` 语义恰好相反：**agent 请求宿主批准**，
要的是"宿主问用户 → 用户答 → 宿主回执给 agent"的**异步往返**。
方向与现有双门相反，不能"映射"过去。

所以要新增的是：**宿主侧审批请求通道** —— 宿主挂起一个待批请求，
面板把它显示成审批块（面板级固定条），用户点完宿主回执。
这条通道**当前不存在**，它是 P4-a 审批 UI 的**隐藏前置**，工作量可能大于面板本身。

**这条不改 D10 的结论**：仍然不走形态 A、仍然用 ACP、仍然不内置 LLM 客户端。
只是"审批"这一格从"映射到现有双门"改成"新增一条宿主询问通道"。

**后果（正）**

- **本工程已有的每一块都正好对上**，新增面只有一处：

  | 环 | 已有资产 | 新增 |
  |---|---|---|
  | 人 → agent | — | 面板输入框 + `session/prompt` |
  | agent 启动 | `dev_proc.cpp` 的 CreateProcessW + Job Object + 硬超时 | ACP 客户端（~400 行，自研 wire protocol） |
  | agent → 工具 | `ai_api.cpp` `CallImpl` / `tools/call` 全在 | stdio→管道桥接（`mcp_stdio.cpp` 同构，改注入令牌那步） |
  | 审批 | `aiAllowDanger` + `approve` 双门（**客户端声明式**） | ACP `session/request_permission` 需要**另一条通道** —— 宿主主动询问式（见下） |
  | 结果回显 | `events/subscribe` 推送 | agent 侧 `session/update` 流 → 面板 |

- 零出网变化、零密钥管理、零第三方依赖、**命令表一行不改**。
- 本工程的 `Effects{readOnly,destructive,idempotent,openWorld,undoable,scope}`
  与 Obsidian 的权限粒度、"asks before actions that need your approval" 已经是同一套模型。

**后果（负）**

- **用户得先有 agent**。本工程坚持单 exe 零依赖，所以"托管下载 opencode 二进制"（Obsidian 的做法）
  只能交给用户或 agent 自己的安装器管 —— 本工程不内置下载器。
- **Claude Code / Codex 需要适配器二进制**；原生说 ACP 的只有 Gemini CLI / Goose / Cline / OpenHands。
- **agent 是 coding agent**，天生想写文件、跑终端。**本工程只把 `CommandBus` 暴露给 agent**，
  绝不接 ACP 的 `fs/write_text_file` 这类直通文件系统的原语 —— 那会绕过危险双门。
  这条是安全底线，不是可选项。
- ACP 是"宿主 ↔ coding agent"的协议，agent 的输出以 **Markdown + diff** 为主，
  流式渲染自绘很痛 —— 这正是 `WebPanel`（D3 分流表的富文本那一行）唯一真正需要 Web 化的地方。

**被否的替代方案**

- *内置 LLM 客户端 + 自存 key*：要新增出网能力与密钥管理，且密钥一旦进 `settings.json` 就是长期资产风险。
- *给 MCP 加反向推送通道*：自研私有协议，四套语义（会话/流式/取消/审批）全要自己维护，
  且与"零第三方依赖"不冲突但与"不重复造轮子"冲突。
- *只保留只读活动视图*（形态 C）：人在窗口里依然没法下指令，P4 的核心目标没达成。

**复检条件**：若 ACP 生态在 12 个月内明显收敛到别的协议（或本工程所需语义被某方私有协议覆盖），
重新评估。若本工程出现"必须内置模型"的产品要求（如离线单机使用且无任何 agent 可装），
本决策作废，回到形态 A 并接受出网与密钥管理的成本。

### D10 的三个落地子决定（2026-11-22）

**① agent 子进程归属：每窗口一个。**

D4 定了多实例 + 激活优先。两个 ExBarBrowser 窗口 = 两个 AI 面板。
**每个窗口拉起自己的 agent 子进程**，不共用。

理由：agent 的对话上下文里带着"用户在看哪个窗口、选中了哪些文件"。
共用一个进程，第二个窗口的请求会带着第一个窗口的上下文进去，
模型会分不清"现在说的是哪个窗口"—— 而 `get_state` 类工具返回的又是**当前进程**的状态，
两边对不上，模型会做出错误推断。
我们的 MCP 发现文件本来就是**多实例枚举**（`ai-pipe.<pid>.txt` / `ai-token.<pid>.txt`），
每窗口一个 agent 天然对得上。

代价：每个窗口多一个 agent 进程（内存/启动开销）。可接受。

**② 对话历史：新增 `<dataDir>/chat.jsonl`。**

`events.jsonl` 存的是**事实**（谁调了什么命令、成没成），**不含 agent 的散文**。
只存事件的话，程序重启后面板的历史只剩"命令活动"，看不到"当时说了什么"。

所以新增 `chat.jsonl`，与 `events.jsonl` 同目录、同约定
（`tf::Join(Config::Get().Dir(), L"chat.jsonl")`，追加写 + 按大小轮转，坏行跳过）。
**一条记录 = 一个对话轮**，带 `traceId` 与 `events.jsonl` 关联，
这样"这段话背后跑了哪几条命令"能对得上。

**③ 输入出口：一个入口，但出口是"选哪个 agent"。**

面板底部就**一个输入框**。旁边那个 chip **不是"两种出口"** ——
本工程**没有本地 agent**（形态 A 已被否决），只有 ACP 一个出口。
chip 的语义是**选 agent**（Claude Code / Codex / OpenCode …），
即"这句话派给哪个 agent 子进程"。

（原型里那个 chip 一度写成"两种出口"是文案错误，已改。）

---

## D11 agent 的动作粒度：检索层、多步编排、计划级确认（候选形状，2026-11-22 记录）

**背景**：D10 定了"agent 借 `CommandBus` 做事"，但**怎么借**还有三个问题没答案：

1. 命令表合计 215 条（用户域 146 + 工具域 69），其中 171 条 `aiExposed` 进 `tools/list`；
   agent 是拉全量列表再挑，还是本地检索？
2. 一次要做三件事（选中 → 改名 → 刷新），是让 agent 循环三次 `tools/call`，还是一次交一串步骤？
3. 一串步骤要不要**一次确认**、**一次撤销**？

`jimmgreen/pulse`（在售的 Windows 文件管理器，C++20 + Win32 + Direct2D，
Apache-2.0）在这三点上都有可验证的实现，故以它为参照。

**Pulse 的一手做法（`src/app/ai_assistant_agent.cpp` 的 `Tools()`，`:165-265`）**

| Pulse 的工具 | 它解决的问题 | 本工程现状 |
|---|---|---|
| `find_actions`（`:249`） | 按词检索动作表，返回 id/title/args/now/enabled+why/risk/asks_user/shows_plan/locked/shortcut/group/allowed | **无** —— 只有全量 `tools/list` |
| `run_actions`（`:254`） | 多步编排：≤12 步，每步 `{action, args, wait}`，**整体作为一个 undo group** | **无** —— `tools/call` 一次一条 |
| `run_task` / `organize_files` / `rename_files`（`:199-235`） | 写动作先出**计划卡**，用户在 Pulse 里确认后才动；确认后整个任务是一个 undo step | 无（只有逐命令 `approve` 双门） |
| `disk_usage` / `find_cleanup` / `find_duplicates` | 只读工具的答案是**卡片**（带复选框与理由），确认后执行 | 无 |

Pulse 的 `risk` 有 10 档（`read / view / organize / create / move / system / external /
delete / permanent_delete / user_only`，见 `docs/ai/mcp.md`），比本工程的 3 档
（`read / write / danger`）细。**本工程不照抄 10 档** ——
档位名是协议的一部分（`ai_api.h` 的 `RiskName`：`tools/list` 里写的、结果里回的、
审计里记的必须是同一个词），加档位要同步改这三处。但"**动作自带机器可读的风险与可执行性**"
这个形状照抄，因为本工程已经全都有了：`Effects{readOnly,destructive,idempotent,openWorld,undoable,scope}`、
`needsSelection`、`requires_`（前置条件）、`assertion` / `assertionExempt`。

**决策（2026-11-22）：这一条只记录形状与待验证项，不锁死实现。**
**且三项本身也不排期 —— 先记着，等 P4-b 跑完再定**（P4-b 只做 ACP 链路 + 纯文本消息区）。
三件事分三档：

| 档 | 事项 | 前置条件 | 去处 |
|---|---|---|---|
| 已有，零新增 | 动作元数据（风险/前置条件/断言）已经机器可读 | — | 现状即达标 |
| 小 | `find_actions` 检索层（本地按词匹配 `CommandTable`） | 匹配算法（中英文/拼音）与 `tools/list` 共用同一张表 | P4-c |
| 中 | `run_actions` 多步编排（≤N 步为一个 group） | **`fileundo::Stack` 现在是扁平 `vector<Item>`，没有 group 标记** —— 要加 | P4-c |
| 中 | 计划级确认（先出方案卡 → 一次确认 → 一次撤销） | 现有 `aiAllowDanger` + `approve` 逐命令双门之上再加一层；两层的关系要定 | P4-c |

**为什么"计划级确认"不能取代逐命令双门**：本工程的双门是**按命令的 `Effects` 判**的
（`destructive` ⇒ 静态授权或运行时确认），它管的是"这条命令该不该被机器做"。
计划卡管的是"这一串动作合起来你想不想要"。两者正交：一条 `destructive=false` 的命令
组成的批量操作也可能是用户不想要的，反过来一条 `destructive=true` 的单命令用户可能就是要。
所以是**两层叠加**，不是二选一。

**为什么"检索层"值得做**：171 条进 `tools/list` 的命令（每条都带 `argsSchema` 与
`outputSchema`）对 agent 是一次很重的上下文消耗，而且模型挑命令靠的是自然语言匹配。
把"找哪条命令"挪到本地做（`CommandTable` 已经在进程里），模型只发一个词。

**被否的替代方案**

- *照抄 Pulse 的 10 档 risk*：档位是协议，改一处要同步三处；本工程 3 档 + `Effects` 的 6 个布尔
  已经能表达"读/写/不可逆"（`Effects` 是 5 个布尔 + 1 个 scope），够用。
- *让 agent 循环 `tools/call` 自己拼步骤*：可以，但没有 group 就没有"一次撤销"，
  而且中途失败会留下半截状态（Pulse 的规则是"Stops at the first step that fails;
  earlier steps stay done"，本工程至少要做到一样诚实）。

**复检条件**：若本工程的 `CommandTable` 稳定在 100 条以下且 agent 侧上下文不吃紧，
检索层可以降级为"不做"；若多步编排始终做不出 group 标记，则该档作废，只保留单步 + 逐命令双门。

---

## D12 AI 面板的宿主：同一客户区最右一列，squeeze，快捷键启停

**背景**：D10 定了"面板里能对话"，D3 定了"不用 WebView2"。
剩下没定的是**面板长在哪里**：是独立窗口？是叠在文件列表上？还是窗格区里的一列？
`jimmgreen/pulse`（在售的 Windows 文件管理器）给了三种可抄的形态，
但它的窗口结构与我们不同 —— Pulse 整个窗格布局是自己算的，我们的文件区是
`IExplorerBrowser` 托管的 Shell 视图。

**决策（2026-11-22）**：AI 面板是**同一个客户区里最右边的一块**，
由**我们自己的布局代码**划出来，把 Shell 窗格区往左挤（squeeze）；
**由快捷键启停**（`win.ai.toggle`，`CmdId::AiTogglePanel`，命令 ID 与名字已预留，
只是 `implemented=false`），关闭时宽度归零、不占地方。

**为什么能划在最右**：`src/app_layout.cpp` 的横向分区是我们自己的代码：

```cpp
RECT inner{navW, 0, rc.right, viewH};   // 让出最右 → rc.right - aiW
```

注释原文："占位由我们的布局算，不由 shell 说了算"。
Shell 的预览窗格是 **`inner` 内部的子窗口**（`shell_view_preview.cpp` 找
`"Shell Preview Extension Host"`），**不占 `viewArea_` 的最右** ——
所以最右这块是我们的，AI 面板与预览窗格**不抢位**。

**为什么是 squeeze 而不是 overlay**：overlay（盖在列表右边）会**遮住文件列表的右半边**，
被遮住的可能正是选中项或正在看的内容。squeeze 让 Shell 自己重排列宽 ——
details 视图的列按比例重分配，与 explorer 拖窗口宽度是同一种行为，用户熟悉。

**后果（正）**

- 面板**不占独立的 HWND**，与 chrome 同一套自绘、同一套焦点与命中模型。
- **关掉不占宽度**：宽度归零即回到纯文件浏览。
- 面板列在 **Shell 窗格区之外**，**不与预览窗格抢位**，也不需要 Shell 的 supplemental pane 机制。

**后果（负）**

- **展开/收起时 Shell 列表重排列宽**。滚动位置一般保持（Shell 按 item 滚动），
  但 **details 视图宽于窗口时的横向滚动位置是否重置要实测** —— 不达标就得补位置保持。
- `inner` 被压窄时 **Shell 的预览窗格会被挤没** → `inner` 必须留下限。
- 焦点管理从"一个窗口一套"变成"两个区域各一套"，必须显式定义两态。

**被否的替代方案**

- *overlay 盖在列表上*：遮住选中项与正在看的内容（已否，理由如上）。
- *做成独立窗口*：多一个顶层窗口（AGENTS.md 的窗口类 ≤2 是硬约束），
  且失去"与文件列表同处一个画面"的上下文感。
- *插进 nav pane 右边（文件区左侧）*：可行，但方向与主流编辑器相反
  （VS Code / Cursor / Zed 的 agent 面板都在右），用户肌肉记忆在右。
- *浮动小窗（Pulse 的 Floating 态）*：我们左侧是全局导航树、不是可折叠导轨，
  浮动卡没有锚点，不做。

**复检条件**：若实测发现 Shell 在宽度突变时**必定**重置横向滚动位置且无法保持，
则改为"面板列固定宽度、展开时**不**挤压而是把 AI 列叠在 `inner` 之上并加一层遮罩"
（回到 overlay 的折中：只遮最右 N 像素，保证选中项仍可见）。

---

## D13 AI 面板走形态 A：本工程自带模型客户端（取代 D10 的形态选择）

**背景**：D10 选的是形态 B（内嵌外部 agent + ACP）。用户于 2026-11-22 21:54
明确要求"我就是要方案 A，你别给我扯什么审批这个那个"，并要求给出其他产品
"到底怎么做到的"可实现方案。调查了 5 个真实在售产品后（见下"业界实证"），
形态 A 的实现路径是清晰、成熟、可照抄的，因此改变结论。

**决策**：AI 面板的对话链路由**本工程自己**完成 ——
WinHTTP 发 HTTPS 请求、流式读 SSE、自己拼 tool_calls、自己管密钥（DPAPI）。
**不内嵌外部 agent，不走 ACP，不要求用户预装任何 CLI。**

**三层架构（硬约束，不可协商）**

```
┌─ UI 线程 ─────────────────────────────────────┐
│  面板：消息列表 + 输入框                        │
│  · 只负责画，不做网络                           │
│  · 定时器（≈30ms）从队列取增量，合并重绘          │
└───────────────▲───────────────────────────────┘
                │ mutex + 增量队列（只投 delta，不投整段）
┌───────────────┴───────────────────────────────┐
│  AI 引擎线程（一条，常驻）                       │
│  · 组装 messages[]、解析 SSE、累积 tool_calls    │
│  · 每个请求一个 requestId（可并发、可取消）        │
└───────────────▲───────────────────────────────┘
                │ WinHttpReadData（阻塞读，专用线程）
┌───────────────┴───────────────────────────────┐
│  WinHTTP（Windows 系统 API，零第三方库）          │
└───────────────────────────────────────────────┘

密钥：DPAPI（CryptProtectData）加密落盘，只在引擎线程解密使用
```

三条铁律，全部来自在售产品的共同做法：

1. **UI 线程不做网络**。WinHTTP 的阻塞读必须独占一条线程。
2. **密钥不进渲染层**。Lain AI Desktop 的作者原话："桌面 AI 客户端最常见的错误，
   是把 key 留在前端…一切与外界通信的东西都坐在原命令后面。"
   即使在纯原生实现里，这条也意味着：密钥只在引擎线程解密、用完擦除，不挂到任何 UI 对象上。
3. **只投增量不投整段**。UI 队列里放的是 `delta` 片段，不是累积后的全文 ——
   否则每条 chunk 都要重排整段文本，长回复必卡。

**为什么现在走 A（而不是继续 B）**

| 理由 | 依据 |
|---|---|
| B 要求用户预装 agent；A 不要求 | B 的"用户得先有 agent"是 D10 自己写下的负后果 |
| B 的国产 agent 支持虽好（Trae/CodeBuddy/Qoder/Kimi/Qwen 原生 ACP），但 Trae 有企业版旗舰套餐门槛、WorkBuddy 其实要退到 `codebuddy` CLI | 2026-11-22 实测查证 |
| A 的实现路径有 5 个在售产品验证，工作量可估（≈1,750 行到能跑） | 见下"业界实证" |
| A 的密钥管理只需 DPAPI 一个系统 API，不是"新建一套安全体系" | `CryptProtectData` 是 Windows 自带 |

**业界实证（2026-11-22 调查，5 个真实产品）**

| 产品 | 网络在哪 | 渲染在哪 | 关键结论 |
|---|---|---|---|
| **DeepSeek Desktop**（C++/Win32 + WebView2 + React） | **原生 WinHTTP 工作线程** | WebView2 | "走原生 http.stream 命令（WinHTTP 工作线程 + 按行切分的 SSE），绕过 CORS，也不会 UTF-8 截断" |
| **PowerBox**（MFC + C++20） | **原生 WinHTTP** | WebView2 | 一家做了 6 个 AI provider，全靠一个 WinHTTP 封装 |
| **TizuMark**（原生 Win32，零依赖） | — | **DirectWrite + Direct2D** | 常驻 **42 MB**、启动 **0.8 s**；开发周期是 Web 方案的 **4.7 倍**（作者自述） |
| **Lain AI Desktop**（Tauri: Rust + Svelte） | **Rust 侧 reqwest** | Svelte | "the API key never enters the webview" |
| **PolyUI**（Tauri: Rust + React） | **Rust 侧** | React | 用 `requestAnimationFrame` 批量累加 token，避免每 chunk 重渲 |

**关键观察**：这 5 个产品里，**没有任何一个在渲染层里发 HTTP 请求**。
渲染方式（WebView2 / 原生）与"谁发请求"是**正交**的两件事。

**同源参考：`E://Dopus//AgentChat`（本地既有实现，v2.0）**

这是同一个作者生态里的 Directory Opus AI 侧边栏，**就是形态 A 的完整实现**：
Node 服务 + OpenAI 兼容 `/chat/completions`（`stream:true`）+ SSE 工具循环 + 人工确认门禁，
15 个工具（`opus_*` 走 Opus 命令、`fs_*` 直连文件系统），UI 是 WebView2 + Web。

它的价值不是"照抄"，而是**用真实代码验证了 D13 的几个关键判断**，并纠正了我们实现里的两处缺陷：

| 它做的事 | 对我们的意义 |
|---|---|
| `applyTool` 判断"这一片能不能**自己**解析成 JSON 对象" → 能则**替换**，否则**追加** | ★ **直接纠正了 `ai_sse` 的一个真实 bug**：部分 vLLM / one-api 网关在每个 chunk **重发整段** `arguments`，盲目 `+=` 会拼出 `{"a":1}{"a":1}` 废 JSON。我们的单测 [11] 现在守着这条 |
| 逐行读 `buffer`，**空行才算一帧**，但 `FinishIncomplete` 里补一次 flush | ★ **纠正了第二个 bug**：末帧没有结尾空行时，最后一条 data（往往是 finish_reason 或最后一段正文）会被留在缓冲里丢掉 |
| `needsApproval`：读类工具不问，写类工具（`fs_write/mkdir/move/copy/delete`）**必问**；Opus 命令按**首词正则**判定是否改状态 | 审批判定要**代码化**，不能只写在系统提示里 —— "模型很乖"不是安全机制 |
| `summariseResult` 把工具结果**硬截断到 8000 字符** | 一次 `readdir` 大目录就能吃掉整个上下文窗口，模型随即停止推理真问题 |
| `ctx.readonly` 时**根本不发审批卡**，直接回 `not executed` | 只读模式不只是"用户不点批准"，而是**下游根本不该看到这个请求** |
| `maxSteps` 循环上限 + `usage` 跨步**累加**（max 合并，避免重复计数） | 无上限的工具循环会烧钱；usage 按"最大"而非"求和"合并能兼容重发累计值的网关 |
| 协议层 `protocols.js` 集中 vendor 预设（9 家）+ 两套 wire（openai / anthropic） | provider 差异要**收在一个文件**，否则两条循环一定会漂移（它为此专门写了 `prompt.js` 抽取共享件） |
| `resolveUserPath` + `allowedRoots` 在**代码层**做路径守卫 | 系统提示里写"不要碰 C://Windows"没有约束力 |

**它的取舍我们不一定照抄**：

| 它 | 我们 |
|---|---|
| WebView2 + Web UI（因为 Opus 脚本控件做不出气泡/流式） | **原生自绘**（D3/D13），因为我们本来就有成熟的 GDI 自绘外壳 |
| Node 服务 + 本地 HTTP（127.0.0.1:8792） | **进程内引擎线程**，不额外起服务、不占端口 |
| `opus_run_command` 走 `dopusrt`/文件 IPC 桥 | 我们**已经有 `CommandBus` 215 条命令**，不需要外部桥 |
| 15 个扁平工具 | 直接暴露命令表里 `aiExposed` 的 171 条 |

**代价（要认，不遮掩）**

- **本工程从此要出网**。"单 exe 零依赖、零出网"里的**后半条被放弃**。
  零依赖仍成立（WinHTTP 是系统 API，不新增任何 lib；`build_fast.py` 的 `LIBS` 只加 `-lwinhttp`）。
- **本工程从此要管密钥**。DPAPI 绑定当前用户，但密钥仍是长期资产 ——
  比 B 的"一字节都不碰"风险高。缓解：只在引擎线程解密、不落日志、不写 `settings.json`。
- **provider 差异要自己适配**。国内主流全部兼容 OpenAI chat format
  （DeepSeek / DashScope / Moonshot / 智谱 / MiniMax 都是 `/v1/chat/completions` + SSE），
  所以**第一阶段只做 `delta.content` 流式**，不碰各家 reasoning/thinking 参数差异。
  这是刻意收窄范围：Pulse 为了 13 家的思考档位写了一整套枚举，我们**不重复这笔账**。
- **tool_calls 分片拼接是必踩的坑**：`delta.tool_calls[].function.arguments` 是
  **分片 JSON 字符串**，必须按 `index` 累积到 `finish_reason == "tool_calls"` 再整体解析。
- **无 agent 的规划能力**。B 借用的是 coding agent 的整套自主循环（读文件、跑测试、自我修正）；
  A 只有我们自己写的 function-calling 循环。能力上限低于 B —— 这是主动接受的取舍。

**渲染：两条路，网络层完全共用**

| | A1：WebView2 | A2：原生 DirectWrite |
|---|---|---|
| 省掉 | Markdown 解析器（≈2,600 行） | — |
| 代价 | 260 MB 私有内存（已实测）+ WebView2 Runtime 依赖 | 自己写 md4c 解析 + DirectWrite 排版 |
| 参考 | DeepSeek Desktop / PowerBox | TizuMark（42 MB / 0.8 s）/ Pulse |
| 工作量 | ≈300 行集成 | ≈2,600 行 |

**这两条只影响"消息区怎么画"，不影响前面 ≈1,750 行（网络/密钥/provider/线程模型）。**
因此第二阶段（P4-b）先做**纯文本消息区**（`DrawText` 即可），跑通链路后再选 A1/A2 ——
这个顺序让"渲染选型"不阻塞"链路验证"。

**D10 里继续有效的部分（未被取代）**

- **审批通道的必要性**：D10 论证了"现有双门是客户端声明式的，宿主从不主动问用户"。
  换成形态 A 后这个问题**依然存在，而且更严重** —— 因为现在是**我们自己**当那个
  "客户端"，所以更容易把审批写成走过场。危险命令必须真的弹框问用户。
- **安全底线**：AI 的能力 = 命令表里 `aiExposed` 的那些，经 `CommandBus` 执行。
  **不给 AI 任何直通文件系统的旁路**。这条与形态无关。

**被否 / 被取代的方案**

- *形态 B（ACP 内嵌外部 agent）*：D10 的原文保留在上方作为完整记录。
  放弃原因见"为什么现在走 A"。
- *形态 A + WebView2，把 HTTP 放在渲染层*：密钥会进入可调试的渲染进程内存
  （WebView2 可开 DevTools），XSS 可变成"用用户的 key 计费"。否决。
- *自己不写网络层，靠 web 前端 fetch*：同上，且要放弃原生外壳。

**复检条件**

1. 若**首批目标 provider 全部不支持 OpenAI chat format**（即"一套 SSE 解析打不通"），
   重新评估 provider 抽象层的复杂度。
2. 若实测发现**长回复（>8k token）在纯文本消息区滚动/重绘有明显卡顿**，
   提前启动渲染层选型（A1 vs A2），不要等到"以后"。
3. 若 DPAPI 密钥方案在某类部署下不可用（如漫游配置、多用户共用机器），
   补一个"每次启动手工输入"的降级路径。
4. 若出现"必须离网可用"的产品要求，形态 A 直接作废（模型在云端，离网无从谈起）。

**索引同步**：D10 状态改 `superseded`；D3 的"富文本载体待定"按 D13 收口
（P4-b 纯文本 → 之后按复检条件 2 在 A1/A2 间选）。

---

## D14 AI 面板用 WebView2 + 子进程服务

**日期**：2026-11-23　**状态**：accepted

**背景**

D12 定了"面板占同一客户区最右一列"，D13 定了"自带模型客户端"。落地时遇到一个
反复出现的问题：AI 服务要驱动**用户眼前那个窗口**，而它是独立进程，只能靠命名管道找过来。
管道名内嵌 `hwnd + pid`，进程每次重启都变；`data/` 下还有几十个 profile 目录各有一套
发现文件。实测出现过 **180 个候选、绝大多数是死实例**，外加 `tasklist` 在当前会话视图里
看不到本程序进程（"实例是否活着"的判断结论完全相反）。

**决策**

1. **服务成为主进程的子进程。** `App::InitAiPanel` 在 `SyncAiChannel()` 之后拉起
   `node aichat/service/server.js`，把 `EXBAR_PIPE` / `EXBAR_TOKEN` 通过环境变量直接传下去
   —— 唯一知道"该连哪个窗口"的人就是主进程自己，服务从此不需要任何发现逻辑。
2. **生命周期三重保证**（任一失效仍有兜底）：
   - `Job Object` + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`（覆盖"主进程被强杀"）；
   - `Panel::Destroy()` 显式 `Stop()`（先等 1.5s 再强杀）；
   - 服务自己每 2s 探一次父进程 pid，发现不在就退出。
3. **渲染层用 WebView2**，`WebView2Loader.dll` **运行时** `LoadLibrary`
   （不进链接期依赖：没有 loader 时只是面板不可用，exe 照常启动）。
4. **端口不固定。** 由系统分配空闲端口（bind 0 读回），经 `AGENTCHAT_PORT` 传给服务
   —— 并行开发多开时互不冲突。固定 8792 会让第二个实例必然 `EADDRINUSE` 退出。
5. **收起 = 隐藏，不是销毁。** 销毁会关掉整个浏览器进程，再展开要重新启动 WebView2
   （约 1 秒白屏 + 丢滚动位置）。

**代价（显式豁免 AGENTS.md §1.2）**

- 新增两个运行时依赖：**WebView2 Runtime**（微软 Evergreen，Win10/11 普遍预装）
  与 **Node.js**。单 exe 零依赖的承诺在最严格意义上不再成立。
- 豁免理由：自己写渲染层是 5000 行量级；且 D13 已决定模型调用留在原生侧
  （密钥不进渲染进程 —— 见 D13 否决项"形态 A + WebView2 把 HTTP 放在渲染层"）。
- 面板是**可选功能**：缺 WebView2 或 Node 时显示占位页说明原因，文件管理完全不受影响。

**已踩过的坑（代码注释里都有记录）**

| 现象 | 真因 | 对策 |
|---|---|---|
| 命令作用在别的窗口 | `--mcp-stdio` 强制 `forceNew`，自己开一个实例 | 走管道，不走 stdio |
| 180 个候选连不上 | 多 profile + 管道名含 pid，发现文件互相独立 | 环境变量直传，不枚举 |
| `tasklist` 看不到本程序 | 会话/权限视图差异，结论与 `Get-Process` 相反 | 用 `process.kill(pid,0)` 或 `Get-Process` |
| Node 连命名管道一律 ENOENT | libuv 把 `\\.\pipe\X` 当普通文件路径解析 | 经 `NamedPipeClientStream` 桥接 |
| `.ps1` 报 `TerminatorExpectedAtEndOfString` | PowerShell 5.1 按 ANSI 代码页读 UTF-8，中文注释变乱码 | 桥脚本**纯 ASCII**，注释用英文 |
| 面板白屏、`ready=0`、回调静默不触发 | ①`Invoke` 没写 `virtual`（不在 vtable，槽位 3 是虚析构）②`Create...WithOptions` 多写了一个出参 | 见下"已解决" |
| 独立探针报 `err=193` | 探针自己编成了 32 位（漏 `-target`），32 位进程加载 x64 DLL 必然 193 | 排查工具先验架构 |

**已解决（2026-11-23 12:45）：真因是两个手写 COM 绑定的经典错误**

现象：面板长期停在白底占位页，`ready=0`；日志只有
`CreateCoreWebView2EnvironmentWithOptions -> hr=0x00000000`，之后的 `Invoke` 一次都没到。

**真因 ①：`Invoke` 没写 `virtual`，压根不在 vtable 里。**

类的写法是 `class CompletedHandler : public IUnknown`，然后写了一个**普通**
成员函数 `Invoke(...)`。它不带 virtual 就不进 vtable —— 于是 vtable 槽位 3
是**虚析构的 complete destructor**。WebView2 按槽位 3 调"Invoke"时，
实际调到了析构函数：对象被就地销毁，随后访问成员 → `0xC0000005`，
且崩溃地址落在我们自己的 `.rdata` 上（看起来像"loader 崩了"，误导排查一天）。

独立探针逐槽位调用得到的铁证：

```
vt[0] → QueryInterface ✓   vt[1] → AddRef ✓   vt[2] → Release ✓   vt[3] → 段错误
```

修法：**先显式声明接口结构（含纯虚 `Invoke`），再继承它**，并给实现加 `override`：

```cpp
struct IWebView2CompletedHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, IUnknown* result) = 0;
};
class CompletedHandler : public IWebView2CompletedHandler { ... };
```

改完立刻：`vt[3] → Invoke ✓`，探针 exit=0。

**真因 ②：`CreateCoreWebView2EnvironmentWithOptions` 多写了一个出参。**

官方 IDL 是 **4 个参数、无出参**（环境对象通过 handler 异步交付）：

```c
HRESULT CreateCoreWebView2EnvironmentWithOptions(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
```

我们照着"工厂函数有 out 参数"的惯性多写了 `IUnknown** environment`。
loader 把第 5 个实参当输出缓冲往里写 → 同样在调用内部崩。

**【教训】跨 DLL 的函数指针必须按 IDL 逐字抄，不要凭印象补参数；
COM 回调必须走"先声明纯虚接口、再实现"的路，不能直接从 IUnknown 继承后写普通函数。**

修好后的验证（主程序实测）：

```
CompletedHandler::Invoke kind=1 hr=0x00000000
OnControllerReady 进入：ctrl=00004C5800130308
导航到 http://127.0.0.1:7399/ -> hr=0x00000000
WebView2 已就绪
msedgewebview2 子进程 6 → 12 ；cache\webview2\EBWebView 已生成
Web UI: HTTP 200, title=「AI 助手」, 引用 app.js
```

**另一条被误判的线索（记录以免重蹈）**：独立探针最初报 `LoadLibraryW err=193`，
据此怀疑"环境阻止 DLL 加载"。实际是**探针自己编成了 32 位**（漏了
`-target x86_64-w64-mingw32`，clang 默认 i686），32 位进程加载 x64 DLL 必然报 193。
正式构建一直在 `build_fast.py` 里带 target，所以主程序从来都能加载 loader。
排查工具本身要先验证架构，否则得出的结论是反的。

**复检条件**

1. `aichat_webview.cpp` 里 `CompletedHandler::Invoke` / `OnEnvironmentReady` 的入口日志
   **保留**：它们是"回调有没有送到"的唯一分界点，成本是每 session 一两行。
2. 若 WebView2 在目标部署环境不可靠（精简系统普遍无 Evergreen Runtime），
   回退到"外部浏览器打开面板"（`ai.panel.web` / `Ctrl+Alt+J` 已实现）。

**已解决（2026-11-23 14:05）：真因 ③ —— 父窗口 0×0 时创建控制器 → 无合成表面**

现象：修好上面两个 COM 错误后，WebView2 全套子进程都在、导航 `hr=0`、`WebView2 已就绪`
也打出来了，但面板**仍是一片空白**；而用浏览器打开同一端口的 Web UI 完全正常
（证明服务和页面都没问题，纯粹是嵌入没生效）。

实证：展开面板后枚举窗口树，`ExBarBrowser_AiPanel`、`Chrome_WidgetWin_0/1`、
`Chrome_RenderWidgetHostHWND` 全是 **0×0 且 vis=0**，只有 `Intermediate D3D Window`
有尺寸 —— 典型的"建了控件但没有可见合成表面"。日志里反复出现
`SetBounds 0x0 @ (0,0)`。

真因：面板宿主是 `WS_CHILD`，创建时是 0×0/隐藏；`Toggle → Show(true)` 投递
`WM_APP_AI_PANEL_CREATE` 后异步创建 WebView2。若那一刻 `Layout()` 还没把真实矩形
`SetBounds` 进来（或收起时恰好给了 0 矩形），控制器就是在 0×0 父窗口上建的 ——
WebView2 照常导航、JS 也 ready，但**不会建立可见合成表面**，画面永远空白。
这与业界（HScreenFilter 等）踩过的"父窗口未显示就建控制器 → 白屏"是同一条约束。

修法（三处，互相兜底）：

1. `WebViewImpl::ApplyBounds()` 忽略非正宽高的 bounds —— 绝不把控件缩成 0；
   `OnControllerReady` 先 `SyncBoundsFromParent()`（取父窗口**实时**客户区）
   再 `put_Bounds`，并**无条件** `put_IsVisible(TRUE)`。
2. `WebView` 增加 `SetReadyCallback`：控制器就绪那一刻回调宿主，用宿主当前客户区
   再补一次 `SetBounds` + `SetVisible`。面板在 `Create()` 里注册该回调。
3. `Panel::EnsureWebView()` 在宿主仍为 0×0 时**不创建**，重投
   `WM_APP_AI_PANEL_CREATE`（有次数上限）等 Layout 给出真实尺寸；
   `Show(true)` 恢复可见时也重新推一次真实 bounds。

修好后的验证（隔离实例，管道命令 `ai.panel.toggle` 展开）：

```
[aiui] SetBounds 420x579 @ (662,104)
[aiui] CompletedHandler::Invoke kind=1 hr=0x00000000
[aiui] OnControllerReady 进入：hr=0x00000000
[aiui] 导航到 http://127.0.0.1:13756/ -> hr=0x00000000
[aiui] WebView2 已就绪
[aiui] 就绪回调：重设宿主客户区 420x579 visible=1
窗口树：ExBarBrowser_AiPanel / Chrome_WidgetWin_0/1 / RenderWidgetHostHWND /
        Intermediate D3D Window 全部 336×463 vis=1（修复前全是 0×0/隐藏）
```

**教训**：WebView2 建在不可见/零尺寸父窗口上不会报错，反而是"成功但空白" ——
这类假成功必须靠"就绪后从父窗口实时客户区重设 bounds + 强制可见"来兜底，
不能只依赖创建时那一次尺寸（它可能是过期的 0）。

**已解决（2026-11-23 16:05）：真因 ④ —— 父窗口已是最终尺寸时创建 → 内容渲染但不呈现**

用户仍反馈"面板空白"。这次窗口树**已经全部非 0×0 且可见**（真因③已修），日志也显示
导航 `hr=0`、页面服务 HTTP 200（`app.js`/`app.css`/`/api/*` 全部返回正确内容）——
按前一条的判据"该显示的都显示了"，可面板还是白的。

这次换了个验证手段才定位：**直接抓屏幕像素统计颜色数**（不是只看窗口树，也不是看
`put_Bounds` 返回值）。写了一个 DPI-aware 的取样脚本（`%TEMP%\ebb_diag\capture.py`），
取面板客户区按 4px 网格采样，数不同颜色：

| 状态 | 取样点 | 不同颜色数 | top 色 |
|---|---|---|---|
| 刚展开、首次就绪 | 14144 | **1** | 全 `0xffffff` 纯白 |
| 长跑实例（反复开合过很多次） | 58652 | 403 | `0x2b2b2b`/`0x323233`/`0xe8e8e8`（正是 UI 的 CSS 主题色） |
| 首次就绪后手动把宿主缩小 2px 再还原 | 14144 | **188→277** | 出现主题色与 `0x5b9bd1`（accent 蓝） |

真因：WebView2 在父窗口**已经是最终尺寸**时首次创建，内容在渲染进程里**已经画好**，
但合成器（compositor）没有为它建立"可见表面"（present surface）—— 面板显示的是
WebView2 的默认白底。只要宿主发生一次**真实的尺寸变化**，子窗口跟着 resize，
合成器借机建立可见表面、把已渲染的内容呈现出来。这与 WebView2Feedback #5673 记录的
"从 hidden 创建、显示后内容不呈现，直到一次外部重绘/resize 才出现"是同一类问题。
注意：就绪回调里 `put_Bounds` 用的是**和已有值相同**的矩形，不构成尺寸变化，所以
真因③的"重设 bounds"修法在这条路径上不触发呈现 —— 这也是它没修干净的原因。

修法：新增 `WM_APP_AI_WEBVIEW_PRESENT`，在 WebView2 就绪回调（以及从收起恢复）里投一条
消息；消息循环空闲后由 `Panel::KickWebViewPresent()` 把宿主 `SetWindowPos` 缩小 2px
再还原（`SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE`），制造一次真实尺寸变化，逼合成器
呈现。有 `presentKicked_` 一次性标记，不会反复抖动。

验证（同一隔离实例，**不做任何手动操作**，全靠程序自己 light 推）：

```
[aiui] 就绪回调：重设宿主客户区 420x579 visible=1
[aiui] 尺寸轻推强制呈现：420x579 → 418x577 → 420x579
取样：14144 点，277 种颜色，top = 0x323233 / 0x2b2b2b / 0x303030 / 0x5b9bd1 / 0xe8e8e8
```

**教训**：判断 WebView2 "嵌入是否生效"，窗口树非 0、导航 `hr=0`、HTTP 200 都只是
必要条件，**不是充分条件**。内容可能在渲染进程里画好了却没被合成器呈现 —— 唯一可靠的
判据是**抓实际屏幕像素**：面板区域是否出现主题色（而非 `0xffffff` 纯白）。凡是
"服务正常、浏览器打开正常、就是面板白"的情况，优先量像素，再决定是修嵌入还是修服务。

### D14 补充：WebView2 页面主题与键盘作用域（2026-11-23）

面板能显示之后暴露了两个"最后一公里"问题，都属于**宿主与 Web DOM 之间缺契约**：

1. **主题不同步**：Web UI 的 `app.css` 只有一套深色变量，宿主切到浅色后右侧面板仍是深色。
2. **打字被序列键截走**：低层钩子 `FocusIsOurs()` 与 `InputRouter::HandleKey()` 只把原生
   `EDIT` 类当文本输入区；WebView2 的焦点窗口是 `Chrome_RenderWidgetHostHWND`，
   于是在聊天框里敲 `g` / `x` 会启动序列状态机并弹出候选层，后续字符全被弹窗吃掉。

决策：

- **主题用"URL 首帧 + WebMessage 热切换"双通道。**
  - 首次导航 URL 带 `?exbarTheme=dark|light`，`index.html` 在 CSS 加载前把它写成
    `document.documentElement[data-theme]`，杜绝首帧白/深色闪烁。
  - 运行中 `App::ApplyTheme()` → `aiui::Panel::SyncTheme()` → `WebView::SetTheme()`，
    通过 `ICoreWebView2::PostWebMessageAsJson`（vtable 槽位 32）下发
    `{"type":"exbar.theme","theme":"dark|light"}`，页面只换 `data-theme`，**不导航、不丢聊天上下文**。
  - 调用前显式 `put_IsWebMessageEnabled(TRUE)`（Settings 槽位 6）。手写 vtable 为此从
    `ICore` 槽位 17 一路补到 32；中间未调用的方法只作占位，顺序按官方 IDL。
- **WebView2 子树登记为"文本输入区"宿主。**
  - `App::InitAiPanel()` 把 `aiPanel_.hwnd()` 同时交给 `kbhook::SetTextEntryRoot()` 和
    `InputRouter::SetTextEntryRoot()`；`OnDestroy()` 成对清除。
  - 钩子侧：焦点在宿主或其子窗口里时 `FocusIsOurs()` 返回 false，字符键不再被吞掉投递，
    输入法照常工作。
  - 路由侧：焦点在宿主子树时按 `inEdit` 处理，裸键不启动序列；若残留候选弹窗仍开着，
    先 `Cancel()` 再放行，避免"弹窗已经打开 → 打字继续被吃"。

代价：手写 WebView2 绑定从 25 个槽位扩到 32 个（仍不引入 2 万行 SDK 头），
CSS 增加一份浅色语义令牌；两者都只影响 AI 面板，文件视图行为不变。

验证口径：切主题后面板主色应出现浅色令牌（如 `#ffffff` / `#f3f3f3` / `#0078d7`）；
在聊天输入框连续输入 `g x y` 不应出现 `ExBarBrowser_SeqOverlay` 窗口，且字符完整进入输入框。

## D15 高频意图走本地直调，不先问模型（2026-11-23）

**背景**：D13 把模型客户端搬进本工程后，所有自然语言请求都先走一轮模型推理。实测
「上一级」「打开下载」「按大小排序」「搜索 发票」这类请求也要等模型想半天，然后再说一堆；
它们的意图与结构化命令本来就是一一对应的，模型往返只是延迟与噪声。

**决策**：在 `aichat/service/agent-stream.js` 的**第一次模型调用之前**插入本地意图匹配
（`aichat/service/intents.js`）。命中即按 `{name, args, reply}` 直接 `dispatch()` 到结构化命令，
发一个工具卡与一句回复，`steps=1`、`usage=null`，立即结束本轮。未命中照常进入模型循环。

准入条件只收三类：

1. **无歧义**：一句固定话术只对应一条命令；「桌面」不能同时是路径和书签。
2. **无破坏性**：不删、不移、不改文件；导航 / 视图 / 选择 / 搜索 / 标签开关可以。
3. **非分析型**：「Pictures 里什么最占空间？」需要读目录再判断，留给模型。

搜索的关键词原样透传（`search.find {"query":...}`），不做本地改写。提示词同步收紧：
首轮就调准确命令、不为了「想清楚」先列工具、最终回答 1–2 句、不暴露推理；参数依命令表
真实 schema（`view.mode` 用 `preset`，`view.sort` 用 `by`）。

UI 的 11 条空态建议与 `/` 菜单的 22 条快捷话术都复用这张意图表；新增建议时若话术没被
`intents_test.js` 覆盖，就不会获得「秒回」效果，这是刻意的约束。

**后果（正）**：高频操作从「一轮模型往返 + 一段推理」降到「一条本地匹配 + 一次管道调用」；
回复长度固定；模型 token 不消耗。

**后果（负）**：意图表是一份有限白名单，话术变化（如「回到父目录」）会漏命中并退回模型；
每加一条规则都必须同步加纯函数单测，不能靠模型兜底掩盖规则写错。

**被否的替代方案**：① 让模型先输出意图分类再调命令 —— 多一轮往返，正是要消除的延迟；
② 在 WebView2 前端直接发管道命令 —— 会绕过服务的审批门 / guard / 活动流，安全语义分裂；
③ 把所有自然语言都做成规则表 —— 会吞掉需要判断与确认的复杂请求，不可接受。

**复检条件**：若模型首 token 延迟降到低于本地匹配 + 管道调用的耗时，或命令表新增了大量
同样无歧义的高频命令，应重新评估这张表是否还有存在价值；届时先量端到端时延，不凭直觉。

## D16 项目知识前置：常驻卡 + 本地召回，取代现场翻文档（2026-11-23）

**背景**：D15 解决的是"固定话术秒回"。但用户问"R21 是什么""AI 面板入口在哪"
"P4-a 做到哪了"这类**项目事实**时，模型仍然要靠 `tool.list` / 读 `AGENTS.md`
现场找答案：多一轮工具往返、片段每次不同、还容易把过期博客当真。

**决策**：在 `aichat/service/agent-stream.js` 与 `agent.js` 第一次模型调用前，
把仓库文档切成 section 索引注入 system message：

1. **常驻卡**：从 `AGENTS.md` / `ARCHITECTURE-RULES.md` / `DECISIONS.md` /
   `ROADMAP.md` / `CHANGELOG.md` 抽硬规则、关键决策与当前状态，压到 7600 字以内。
   R21、零新依赖、D13/D14/D15、P4 状态永远在同一轮 prompt 里。
2. **本地召回**：按当前提问做中文 bigram + ASCII 词命中，补最多 3 个相关 section
   （2400 字以内）。命令参数仍以 `TOOL_SCHEMAS` 为唯一真源，文档片段不覆盖工具契约。
3. **mtime 缓存**：索引按源文件 `mtimeMs + size` 失效；改文档后下一轮自动重读，
   不改文档则零重复 IO。纯 Node，无新依赖、不写文件。
4. **prompt cache 分段**：system message 用内部标记分成「稳定前缀 / 每轮易变段」。
   Anthropic 线路把稳定前缀转成带 `cache_control: ephemeral` 的 text block；
   OpenAI 兼容线路抹掉标记、无缝拼回，避免 NUL 进入严格 relay 的请求体。

**后果（正）**：项目事实类问题不再触发"翻说明书"式工具调用；规则与状态在多轮之间
保持一致；文档改动能自动进入后续回答；Anthropic 端点上稳定前缀可命中缓存。

**后果（负）**：常驻卡每轮最多约 2k tokens。缓存命中前仍按原价计费；不支持
prompt cache 的端点只受益于"不再现场翻文档"，不受益于缓存折扣。若卡继续膨胀，
优先删常驻项、把细节留给召回，而不是放宽上限。

**被否的替代方案**：① 微调模型把文档塞进权重 —— 成本高、更新滞后，且无法回读源码真值；
② 每轮让模型读一遍 `AGENTS.md` —— 正是本决策要消掉的延迟与 token；
③ 把文档全文塞进 system —— 会淹没工具 schema，且工具参数会与文档漂移。

**复检条件**：量一次项目事实类提问的首 token 延迟与 `usage.prompt_tokens`，与
"模型自己调 `tool.list` 再答"的路径对照；若常驻卡带来的 token 增量超过省下的工具往返，
就收缩常驻卡并加大召回权重。

## D17 本地 HTTP 不是可信边界：token + Host/Origin + 回合快照（2026-11-23）

**背景**：AI 面板的页面跑在 `127.0.0.1` 的 Node 服务上。早先的假设是"loopback
只有自己能访问，所以不用鉴权"，但这挡不住 DNS rebinding，也挡不住本机任意网页
用 `fetch` / 表单驱动 `/api/chat`、`/api/approve`。同时 `server.js` 每个请求都会
`reloadConfig()`，一个回合跑到一半时用户切模型，后半段会换参数。

**决策**：

1. 原生侧每次启动 node 生成 16 字节随机 `EXBAR_HTTP_TOKEN`，拼进 WebView 的首个
   URL；前端读 `exbarHttpToken` 后给所有 `/api/*` 请求加 `X-ExBar-Token`。
2. 服务端校验 `Host` / `Origin` 必须是同一 loopback 地址与同一端口；`/api/*`
   除 `/api/health` 外都必须带 token，`/agent` 同样要求。静态首页免 token，
   但没有令牌时 UI 的 API 调用会给出明确提示，而不是静默空白。
3. JSON body 上限 256 KiB；SSE `emit` 进入写队列，`res.write()` 返回 false 时等
   `drain`，写失败直接 abort 上游模型。每个事件带 `run`（UUID）与 `seq`，
   前端已按 `turnId` 挡旧事件，现在还能挡同一回合内乱序/重复的帧。
4. `agent-stream.js` 在回合开始对 `config` 做浅快照，`callModelStream` /
   `maxSteps` / `verbose` 全部读快照；工具名的流式分片改为按"完整值 or 片段"
   合并，不再用覆盖，避免中转重发时只剩最后一个字。
5. WebView2 注册 `NavigationCompleted`，页面自 reload 后重新 `PostWebMessageAsJson`
   下发主题；控制器代际、loader 绝对路径优先、异步失败回调与可重建 Retry
   一并收口。

**后果（正）**：跨站页面与 DNS rebinding 无法驱动本地命令；慢客户端不会把整段
SSE 堆在内存里；用户在设置页改配置不会撕裂正在进行的回合；页面 reload 不再丢主题。

**后果（负）**：手工在浏览器输入不带 token 的地址只能看静态壳，API 会返回 401；
诊断要用面板里给出的完整链接（链接本身含 token）。直接 `node service/server.js`
调试时若无 `EXBAR_HTTP_TOKEN`，仍保留无 token 的兼容退路，不用于嵌入路径。

**被否的替代方案**：① 只绑 loopback —— 已证明不足以挡 rebinding；
② 把 token 写进 localStorage —— 同源任意脚本可读，且跨服务重启不失效；
③ 用 PostMessage/SendMessage 模拟标题栏拖动 —— 违反 R21 输入纪律，留给后续
真实指针捕获方案，不在这次改动里假实现。

**复检条件**：`server_security_test.js` 覆盖 Host/Origin/token/body 上限；
若将来允许非 loopback 访问，必须重新设计 token 分发与 Origin 白名单，不能简单放行。

## D18 回合是身份，确认是事实（2026-11-23）

**背景**：活动流与内联台账都按 `turn` 分组，但旧服务端从历史里的 user 条数推算
轮号；历史一到 `MAX_HISTORY_MSGS` 就截断，`/api/clear` 又清空文件，轮号会回退并
把两次不同对话合并。同一 session 的两个 `/api/chat`（一个停在审批卡，一个已开始）
各自 `loadHistory()` 后再写回，后写会静默抹掉前一回合。前端则没有事件归属：用户
点停止后，旧流的 `done/error` 仍能把新回合的 busy、审批卡和回答一起清掉。
确认语义上也有两处说谎：`undoable` 被当成免确认的理由；Web 面板的「自动执行」
既不弹危险审批，又不会把 `approve=true` 传给宿主，结果是危险命令必然失败，
但用户看到的模式名却像"都能自动做"。

**决策**：

1. **回合号是持久身份，不是历史计数**。`service/turn-state.js` 用 `<session>.turn.json`
   记录 `nextTurn`；无元数据的旧会话从历史里最大 `turn/turnId` 续号。截断与清空
   都不回退。服务端在拿到会话串行锁后才分配号码，并给每个 SSE 事件带上
   `turn` / `turnId`，历史条目也落盘同一身份。
2. **同会话整段单飞**。`/api/chat` 从读历史到写历史都在同一 promise 链里；不同
   session 不互相阻塞；排队期间客户端断开则放弃，不消耗回合号。异常必须释放锁。
3. **事件有 owner**。`ui/app.js` 每次 `send()` / `stop()` 推进 `generation`，
   `start` 事件写入服务端 `turnId`；处理事件前双身份校验。旧 generation 或旧 turn
   的 `done/error/tool/approval` 一律丢弃，连 EOF 警告也不写。
4. **可撤销不是免确认**。`CommandBus::NeedsConfirm()` 只认 `destructive` 与显式
   日常白名单；`undoable` 仅表示"错了能补救"，不表示"丢了无所谓"。删书签、清历史、
   覆盖导入都要先问。
5. **模式只改普通写，不改危险门**。confirm 问所有写；auto 免普通写但危险命令仍弹
   审批卡，用户点确认后才带 `approve=true`；readonly 拦所有写。会话级免确认
   **永远不是**单次人工批准事实，宿主侧 `aiAllowDanger + approve` 双门保持最终裁决。
   未知工具在所有模式都 fail closed。

**后果（正）**：刷新、截断、清空之后回合分组稳定；同会话不再丢回合；停止/新回合
不会被旧事件污染；危险命令在 auto 模式下是真能执行（经用户当次确认），而不是
"看起来自动、实际被宿主拒绝"；只读模式不再悄悄跑普通写。

**后果（负）**：auto 模式下危险命令仍需点一次确认，省掉的那次点击本来就不该省；
删除/清空这类可撤销操作现在会多一次确认，换来的是事前知情权。`.turn.json` 是
新落盘文件，删除它会从当前历史最大值重新续号，历史也被清空时才会回到 1。

**被否的替代方案**：① 继续用历史条数推算 —— 与截断语义天然冲突；
② 让 auto 模式自动 `approve=true` —— 把会话偏好说成单次人工批准，宿主双门会被架空；
③ 只靠前端 busy 防并发 —— 多个窗口/标签页同时打开面板时仍有真实竞态。

**复检条件**：`turn-state_test.js` 覆盖截断/清空续号、同会话串行、异常释放锁；
`verify_turn_guard.js` 覆盖旧 generation/旧 turn 的 `done/error/tool`；
`approval-policy_test.js` 覆盖三模式边界；`commands_confirm_test.cpp` 覆盖
`bmk.remove` / `bmk.clear` 这类 destructive+undoable 仍要确认。

---

## D19 WebView2 页面 → 宿主只走窄动作白名单（2026-11-24）

**背景**：AI 页自带一条仿窗口标题栏，按钮通过 `window.chrome.webview.postMessage`
发给宿主；但原生侧没有 `WebMessageReceived` 消费者。结果是最小化、关闭、置顶
全是空点击，更糟的是任何页面脚本都能借这个通道请求宿主动作。面板本身是主窗口
的**子窗口**，不具备独立拖动语义；页面发的 `drag` 也无法安全转成顶层窗口拖动。

**决策**：

1. WebView2 绑定补齐 `ICoreWebView2` 到 `remove_WebMessageReceived` 的 vtable
   槽位，显式打开 `IsWebMessageEnabled`，在首次导航前注册处理器；代际门闩保证
   `Stop()` / `Retry()` 后旧回调不能碰新实例。
2. 消息必须同时通过三道闸：来源以启动时 URL 的 `origin` 开头（且下一字符是
   `/`、`?` 或结束）；`type == "window"`；`action` 在
   `minimize / close / maximize / pin / unpin` 白名单里。其余消息静默丢弃。
3. 宿主动作在 `App` 层落地：`close` 是**收起 AI 面板**并触发 `Layout()`，不是退出
   程序；`minimize` / `maximize` / `pin` / `unpin` 操作主窗口，全程不抢焦点、不动
   光标。`drag` 不实现，前端在 WebView2 模式下也不再发送。
4. 浏览器直开（无 `chrome.webview`）时隐藏这些按钮，避免留下一排无意义的控件。

**后果（正）**：标题栏按钮不再"点了没反应"；页面只能发起五类窄动作，不能借
web message 驱动任意宿主命令；浏览器退路不再暴露无效按钮。

**后果（负）**：面板不能像独立窗口那样从页面标题栏拖动；这符合 D12 的"右侧一列"
形态，不把它伪装成浮动窗口。

**被否的替代方案**：① 让页面直接改 `window.location` 或写 HTTP 端点触发动作 ——
绕过来源校验且把 UI 意图混进服务协议；② 在原生侧实现 `drag` 消息转发 ——
WebView2 会吞掉指针事件，得到的是不可靠的坐标序列，且容易引入 R21 风险。

**复检条件**：`webview_actions_test.cpp` 覆盖白名单、JSON 解析、来源边界；
`verify_window_actions.js` 覆盖 WebView2 模式只发白名单动作、浏览器直开隐藏按钮；
新增 vtable 槽位后构建与全量链接必须通过。

---

## D20 撤销组先定元数据契约与纯模型（2026-11-24）

**背景**：AI 一次任务常是「复制 → 重命名 → 移动」这类多步写。当前
`fileundo::Stack` 是一条记录一步撤销，书签又是另一套栈；若直接把 AI 工具接到
现有栈，用户要按多次 Ctrl+Z 才能撤掉一次逻辑任务。另一方面，`undoable` 只说明
事后能补救，不说明这些步骤属于同一域、能由机器合成一个撤销计划。

**决策**：

1. **元数据驱动，命令名不驱动**。宿主在 `tools/list` 的 annotations 中给出
   `undoGroup` / `undoDomain`：命令有 `effects.undoable` 且分组为 `File` 或
   `Bookmark` 时，分别声明 `join` + `file` / `join` + `bookmark`；其他命令即使
   可撤销也只给 `none`。`tab.close` 这类无同域栈的命令不得虚报 `join`。
2. **Node 侧先做纯数据模型**。`service/undo-groups.js` 只校验组 ID、域、步骤准入、
   关闭状态和逆序撤销计划；不发命令、不写文件、不碰 UI。可加入的步骤必须同时
   满足 `undoable === true`、`undoGroup === "join"`、域匹配；每组最多 20 步、
   `undoRef` 唯一。
3. **撤销计划是逆序快照，不是命令重放调用**。`UndoPlan()` 输出逆序的
   `undoRef` 列表；失败关闭的组不生成计划。机器代撤遇到任一步 `byMachine=false`
   时整组拒绝，不做「撤一半」，沿用现有 fileundo 的人机边界。
4. **本次只接契约，不改现有栈行为**。真实 `fileundo::Stack` / 书签栈的职责边界、
   失败补偿和 UI 文案都还没定；先把可测的准入与计划模型钉住，避免把半成品
   撤销语义塞进正在使用的栈。

**后果（正）**：AI 工具列表不再让客户端猜哪些写能组成一次撤销；组模型可单测；
后续接栈时只需消费已验证的计划，不必重新设计 ID、域和人机边界。

**后果（负）**：当前 AI 多步写仍未真正合并为一次 Ctrl+Z，这是有意延后，不是
已经交付的用户可见能力。新增 annotations 是协议面扩展，旧客户端忽略即可。

**被否的替代方案**：① 按命令名前缀（如 `file.`）硬编码分组 —— 会和 `tab.close`
这类 undoable 但无同域栈的命令冲突；② 现在就把 AI 步骤逐个压进
`fileundo::Stack` —— 会把「一次任务」错误映射成「N 次撤销」；③ 允许机器撤人工步骤
—— 越过 D18 的人工批准事实边界。

**复检条件**：`ai_api_test.cpp` 钉住 `fs.*` / `bmk.*` 的域映射与 `tab.close`
不 join；`undo-groups_test.js` 覆盖 ID/域校验、准入、逆序计划、失败关闭、
20 步上限和人机边界；接入真实撤销栈时必须另开决策记录。

---

## D21 活动流与回答台账共享服务端 turnId（2026-11-24）

**背景**：服务端已经给每个 SSE 事件写 `turnId`，但结构化活动流用 `ev.turn`
分组，回答内联台账只在每张工具卡里被动接收事件；回答气泡自身没有权威身份。
结果是活动流能显示「第 N 轮」，却没有稳定办法定位回那条回答；历史恢复时
若旧数据同时带冲突的 `turn` / `turnId`，两个视图可能取到不同轮号。

**决策**：

1. **单一读取口径**：前端统一用 `turnIdOf()` 取身份，`turnId` 优先、旧 `turn`
   仅兼容回落；所有非正值都视为无身份。事件归属、活动流分组、历史重建都走
   这一个函数。
2. **回答是身份的宿主**：assistant 消息与 `.step-ledger` 同时写
   `data-turn-id`；实时流由 `start` 事件写入，历史恢复由落盘条目写入，工具
   事件到达时再补一次。台账头部显示「第 N 轮」，避免在两处各自推算。
3. **活动流轮次可反查**：活动流分组保存同一 `turnId`，轮次标题渲染成按钮；
   点击后切回聊天视图、按 `data-turn-id` 找到回答、展开它的台账并滚动过去。
4. **旧数据保守处理**：只有旧 `turn` 的历史仍可被分组和展示，但不生成可点击
   的轮次按钮；不把消息条数当成服务端身份。

**后果（正）**：两个视图对“同一轮”不再各说各话；活动流从审计视图变成可导航
入口；为后续重试/分支提供稳定的 UI 锚点。

**后果（负）**：DOM 多了一个身份属性和一个轮次按钮；历史里没有 `turnId` 的
旧会话无法享受反查，这是数据缺失，不是前端能补出来的事实。

**被否的替代方案**：① 按时间戳或数组下标定位回答 —— 刷新、截断、并发回合
都会漂移；② 让活动流重新发一次 `/api/history` 查身份 —— 多一次往返且仍可能
与当前 SSE 竞争；③ 把两个视图合并成一个 —— 会丢掉审计时间线和内联回答各自
的阅读场景。

**复检条件**：`verify_turn_identity.js` 覆盖 `turnIdOf()`、气泡/台账 DOM 身份、
活动流分组、历史恢复、`start` 事件写入和轮次反查；原有 `verify_turn_guard.js`
16 项仍须全过，确保共享身份没有削弱旧事件隔离。

---

## D22 重试是分叉新回合，不覆盖旧尝试（2026-11-24）

**背景**：失败/停止的回合需要重试，用户也常要改一句提示词再问。若直接改旧历史，
审计链断了；若把旧回答一起喂给模型，重试就会变成“基于失败答案继续”，既不
符合用户预期，也会浪费 token。Pulse 的 versioned turns 提供了更完整的分支切换，
但本项目还没有稳定的 UI 分支模型，不能一次补齐全部语义。

**决策**：

1. **新回合有全新权威 `turnId`**。`retryOf` 只指定分叉点；服务端读取旧回合的
   `rootTurnId`，把当前最大 `attempt + 1` 作为新尝试号。新回合的 user/assistant/
   tool 记录都落 `retryOf / rootTurnId / attempt`，SSE 事件同步携带。
2. **旧回合不被删除或改写内容**。落盘时只给同 root 的旧记录加 `supersededBy`
   指向新回合；历史仍保留全部尝试，供回看与后续分支选择。
3. **上下文只取活动分支**。普通 `send` 先按 root 聚合，只保留最大 attempt 的
   回合；`retryOf` 请求取旧用户条目之前的历史，再套同样的活动分支过滤。被
   取代的旧尝试不会借普通发送或重试复活。
4. **无效分叉点直接拒绝**。目标不存在时返回 400，不分配回合号、不启动模型。
5. **UI 先接最小闭环**。失败/停止回合显示“重试 / 编辑重试”：前者用原提示词
   直接发新回合，后者预填输入框并把下一次 send 标成 `retryOf`。活动流显示
   尝试号与“旧版本”；按 `turnId` 反查仍走 D21。

**后果（正）**：重试/编辑可审计、不覆盖历史；模型上下文只包含当前有效分支；
为后续“选择某个旧版本继续”留出了 root/attempt 坐标，而不是靠消息下标猜。

**后果（负）**：历史文件会保留失败的旧尝试，体量增长更快；当前 UI 只能标记旧
版本，还不能切换到任意旧分支继续，这属于下一步，不伪装成已交付功能。

**被否的替代方案**：① 截断分叉点之后的历史 —— 简单但丢失审计与回退能力；
② 把旧失败回答也塞进上下文让模型“自我修正” —— 语义不是重试，且继承错误；
③ 用数组下标当分叉坐标 —— 与 D18/D21 的权威 turnId 冲突，截断后必然漂移。

**复检条件**：`turn-versions_test.js` 覆盖活动分支、血缘递增、分叉上下文、
旧版本过滤与 `supersededBy` 标记；`server_security_test.js` 钉住
`historyForChat()` 对普通 send / 有效 retry / 无效 retry 的三种结果；
`verify_turn_versions.js` 覆盖 UI 的 retryOf 请求、编辑预填、版本标记与活动流展示。

---

## D23 活动分支是持久选择，父链决定上下文（2026-11-24）

**背景**：D22 默认把最大 attempt 当活动分支。用户切回旧版本后如果还按最大
attempt 计算，下一次发送会立刻跳回新版本；而只改 root 的选中回合也不够——
挂在新版本之后的普通回合仍会被带进上下文，形成“旧版本 + 另一分支后半截”的
混合对话。

**决策**：

1. **选择是状态，不是推导**。`service/branch-state.js` 按会话保存
   `rootTurnId -> selectedTurnId` 到 `<session>.branches.json`；`activeHistory()`
   接受 selection，默认仍取最大 attempt，显式选择优先。`/api/clear` 同步清空。
2. **每个回合记录父节点**。新回合落 `parentTurnId`：retry 复用分叉目标的父节点，
   普通 send 挂当前上下文最后一个回合。旧历史没有该字段时按“父未知即保留”
   处理，避免截断后整段消失。
3. **父链是过滤边界**。活动分支只启用“root 选中回合”且父节点已启用的回合；
   未选中尝试的后代——即使属于别的 root——一并排除。这样切回旧版本不会继承
   另一条分支的后继对话。
4. **切换是显式 API**。`GET /api/branches` 返回 root/attempt/selected，
   `POST /api/branch` 在会话锁内校验 root 与 turn 同属一条血缘后写选择；跨 root
   或不存在的回合返回 400。活动流版本组提供“切换”按钮，成功后重载历史。
5. **新回合自动 become active**。retry 与编辑重试生成的新 turn 会把对应 root 的
   选择更新为自己，不需要用户再点一次切换。

**后果（正）**：旧版本可真正继续对话；上下文不再混入被舍弃分支的后代；
选择跨刷新保持，跨会话隔离。

**后果（负）**：多一个 `.branches.json` 状态文件；历史截断掉父节点时，父链过滤
会退化为“父未知即保留”，这是数据不足下的保守行为，不是完整血缘重建。

**被否的替代方案**：① 每次都取最大 attempt —— 切换无法持久；
② 切换时删除其他分支 —— 破坏 D22 的审计与回看；③ 只按 root 过滤、
不看 parent —— 会拼出混合上下文，正是本次要修的缺陷。

**复检条件**：`branch-state_test.js` 覆盖读回、清除与非法参数；
`turn-versions_test.js` 覆盖分支列表与选择校验；`server_security_test.js`
钉住默认最大 attempt 与切回旧版本后的父链收窄；`verify_turn_versions.js`
覆盖 UI 切换请求与反馈。

---

## D24 面板收起不是消失：状态源独立于 chip，peek 只呈现结果（2026-11-24）

**背景**：AI 面板默认收起，WebView2 宿主只是隐藏而不是销毁，SSE 回合仍会
在后台跑完。旧实现没有任何收起后的落点：用户只能重新展开面板才知道命令
是否完成、是否需要确认、能否撤销，这与"后台任务 + 需要时瞄一眼"的交互模型
相反。

**决策**：

1. **状态来自页面事件，不来自面板可见性**。`ui/app.js` 在 `start/status/tool/
   approval/done/error/stop/newChat` 上用 `postMessage({type:"status"})` 上报；
   `WebMessageReceived` 沿用 D19 的 origin 校验，新增状态白名单
   `idle/busy/approval/done/error`、摘要/详情长度上限与撤销域白名单。
2. **状态栏 AI chip 常驻**。新增 `Seg_Ai`，由 `SetEnabledSegments()` 强制补入，
   不跟随 `statusSegs` 的显隐；状态文本由异步事件直接写入，
   `RefreshStats()` 重建 `StatusModel` 时必须带回原值。这样"关掉文件统计段"
   不会把后台 AI 的运行状态一起藏掉。
3. **peek 是轻量呈现层**。`ComputeAiPeekLayout()` 以 chip 屏幕矩形为锚点，
   优先向上展开、越屏夹回工作区；气泡只有"打开对话 / 撤销 / 关闭"三个动作。
   点击 chip 切换 peek，点"打开对话"展开面板并收起 peek；窗口布局变化时直接
   收起，避免锚点漂移后气泡还挂在旧位置。
4. **撤销必须是真承诺**。工具事件只有 `status=done` 且 `undoGroup=join` 且
   `undoDomain` 属于 `file/bookmark` 才写入 `state.lastUndo`；宿主侧再做一次
   白名单校验。peek 的撤销按钮仍按 `Actor::Ai()` 派发，保留"机器撤销不穿透
   人工操作"的边界。
5. **面板展开时不弹 peek**。结果已经直接可见，气泡再叠一层只是遮挡；状态仍
   更新 chip，用户收起后仍能看到最后一次结果。

**后果（正）**：收起面板后仍有完成摘要、待确认提示和撤销入口；状态通路与
面板生命周期解耦，WebMessage 的安全边界仍集中在一处；chip 常驻避免用户
以为"AI 没在运行"。

**后果（负）**：多一个 owner 弹层与一个状态栏段，状态栏有固定占位；
`statusSegs` 不再能隐藏 AI chip；撤销按钮只覆盖已有撤销栈的两个域，
其它域即便工具声明 `undoGroup=join` 也不会给假承诺。

**被否的替代方案**：① 用 tooltip 显示结果 —— 不能放动作按钮；
② 把 peek 塞进 WebView 页面 —— 面板收起后随宿主一起不可见；
③ 在 `RefreshStats()` 里轮询服务状态 —— 事件已经送达，轮询只会增加延迟
和 UI 线程成本。

**复检条件**：`webview_actions_test.cpp` 覆盖状态白名单、撤销域和长度裁剪；
`ai_status_peek_test.cpp` 覆盖锚点、越屏夹取和按钮不重叠；
`status_bar_test.cpp` 钉住 AI chip 的图标口径；`verify_ai_status.js` 覆盖
`postHostStatus()` 与 done 事件的摘要/撤销上报。

---

## D25 未读是用户动作的结果：终态点亮，展开或消费 peek 才清（2026-11-24）

**背景**：D24 让面板收起后还能看到状态与 peek，但"自动弹一次气泡"既不等于用户
看到了结果，也没有留下可回看的标记。用户离开电脑一会儿回来时，状态栏只显示
最后一次状态，无法区分"这是刚到的新结果"还是"我早就看过的结果"。

**决策**：

1. **未读只由终态结果产生**。`approval / done / error` 在面板收起时点亮；
   `busy / idle` 与未知状态不产生未读。判定口径 `ShouldRaiseAiUnread()`
   与 chip 文案 `AiChipText()` 是纯函数，避免 UI 与测试各写一份。
2. **自动呈现不算消费**。页面上报终态时仍会按 D24 自动弹 peek，但未读圆点保留 ——
   用户可能不在电脑前。用户点 chip（展开或收起 peek）、点"打开对话"、
   点"撤销"或点气泡关闭按钮，才调用 `ConsumeAiUnread()` 清掉圆点。
3. **展开面板即已读**。任何路径把面板打开（`Ctrl+J` / peek 的打开对话 / 命令）
   都在 `ToggleAiPanel()` 里消费未读；面板可见期间到达的新状态直接清未读，
   不制造"面板开着却说有未读"的自相矛盾。
4. **未读是可回读的观测面**。`ai::StateSnapshot::aiUnread` 与 chip 圆点同源，
   `StateToJson()` 输出 `aiUnread`。没有 `ai.unread.clear` 命令：它不是可写状态，
   只有真实用户动作能清，AI 不能替用户宣称"已读"。

**后果（正）**：收起期间的新结果有明确标记；自动弹出不会偷走未读状态；
UI、快照与测试共用同一套终态/文案口径。

**后果（负）**：多一个 bool 状态与一个状态快照字段；重复收到同一条终态事件
会重新点亮未读（真实事件流里由回合身份去重，重放场景由消费动作兜底）。

**被否的替代方案**：① 自动弹 peek 时就清 —— 用户不在场时结果与未读一起消失；
② 用全局计数器代替 bool —— 状态栏只需"有没有"，计数会把简单判定复杂化；
③ 在 AI 侧提供清除命令 —— 机器不能替人确认"看过了"。

**复检条件**：`ai_status_peek_test.cpp` 覆盖终态判定、面板可见性门闩与圆点文案；
`ai_status_peek.cpp` 的关闭按钮回调与 `App::ToggleAiPeek()` 覆盖用户消费路径；
`StateToJson()` 的 `aiUnread` 字段用于回读核对。

---

## D26 工具回退只重探连接，不绕过边界（2026-11-24）

**背景**：D13 之后 AI 通过命名管道操作用户眼前的窗口，D14/首轮预检又保证了第一次模型
请求前完成握手。但并行开发时程序频繁重启：旧管道刚退场、新管道尚未建好，第一次
`tools/list` 会失败或返回空表。旧行为直接进入"本轮没有工具"，模型只能写字，明明
稍等一下就能拿到命令表。与此同时，工具入口的 `denyCommands / dryRun / allowedRoots`
与危险确认都挂在 `dispatch()` 上；任何"自动重试整轮"都可能把一次被拒命令重放成真执行。

**决策**：

1. **只回退一次连接，不回退工具执行**。`EnsureReadyWithFallback()` 在首轮失败或工具表
   为空时等待一小段固定时间，重置连接后再试一次，返回
   `{ ok, tools, retried, reason, cancelled }`。模型调用仍只发生在预检之后。
2. **恢复与降级都要说清楚**。重探恢复时向 SSE 发"工具桥已恢复"，首条状态带上恢复信息；
   持续失败时发"未连接 / 已回退重探一次"，并在 system prompt 明确写
   `no tools this turn`，不假装能操作。
3. **旧工具清单不得残留**。连接失败时清空 `TOOL_SCHEMAS`，避免模型拿着上一会话的命令
   清单回答"我有工具"；`CapabilityNote()` 在工具数为 0 时不再输出"已连接"提示。
4. **安全边界不因回退改变**。`guard.Check()` 仍在 `dispatch()` 最前面，未知工具仍
   fail-closed 为需要确认；回退过程不调用 `dispatch()`、不发宿主命令，也没有
   "因为重试过所以自动批准"的捷径。
5. **离线测试钉住口径**。`tools-fallback_test.js` 覆盖持续失败、重探恢复、deny 与
   未知工具审批；`agent-preflight_test.js` 覆盖 `ensure → retry → ensure → fetch`
   的顺序和状态/提示词文案，两者都纳入默认 `npm test`。

**后果（正）**：程序重启窗口期内首轮就能恢复工具，不需要用户重发提示词；持续的
连接失败有明确状态与机器可读 prompt 口径；回退不会扩大权限面。

**后果（负）**：每个真正没有桥的回合会多等约一次固定延迟；工具表为空被视作需要重探的
异常，可能把"宿主本意不暴露工具"误判成暂时故障（当前宿主契约下不使用这种语义）。

**被否的替代方案**：① 在模型调用失败后再补工具并重跑整轮：会把已经产生副作用或
审批结果的操作重放；② 无限重连：重启间隙会变成不可控等待；③ 在回退里自动执行
"发现动作"类工具：等于让容错逻辑拥有工具执行权，破坏 D17/D18 单一出口。

**复检条件**：`npm test` 中的 `tools-fallback_test.js` 与 `agent-preflight_test.js`
必须通过；若将来宿主允许合法的空工具表，需要重新定义"空表要重探"的语义。

---

## D27 页面未上报前，状态栏显示宿主原生启动态（2026-11-24）

**背景**：面板占位页已经能画"准备中 / 创建中 / 加载界面"，但状态栏 chip 的
权威状态只来自页面 WebMessage。WebView2 环境创建前、页面脚本尚未加载、或
运行时缺失时，chip 一直显示"空闲"：用户既看不出 AI 通道正在启动，也看不出
它已经失败。Pulse 的对照也指出，面板之外的常驻入口需要一个不依赖页面脚本的
最低状态。

**决策**：

1. **新增宿主原生态 `starting`**。显示名"准备中"，由 `Panel` 的生命周期回调
   驱动：展开面板、服务未启动、WebView2 创建中、手动 Retry 都落到这一档；
   失败落到 `error`，页面就绪但尚未上报时落到 `idle`。
2. **真实页面状态优先**。`App` 记 `aiPageStatusSeen_`：第一条合法 WebMessage
   到达后，原生态不再回写 chip，避免"页面已经完成，宿主又改回准备中"。
   `starting` 会把这个标志清零，因为 WebView2 重建意味着旧页面状态失效；
   后来的原生 `error` 是真实新结果，允许覆盖。
3. **页面不能伪造启动态**。`starting` 不进 `IsWebViewStatusState()` 白名单；
   页面发来的 `starting` 与未知态一样被归一成 `idle`。启动态只由宿主生命周期写入。
4. **不制造未读**。`starting` 与 `busy` 一样是过程态，不点亮 chip 圆点；
   原生失败走既有 `error` 口径，收起时会点亮未读。
5. **状态对外可回读**。`ai::StateSnapshot::aiState` 与 chip 同源，
   `StateToJson()` 输出该字段。AI 读回时看到的就是用户看到的状态。

**后果（正）**：面板还没加载完时，状态栏不再假装空闲；WebView2 缺失时用户
在状态栏就能看到失败原因入口；原生态与页面态的优先级有明确规则，不会互相踩。

**后果（负）**：多一个状态值与一个 bool 观察标志；页面刚就绪到第一条 status
之间会短暂显示"空闲"，这是有意的 —— 宿主不知道页面内部的准备工作。

**被否的替代方案**：① 让页面自己发 `starting`：脚本还没加载时消息根本发不出来；
② 用 `busy` 代替：语义上是"AI 正在执行"，启动失败时说不通；
③ 只更新占位页不改 chip：用户收起面板后仍看不到任何启动进度。

**复检条件**：`ai_status_peek_test.cpp` 钉住 `starting` 文案、非终态与原生状态解析；
`webview_actions_test.cpp` 钉住页面不能伪造 `starting`；
`ai_api_test.cpp` 与快照回读确认 `aiState` 与 chip 同源。
