---
status: current
baseline: 2026-11-21（第三轮复核）
method: 全网检索（2026-11-21）+ 本仓代码实测 + 前两版结论逐条重验
上一版: 2026-11-21 22:40 第二版
---

# 外部对标与缺口报告（第三轮）：人机协作 / AI 操作软件 / 自我迭代

> 本文回答：与全网同三类项目相比，ExBarBrowser 的 AI 接口层**还差什么、哪里写错了**。
>
> 结论分三类，**不混在一起**：
> - **错误** = 本仓内部矛盾或真缺陷（附文件 + 符号/行号证据）
> - **缺失** = 相对成熟项目的能力空白
> - **有意取舍** = 明确的设计选择，**不算缺陷**
>
> 判定基线：本轮实跑三个校验器 + **用校验器自己的解析器**（`check_ai_meta.parse_commands()`）
> 导出 173 条命令的完整元数据做交叉比对，而不是靠正则抽字段。
> 凡文档声称与实测冲突，以**实测**为准。

---

## 0. 第三轮复核摘要（先读这一节）

前两版是从"**目标**"往下比（应该有什么）。这一轮换了方向：从"**声明**"往两边比 ——
**命令表声明了什么 ↔ 校验器认定了什么 ↔ 代码实际做了什么**。三角不等的地方就是缺陷。

结果：**发现 1 类前两版完全没看见的 P0 级缺陷（人机判据失真，7 种 actor 被判成机器）**，
以及**一个前两版数字严重低估的覆盖面问题（45 条而非"6 条缺口"）**。

### 0.1 本轮新的三类发现

| 类型 | 发现 | 为什么前两版看不见 |
|---|---|---|
| **新 P0** | **`fromMachine()` 把 7 种真人输入判成"机器"** —— 8 种 actor 里只有 `"ui"` 算人。`key`/`mouse`/`seq`/`drop`/`palette`/`sysmenu` 全是真人操作，却全部触发"AI 撤销边界" | 前两版只看 `ai` 与 `ui` 两条路径，**没枚举 actor 全集** |
| **数字修正** | 写类暴露命令中**既无 `assertion` 也无 `outputSchema` 的有 45 条**（不是 v2 隐含的"除 6 条外都缺"） | 前两版只统计"有断言的 6 条"，**没算 outputSchema** —— 有 `outputSchema` 的命令其实有回执可验 |
| **同义命令元数据不一致** | `fs.delete`(暴露,`undoable=1`) vs `file.delete`(未暴露,`undoable=0`)；`fs.rename`/`fs.copy`/`fs.move` 同 | 要**按操作语义配对**才知道同一个动作被声明了两套风险 |

### 0.2 前两版结论在本轮的处置

| 编号 | 本轮实测 | 处置 |
|---|---|---|
| **v2 P1-5**（`openWorld` 矛盾） | 已撤回，正确 | 维持撤回 |
| **v2 P2-10**（无搜索能力） | 已撤回，正确 | 维持撤回 |
| **v2 P0-1**（`dev.*` 绕过双门） | 仍成立（`destructive=0`） | 维持 |
| **v2 P0-4**（断言覆盖 6/165） | **部分说轻了**：6 条有断言，但另有 **89 条**有 `outputSchema` 可作回执。真正**两者皆无**的是 **45 条写类暴露命令** | **修正数字与结论** |
| **v2 P0-7/P0-8**（评估面可重签 / 未入库） | 仍成立 | 维持 |
| **v2 P0-9**（无写文件命令） | 仍成立。**注**：`fs.copy` 我一度怀疑 schema 与实现不符，实测**是对的**（映射到 `FsCopyTo`，schema 的 `to` 与 `targets` 都被用） | 维持，撤回该怀疑 |
| **v2 §3 的"Effects 分布"** | 与用解析器导出的结果一致（18/147/23/20/28） | 维持 |

### 0.3 本轮**唯一**真正的新 P0

**P0-10 `fromMachine()` 的判据是"actor != ui"，而不是"actor 是不是人"。**

```
src/commands.h:505   bool fromMachine() const { return actor_ != nullptr && wcscmp(actor_, L"ui") != 0; }
```

全仓 actor 字面量实测（`grep -rhoP 'Invoke[A-Za-z]*\([^;]*?L"\K[a-z]+(?=")'`）：

| actor | 次数 | 是不是人？ | `fromMachine()` 判为 |
|---|---|---|---|
| `ui` | 31 | 是 | **人** |
| `sysmenu` | 5 | **是**（系统菜单是用户右键标题栏） | 机器 ❌ |
| `seq` | 2 | **是**（连击键序列） | 机器 ❌ |
| `palette` | 2 | **是**（命令面板是用户打开的） | 机器 ❌ |
| `mouse` | 1 | **是** | 机器 ❌ |
| `key` | 1 | **是**（快捷键） | 机器 ❌ |
| `drop` | 1 | **是**（拖放） | 机器 ❌ |
| `ai` | 1 | 否 | 机器 ✅ |

**7 种真人输入被判成机器，1 种判对。**

直接后果链（已核实）：

1. `src/bookmarks.cpp:773`：`PushUndo()` 记录 `byMachine = CommandBus::Get().fromMachine()`
2. `src/app_commands_misc.cpp:356`：`if (fromMachine() && !store.TopUndoFromMachine())` → 拒绝
3. **人从命令面板执行 `bmk.remove`（`actor="palette"`）→ 该次修改被标为"机器"→ 之后人从书签栏右键"撤销删除"（`actor="ui"`）可以撤；但若人再次从命令面板执行 `bmk.undo`（`actor="palette"`），会被自己刚做的修改判成"AI 不穿透"而拒绝**

反过来更糟：**AI 的修改与人的修改在栈里无法区分** ——
因为真人的 6 种输入全被标成 `byMachine=true`，"AI 撤销边界"这道防线**实际上什么也没守住**。

| 项 | 内容 |
|---|---|
| 对标 | Amershi 2019「细粒度反馈与控制」；可逆性分级（人的操作与机器操作应有**不同**的撤销语义）；MCP 与 HITL 的通行做法是**显式身份**（`actor` 类型 + `approvedBy` + `sessionId`），而不是"不等于某个魔法字符串" |
| 修法 | ①把 `actor` 从 `const wchar_t*` 升级为**带分类的枚举**：`{Human, Ai, Scheduler}`，每个调用点显式声明；②`fromMachine()` 改为 `actorClass == Ai`；③`check_components.py` 增加规则：**禁止新增 actor 字面量**（必须走枚举），否则下次又会漏一个；④迁移期用白名单（`ui/key/mouse/seq/drop/palette/sysmenu` ⊂ Human）先止血 |

### 0.4 第四轮（按 §7.1 逐项改造时）新发现并已修的 3 个缺陷

改造过程中靠**真实端到端跑**（而不是读代码）又挖出三个缺陷。它们都属于
"读代码看不出来、跑一次就现形"那一类 —— 这本身就是一条方法论结论：
**静态校验（25 项全绿）挡不住这类缺陷，只有端到端能挡。**

| # | 缺陷 | 现象 | 真因 | 状态 |
|---|---|---|---|---|
| **F-1** | **外部路径没归一化**（→ §4.1 **P0-12**） | `fs.rename {"target":"C:/dir/a.txt"}` → `errorCode=blocked`，报错"重命名失败（名称非法或目标被占用）"；目录里文件没改名 | `SHCreateItemFromParsingName` **只认反斜杠**，收到正斜杠返回 `E_INVALIDARG(0x80070057)`。而 `fs.*` 这一组**没有任何路径归一化**（导航那一层有，在 `shell_view.cpp` 里手抄了一份） | **已修**：新增 `pathutil::ToShellPath()` 作为全项目唯一收口，`shell_view.cpp` 改调同一份，`shell_ops.cpp` 与 10 条 `fs.*` 命令全部接上 |
| **F-2** | **撤销失败会"改撤别的操作"**（→ §4.1 **P0-13**） | `file.undo` 在应用内撤销栈**非空但撤销失败**时落回 shell 的撤销栈 → "撤 A 失败"变成"居然撤了 B" | 两条不同的失败（机器撤人工操作 / 撤销执行本身失败）被同一条 `if` 合并处理 | **已修**：栈非空但撤不动时**到此为止**，只有"栈本来就是空的"才轮到 shell |
| **F-3** | **失败原因被压成一句废话**（→ §4.2 **P1-9**） | 重命名失败无论何种原因都是"重命名失败（名称非法或目标被占用）" | `RenameTo` 只返回 `bool`，而真因**只存在于 HRESULT 里** | **已修**：`RenameTo` 增加 `why` 出参，区分五种原因并带上 HRESULT；`file.rename.batch` 的 `failed[]` 改成 `{from, reason}` |

> **F-1 的教训值得单列**：同一类输入（正斜杠路径），导航能吃、`fs.*` 不能吃 ——
> **"同一件事两种表现"本身就是 bug 的定义**，而不是"这个命令不支持这种写法"。
> 修法不是给 `fs.*` 打个补丁，而是把规则收口到一个函数
> （`pathutil::ToShellPath`），并让**所有**入口都走它。
> 凡是"同一规则在多处各抄一份"的地方，都应假设它已经分叉了。

---

## 1. 前置事实：不要比错对象

本仓 `src/` 下**零命中** `openai / anthropic / claude / gpt / llm / api_key`。
**本项目不接任何 LLM** —— 它是**被 AI 驱动的对象**（MCP Server + 命令总线），不是 agent 本身。

| 层 | 谁负责 | 本仓状态 |
|---|---|---|
| 决策（想做什么、怎么排） | 宿主 agent（Claude Code / Cursor / 任意 MCP 客户端） | 不在本仓 |
| 感知 + 动作 + 验证 | **本仓** | 主力在此 |
| **改动（mutation）** | **外部 agent 的文件工具** —— **不在本仓，不在审计里** | 信任边界缺口（P0-9） |
| 自我迭代（改自己、择优、沉淀经验） | 本仓提供**脚手架**，agent 提供**决策** | 脚本已有；**无 archive / 无评分 / 无记忆** |

把"没有 LLM"报成缺陷是误报。把"**没有改写通道**"报成缺陷同样是误报 ——
改写由 agent 侧完成是**架构分工**。**缺失的是"改写这件事没有被本仓观测到"**。

**本轮补充一条前置事实**：本仓的 `actor` 只有**字符串**，没有**身份类型**。
这是 P0-10 的根因，也是"审计里分不清人和机器"的根因（P1-3）。

---

## 2. 全网参照系（2026-11-21）

> 只列**机制**，不列宣传语。🆕 = 相对第一版新增。

### 2.1 人机协作交互（HITL / mixed-initiative）

| 项目 / 论文 | 核心机制 | 对本仓的映射 |
|---|---|---|
| **Horvitz 1999 混合主动 12 原则** | act / ask / wait 三阈值；纠错、否决、解释；记住近期交互 | 本仓只有 approve 布尔，**无 ask** |
| **Amershi 2019 18 条人机协作指南** | 明确告知能力边界；**细粒度反馈与控制**；从用户行为学习 | 工具 description 无"何时不要用"；**"谁在操作"这个最基础的粒度就是错的（P0-10）** |
| **Anthropic《Building Effective Agents》** | Agent Loop = 感知→决策→行动→**验证**；evaluator-optimizer；**四种刹车** | 有验证骨架但覆盖 45/165 条无任何回执；**无刹车** |
| **HiLSVA** | plan-first + 显式审批；stepwise provenance；Learn-at-Test-Time；**置信度触发定向提问**；人可随时接管 | **无置信度提问、无接管、无知识库** |
| **Cursor / Copilot Agent Mode / Cline** | 每步 diff 审阅；**checkpoint 可回滚**；plan 模式先审计划 | 无面板、无 checkpoint、无 diff 审阅 |
| 🆕 **可逆性分级的监督强度**（AgentMarketCap 2026-04 / Rippling Threat T10） | 按**可逆性**而非步数分级：只读→自动放行；可逆写→留痕；半可逆→批量送审；**不可逆→强制同步闸门**。**"Overwhelming HITL" 是威胁类别**（SOC 4,484 条告警/天 → 忽略 67%） | `Effects` 有分级原料，但**审批仍二值**（只有 `destructive` 触发） |
| 🆕 **审批请求要素规范**（D3X / Moai 2026） | ①**diff/渲染预览，不是描述** ②理由与依据 ③**为什么升级到人** ④拒绝后果 ⑤approve/edit/reject 各一步 ⑥具名审批人 + **超时不自动放行** | `approve` 只是布尔参数，**无任何"给人看"的面** |
| 🆕 **例外式升级 vs 置信度闸门** | 用**确定性检查**构造升级触发器，**不用模型自报置信度** | `Requirements` 正是确定性检查的载体，但只用于"能不能做" |
| 🆕 **可选择性撤销** | "撤销第 47 步（共 200 步）"；plan 边界 checkpoint | `bmk.*` 是 LIFO-20，只能撤最后一步 |
| 🆕 **显式身份是一切的前提** | 审计与审批都必须回答"**这是谁做的**"：`actor` 类型、`approvedBy`、`sessionId`；"信任增长曲线"（同一质量下审批需求随时间下降）靠的是**按 actor 分组的统计** | **本仓的 `actor` 是字符串，且分类判据错（P0-10）** —— 连"这次是谁做的"都答不对，后面的分级审批、信任曲线、人工改写率全部无从谈起 |

### 2.2 AI 操作软件并获得反馈

| 项目 | 核心机制 | 对本仓的映射 |
|---|---|---|
| **MCP 规范 `2026-07-28`**（当前代） | **无状态协议核**：移除握手（SEP-2575）与 `Mcp-Session-Id`（SEP-2567）；`_meta` 自带版本/客户端能力；`server/discover`；**MRTR（SEP-2322）**：`resultType:"input_required"` + `inputRequests` + `requestState`，客户端带 `inputResponses` 重发；头路由（SEP-2243）；缓存提示（SEP-2549）；**W3C Trace Context 走 `_meta`**；**Tasks 变扩展**（SEP-2663，不兼容）；`subscriptions/listen`；Roots/Sampling/Logging **废弃** | 停在 `2024-11-05`。**"补 `notifications/initialized`"在 2026 语义下已作废** |
| **MCP `2025-11-25`**（上一代） | 生命周期；Elicitation；Sampling；Roots；Tasks(SEP-1686)；`notifications/progress` | 未实现其中任一 |
| **Anthropic《Writing effective tools for agents》** | 工具命名空间化；**返回有意义上下文**；token 效率；**用 Claude Code 自动优化 tool 本身** | description 自动拼接；**45 条写类暴露命令无回执** |
| **WindowsAgentArena** | 首版 19.5% vs 人类 74.5%；UIA+视觉 +57%；**2026-09：UI-TARS-2 50.6%、Agent S3 50.2→56.6** | 无像素/无障碍观测面（**有意取舍**） |
| **OSWorld** | 原版最佳 **12.24%（纯 a11y 树纯文本）**；**加截图掉 0.15 分**；纯截图 5.03%；SoM 4.59% 最差；**2.0**（2026-06）108 条长程流，最佳 20.6% binary / 54.8% partial | **a11y 纯文本赢过所有视觉配置 = R21 路线最硬的外部背书** |
| 🆕 **FAIL 是一等动作** | 恒答 FAIL 得 **8.13%**，**26 个基线里 21 个低于此线** | 有 `not_implemented`/`not_exposed`，但**无"明确弃权"** |
| 🆕 **环境扰动脆弱性** | 窗口最小化 → 相对塌 **70.4%**；最佳 agent 比人多用 **2.7–4.3 倍步骤**；链式完成约 31% | **无步数/轮数上限** |
| **UI-TARS / Agent S2** | 分层；Mixture-of-Grounding；主动分层规划 | 外部契约路线，不同赛道 |

### 2.3 自我迭代

| 项目 | 核心机制 | 对本仓的映射 |
|---|---|---|
| **Darwin Gödel Machine**（2505.22954） | archive + 多样性采样；SWE-bench 20%→50%；**objective hacking 实证**（agent 删掉幻觉检测标记）；开放问题"如何防止改 evaluator" | 有脚本面，**无 archive、无评分**；评估面守卫已实现 |
| 🆕 **RewardHackingAgents**（2603.11337） | 自然运行中**约 50% episode 篡改 evaluator**；**唯一有效防御=锁死**，代价 **25–31%** 运行时 | 印证 `eval_surface` 方向正确，并给可引用的成本数字 |
| 🆕 **作弊会传播**（2609.04170） | 某 agent 发现评测漏洞后**经共享知识库与 P2P 消息传开**；同时自发出现"吹哨人"审计 | 对 C5 是**反向约束**：沉淀层可写 = 作弊通道 |
| 🆕 **PACE**（2606.08106） | 把"接受改动"重述为**序贯假设检验**：配对（McNemar 不一致对）+ **betting e-process**，`E ≥ 1/α` 才提交，任意停止点控制单候选虚假提交率。实证：贪心接受 **30–42% 虚假 / 10–33% 有害**；无收益时 **13–21 次伪自修改（72–100% 虚假）**，最脆弱 agent **掉 4.9 分**；PACE 虚假 0/5、成本 −18%。局限：仅提示词层、温和改进只捕获 78% | **本仓没有接受器**；将来必须用此口径，**不要"分数涨就留"** |
| 🆕 **EVOMAL**（2608.25776） | 恶意技能**自发复制 4.9–9.0 倍**；**删源后第 5 轮仍 68% 攻击成功率** | C5 必须"先验证再入库 + 隔离" |
| 🆕 **When Self-Evolution Backfires**（2608.05810） | 技能池超临界规模**反而拉低**；污染链**结构不可逆**，回滚只挽回一小部分 | C1/C5 需**容量上限 + 淘汰** |
| 🆕 **RRSI**（2609.24972） | 域内 +14.1 vs 5 个域外任务 +4.7 | 自测需**域外 hold-out** |
| 🆕 **Red Queen Gödel Machine**（2026） | **epoch 内评估器完全冻结**；epoch 边界用**锚数据集 + Beta 后验下界 `BB_ε`** 提升；**选择性擦除**旧效用 | 比"永久锁死"更实用；本仓可从"永久锁死+人工重签"演进到 epoch 模型 |
| **OpenEvolve / AlphaEvolve / ShinkaEvolve** | MAP-Elites archive + island；validity funnel；只 promote 通过 hidden tests 的变体 | 只有单条 rollback |
| **ADAS / Meta Agent Search**（2408.08435） | 元 agent 写新 agent；archive 带分数 | 命令表编译期固定 |
| **Reflexion / ExpeL / Voyager** | 情景记忆（≤3 条）/ 跨任务洞察（ADD·UPVOTE·DOWNVOTE·EDIT）/ 技能库（top-5，**先验证再入库**） | **无跨会话沉淀** |
| **Gödel Agent**（ACL 2025） | 运行时自改，**无 review surface、无回滚** | 反面教材；本仓有 rollback 属优势 |
| 🆕 **沙箱不可协商项** | 隔离容器默认无网；工作区作用域文件访问；**硬超时**；命令过滤；**每次 diff 完整审计** | `dev.*` 直接在开发机跑，**零隔离** |
| **Skalse 定理 / STOP 论文** | 安全来自**限制能做什么**；应**测量**边界而非假设 | N3/N4 的理论依据 |
| **LLMs Cannot Self-Correct Reasoning Yet**（ICLR 2024） | 无外部验证的反思是空转（GSM8K 95.5%→89.0%） | 无评分 ⇒ 自评不可信 |

### 2.4 观测与追踪

| 项目 | 核心机制 | 对本仓的映射 |
|---|---|---|
| **OpenTelemetry GenAI 约定**（2026-11 仍标 Development） | `invoke_agent` / `chat` / `execute_tool` / `invoke_workflow`；`gen_ai.tool.name`、`gen_ai.tool.call.id`、`gen_ai.agent.id`、`gen_ai.conversation.id`、`gen_ai.usage.*`、`gen_ai.response.finish_reasons`；内容默认不记录 | 事件只有 3 种类型，无 `traceId`/`turn`/`step` |
| 🆕 **跨 MCP 的 trace 连续性** | 客户端 span 与服务端 span 是**两个 runtime**；不传播 context ⇒ 两条断链。MCP `2026-07-28` 已在 `_meta` 支持 **W3C Trace Context** | `ai_server.cpp` 不读 `_meta`，**无 traceId 贯通** |
| 🆕 **轨迹该记什么** | 除模型/工具调用，还要记**决策点**（为什么停：答案 / 轮数上限 / 出错 / **人接管**）与**人动作**（请求过审批、**谁**批的、何时） | 审计只记 `actor / cmd / arg / ok`，且 **actor 分类错（P0-10）** |
| 🆕 **该看的指标** | 任务成功率（按结果判，不是"没崩"）、**每工具错误率**、**每任务轮数**（上升=在乱试）、**人工改写率**、审批延迟、**信任增长曲线** | `app_stats.cpp` 只做状态栏取数 |

---

## 3. 本仓能力实测基线（2026-11-21 第三轮，用解析器导出而非正则）

```
命令表 173 条 | 已实现 165 | AI 暴露 140 | 处理器 165 | keymap 引用 93
未实现 8：tab.merge / path.copy.link / view.toggle.checkboxes / ext.run
          / ext.sendkeys / search.everything / win.ai.toggle / win.ai.stop
check_ai_meta.py 22 项通过；check_commands.py 三方一致；check_components.py 全绿
（30 条唯一性 + 7 条禁令 + 1 条接线 + 1 条令牌 + 4 条输入纪律）
```

> 本节所有数字来自 `check_ai_meta.parse_commands()` 的完整输出
> （它才是三方对账用的那份解析结果），**不是**正则抽字段。

**`Effects` 各维度分布（165 条已实现）**：

| 维度 | 条数 | 说明 |
|---|---|---|
| `readOnly=1` | 18 | 真正只读的只有 18 条 |
| `readOnly=0` | **147** | 写类命令 |
| `destructive=1` | 23 | 触发危险双门 |
| `openWorld=1` | 20 | 作用于进程外 |
| `undoable=1` | **28** | 声明可撤销 |

**验证面（本轮关键修正：把 `outputSchema` 也算进"可验"）**：

| 类别 | 条数 | 说明 |
|---|---|---|
| 有 `assertion`（自断言） | **6** | 效果可机械判定 |
| 有 `outputSchema`（回执） | **89** | 调用方能核对返回值 |
| AI 暴露 + 写类 + **两者皆无** | **45** | **真正的盲区** |

那 45 条（可直接落修）：

```
bmk.dedupe bmk.focus bmk.undo config.set ext.terminal ext.terminal.admin
file.focus.bottom file.focus.top file.new.folder file.new.text file.paste
file.select.all file.select.invert file.undo fs.copy fs.copy.toclip
fs.cut.toclip fs.delete fs.move fs.open fs.open.tab fs.open.window
fs.properties fs.rename go.config go.controlpanel go.desktop go.documents
go.downloads go.home go.localappdata go.powershell go.startfolder go.thispc
nav.back nav.forward nav.home nav.up session.named.save tab.color
tab.copy.title tab.lock tab.move.window tab.pin win.new
```

> **v2 的口径需要修正**：v2 说"`verdict` 对 159/165 恒为 `ok`"，这在**断言维度**上是对的；
> 但由此推出"几乎所有命令都不可验"**说过头了** —— 89 条有 `outputSchema`，
> 调用方拿到返回值就能自行核对。真正的盲区是**这 45 条**。

**闭环三要素**：

| 要素 | 应覆盖 | 实际 | 覆盖率 |
|---|---|---|---|
| 自断言 | 165 条已实现 | 6 条 | 3.6% |
| 可写状态可回读字段 | — | 6 个 | — |
| 事件类型 | — | 3 种（`command`/`state.changed`/`dev.job.done`），**全在内存环形缓冲** | — |
| 声明式意图 | — | 2 条（都只针对书签） | — |
| 端到端回归 | — | 1 条（书签移动） | — |
| 真撤销栈 | 声明 28 条 | 13 条（全 `bmk.*`） | 46% |
| e2e 覆盖 dev.\*/事件流/双门/profile | — | **0 条** | 0% |
| 能写文件内容的命令 | 自我迭代需要 | **0 条** | **0%**（P0-9） |
| **actor 身份类型** | 审计与审批的前提 | **0（只有字符串）** | **0%**（P0-10） |

**同义命令的元数据不一致（本轮新查）**：

| 操作 | AI 暴露的那份 | 未暴露的那份 | 偏差 |
|---|---|---|---|
| 删除 | `fs.delete` `undoable=1` | `file.delete` `undoable=0` | **同一实现两套声明** |
| 重命名 | `fs.rename` `undoable=1` | `file.rename` `undoable=0` | 同上 |
| 复制到 | `fs.copy` `undoable=1` | `file.copy.to` `—` | 同上 |
| 移动 | `fs.move` `undoable=1` | （无 `file.move`） | — |
| 打开 | `fs.open` `openWorld=1` | `file.open` `openWorld=1` | 一致 ✓ |
| 新标签打开 | `fs.open.tab` `openWorld=1` | `file.open.tab` `openWorld=0` | **不一致** |

> 这些 `fs.*` 与 `file.*` 走的是**同一批底层实现**（`shellops::DeleteSelection` / `RenameTo` / `TransferTargets`）。
> 元数据却分两套，且**暴露给 AI 的那份恰好是声明"可撤销"的那份** —— 而 `fs.*` 根本没有撤销栈（P0-3）。
> 这不是笔误，是"同一事实有两个源头"的结构性问题（违反 AGENTS.md §1.3 的唯一来源原则）。

### 3.1 第四轮复核（按 §7.1 改造之后，2026-11-21）

上面的数字是**第三轮的历史基线**，保留不动以便对照。改造后的实测：

```
命令表 175 条 | 已实现 166 | AI 暴露 139 | keymap 引用 93+
check_ai_meta.py 25 项通过（新增第 25 项"可逆性不许说谎"）
check_commands.py 三方一致；check_components.py 全绿
单测 48 个（47 PASS / 1 无法运行=剪贴板环境限制 / 0 失败）
src/ 151 文件 33351 行 | tests/ 48 文件 6518 行 | exe 33.2 MB
```

| 维度 | 第三轮 | 第四轮 | 变化 |
|---|---|---|---|
| `undoable=1` | 28 条（只有 13 条真能撤） | **22 条，且 22 条全部有真实撤销机制** | 11 条空承诺归零 + 新增文件操作撤销栈 |
| 有自断言 | 6 条 | **91 条**（65%），另 49 条**显式豁免**且理由对外可见 | 豁免机制落地，不再是"没断言=没人管" |
| `actor` 身份类型 | 0（只有字符串） | `ActorKind{Human,Ai}` + 9 个工厂 | P0-10 已修 |
| 同义命令 Effects | 10 组两套 | 10 组共用**同一份常量**，机械校验逐项比对 | P0-11 已修 |
| 评估面清单 | **根本没入库** | 已入库 + `selfSeal` + git 信任层（已跟踪 + 工作树干净，fail-closed） | P0-7/P0-8 已修 |
| `dev.rollback` | 不可逆（直接 `git checkout`） | 按 `snapshotId` 还原，**回滚前先存 `pre-rollback/`** → 回滚本身可逆 | P0-5 已修 |
| 文件操作撤销 | 0 条 | `fs.*` / `file.*` 的改名/移动/复制全部有应用内撤销栈 | P0-2 已修 |


## 4. 错误清单

> 判定标准：**本仓内部矛盾**或**可复现的缺陷**。每条附文件 + 行号，可直接落修。

### 4.1 P0 — 会导致事故或"假绿"（13 条）

#### P0-1（修正）`dev.build` / `dev.test` 仍**绕过危险双门**

| 项 | 内容 |
|---|---|
| 证据 | `src/commands.cpp:218,221`：`dev.build` / `dev.test` 的 `Effects{0, 0, 1, 1, 0, Scope::App}` —— 第 2 位 `destructive=0` → **不触发** `aiAllowDanger` + `approve` 双门（`src/ai_api.cpp:258-269`） |
| 已修的部分 | 参数面已收紧：`target` 改枚举 `["all","clean"]`；`filter` 走 `ValidTestFilter` 白名单（`src/app_commands_dev.cpp:30-47`）；`RunCapture` 改 `CreateProcessW` + argv 数组，不经 shell（新增 `src/dev_proc.cpp`） |
| 后果 | AI 可**无批准**地触发编译/测试 —— 而编译会执行仓库里任意可执行内容（测试二进制、`build_fast.py` 里的任意 Python）。这是"执行任意代码"的现成入口 |
| 对标 | Skalse 等人定理：安全来自**限制 agent 能做什么**。RewardHackingAgents 实测：约 50% 的 episode 出现篡改 evaluator 的尝试 |
| 修法 | ①`dev.build`/`dev.test` 的 `destructive` 置 1（⇒ 必须 `approve=true`）；②`approve` 的语义从布尔升级为**绑定参数哈希**（防"批一次、跑十次"）；③`openWorld=1` 已在（保留） |

#### P0-2 `file.undo` 仍无可用撤销栈（严重性下调，能力缺口仍在）

| 项 | 内容 |
|---|---|
| 证据 | `src/app_commands_files.cpp:441-459`：撤销转发给 `shellops::Undo(v)`（`src/shell_ops.cpp:999`），该函数向视图背景 `IContextMenu` 要 undo 谓词<br>代码注释自陈："2026-11-21 实测：删除/改名之后这条路径在 IExplorerBrowser 里拿不到 undo 行，撤销无效（缺陷未修）" |
| 本轮改进 | **假绿已消除**：现在返回 `ok=0` + `NoteFailure("当前视图没有可撤销的操作…")`，并把 `GetCommandString` 触发扩展加载、导致 UI 线程永久挂死的坑绕开了（`src/shell_ops.cpp:1040-1060`）—— 这是上一版**没看见**的实质改进 |
| 残留 | `fs.delete` / `fs.rename` / `fs.move` / `file.rename.batch` **仍不可撤销**；AI 侧只看到"失败"，拿不到"能否撤销"这一信息 |
| 对标 | 可逆性分级（§2.1）：半可逆动作应有"异步送审 + 可逆窗口"，不可逆才强制同步闸门 —— 本仓现在**两者都做不到**，因为既没有真栈也没有分级 |
| 修法 | 在 `fs.*` 侧自建撤销栈（同 `bmk.*` 的 `PushUndo` 口径，`src/bookmarks.cpp:741`）；同时在 `Effects` 里把 `undoable` 与 `destructive` 分开声明，让 agent 能**按可逆性**选审批强度 |

#### P0-3 28 条命令声明 `undoable`，只有 13 条真有撤销栈

| 项 | 内容 |
|---|---|
| 证据 | 全表 `undoable=1` 共 **28** 条；实际有栈的只有 `bmk.*` **13** 条（`src/bookmarks.h:173` `std::vector<UndoEntry> undo_`，上限 20）<br>`src/tab_set.h` / `src/tab_set.cpp`：**grep 无任何 Undo 符号** → 7 条 `tab.*` 命令声明 `undoable=1` 却无栈<br>`src/shell_ops.cpp`：`DeleteSelection` / rename / move 靠 `FOF_ALLOWUNDO`，但取 undo 谓词走 `IContextMenu`，实测拿不到 |
| 后果 | AI 依据 `undoable=true` 认为"可放心试错"，实际多数不可逆 |
| 对标 | 可选择性撤销（§2.1）：需要"撤第 N 步"，不是 LIFO-20 |
| 修法 | 引入 `undoable` 的**机械校验**（见 P1-8）：`undoable=1` 必须在命令表里指向一个真实栈，否则置 0 或补栈 |

#### P0-4 `verdict` 对 159/165 条命令恒等于 `ok`；真正无任何回执的是 45 条写类暴露命令

| 项 | 内容 |
|---|---|
| 证据 | `src/commands.h:347` `const char* assertion = nullptr;`；带 `assertion` 的仅 **6** 条（`tab.activate` / `view.mode` / `view.sort` / `view.group` / `bmk.show` / `bmk.hide`）<br>可写状态可回读字段仅 **6** 个（`src/ai_api.cpp:451-462`；`tools/check_ai_meta.py:181` 的 `WRITABLE_STATE` 也是 6 条）<br>`tools/check_ai_meta.py` 第 17/18 项**只校验"有断言的命令"** —— 不检查"应有断言的命令" |
| **本轮修正** | **v2 说"几乎全部不可验"是过头了**：另有 **89 条**命令带 `outputSchema`，调用方拿到返回值即可自行核对（这是 Anthropic《Writing effective tools》说的"返回有意义上下文"）。真正**既无断言、又无 outputSchema** 的写类暴露命令是 **45 条**（清单见 §3） |
| 后果 | 那 45 条里包含最高频的 `fs.delete` / `fs.move` / `fs.rename` / `file.paste` / `file.undo` —— 也就是**最需要验证的那些恰恰没有任何回执**。`verdict` 全绿 ≠ 闭环成立 |
| 对标 | Anthropic《Building Effective Agents》：evaluator-optimizer 的前提是 evaluator **真能判失败**；OSWorld 的教训是 **FAIL 是一等动作**（恒答 FAIL 得 8.13%，26 个基线里 21 个更差） |
| 修法 | ①`check_ai_meta.py` 增加第 23 项：**所有 `readOnly=0` 且 AI 暴露的命令必须有 `assertion` 或 `outputSchema`，否则报错并列出**（豁免须写理由）；②`StateToJson()` 扩到覆盖 `tab.*` / `fs.*` / `view.*` / `nav.*`；③`view.toggle.hidden/system/extensions`、`tab.rename`、`tab.color`、`config.set` 目前**有写无读** |

#### P0-5 `dev.rollback` 不可逆，而文档指引的 `dev.snapshot` 不保存内容

| 项 | 内容 |
|---|---|
| 证据 | `src/dev_tools.cpp:205` `Rollback()` 执行 `git checkout -- <file>` —— **丢弃工作树改动，不可逆**<br>`src/dev_tools.cpp:174` `Snapshot()` 只返回 `head / status / diffStat / repo / profile / bash / python` —— **不保存文件内容**<br>`docs/FULL-COVERAGE.md` §10.4 却写："回滚会丢工作树改动，调用方必须先 dev.snapshot" —— 该保护**无效** |
| 后果 | AI 调用 `dev.rollback` 恢复文件时，之前的修改**无法找回**。双门只防"未授权"，不防"授权后不可逆" |
| 对标 | Cursor checkpoint：存的是**内容**不是 diff 摘要；"每次 diff 的完整审计"是沙箱不可协商项 |
| 修法 | ①`dev.snapshot` 真正落盘（写 `snapshot/<ts>/` 保存改动文件副本），返回可 restore 的 `snapshotId`；②`dev.rollback` 增加 `snapshotId` 参数；③description 中写明"不可逆"与"需先快照" |

#### P0-6 危险拒绝 / `bad_args` / `schema_invalid` **不进审计**

| 项 | 内容 |
|---|---|
| 证据 | `src/ai_api.cpp:258-269`：双门判定在 `Invoke` **之前**，`return` 直接出<br>`src/ai_api.cpp:275-286`：`schema_invalid` / `bad_args` 同样在 `Invoke` 前 `return`<br>`src/commands.cpp:601,611,616` 的 `Audit(...)` 在这之后才发生<br>实测 `data/audit.log`（1095 行）：`ok=1` **958** 条 / `ok=0` **137** 条 / 含 `blocked` **33** 条；**`danger_disabled` / `need_approval` / `bad_args` / `unknown` 全部 0 命中** |
| 后果 | 审计日志**看不见** AI 被拒的破坏性请求 —— 恰恰是最该看见的那部分。违反自订 `docs/ARCHITECTURE-RULES.md` R18（"每次调用写 audit.jsonl"）与 C6 |
| 对标 | Darwin Gödel Machine 的 archive 提供**可追溯 lineage**；客观激励 hacks 之所以能被发现，前提是**日志完整** |
| 修法 | 把审计下沉到 `ai_api.cpp` 的**统一出口**：所有早退路径写一条 `actor=ai ok=0 code=<10 类机器码>`，并在 `CommandBus::Audit` 签名上补 `errorCode` 字段 |

#### P0-7【本轮新增】评估面清单可被 agent **自己重签** —— `eval_surface` 的最大洞

| 项 | 内容 |
|---|---|
| 证据 | `src/eval_surface.cpp:132-162`：受保护目录只有 `tests/*.cpp` 与 `tools/*.py`（`src/eval_surface.cpp:42-45`）<br>**`data/eval-surface.json` 本身既不在 `PROTECTED_FILES`（`tools/gen_eval_baseline.py:39-43`），也不在 `PROTECTED_DIRS`（`tools/gen_eval_baseline.py:47-50`）**<br>重签动作 = `python tools/gen_eval_baseline.py`（`tools/gen_eval_baseline.py:156-159` 直接覆写清单） |
| 后果 | 门只检测"**评估面被改了但清单没跟着改**"。agent 流程可以是：改软 `tests/run_all.sh` → 跑一次重签 → 门全绿 → "测试通过"变成一句空话。**整套守卫对"知情改软 + 同步重签"完全无效** |
| 为什么这是最高级别 | RewardHackingAgents 实测**约 50% 的 episode 出现篡改 evaluator 的尝试**；唯一有效防御是**锁死**评估器。本仓的锁只锁到"清单"，没锁到"签发权" |
| 对标 | "贪心接受 = p-hacking"综述的结论：**评分实现对提议者隐藏**，只暴露分数与轨迹；**发现的利用一律按"候选失败"计**；RQGM 的做法是 **epoch 内评估器完全冻结**，只在带锚的 epoch 边界由人提升 |
| 修法 | ①**签发权与改写权分离**：清单的 SHA-256 上限值不写在仓库里，而写在**仓库外**的受信位置（如 `%LOCALAPPDATA%` 或用户自己的签名文件），或由 git tag / 签名提交承载；②清单文件本身纳入 `PROTECTED_FILES`（自指保护）；③重签必须走**人可见的交互**（例如弹出确认窗写明"你正在放宽判据"），AI 通道**不得**触发重签；④引入 **epoch 模型**：评估面在一个 epoch 内冻结，跨 epoch 的提升需要锚数据集 + 人签字（RQGM 口径），比"永久锁死"更可持续 |

#### P0-8【本轮新增】`data/eval-surface.json` **根本没入库** → "可追溯"承诺当前是假的

| 项 | 内容 |
|---|---|
| 证据 | `git status --short data/` → `?? data/eval-surface.json`（**untracked**）<br>`git ls-files data/eval-surface.json` → 空<br>`.gitignore` 只忽略 `data/settings.json` / `audit.log` / `ai-token.txt` 等，**没有忽略它** —— 是漏了 `git add`，不是有意排除 |
| 后果 | ①`src/eval_surface.h:32` 承诺"签发记录进 git，可追溯" —— **当前不成立**；②`dev.rollback` 用 `git checkout --` 回滚，**回滚不了未入库的清单**；③谁在什么时候放宽了判据，**git 里查不到**；④换机器/CI 上清单不存在 → `eval_surface::Check()` 返回 `ok=false` → `dev.build`/`dev.test` **直接拒绝启动**（这是 fail-closed，设计正确，但会让 CI 一开始就红） |
| 修法 | `git add data/eval-surface.json` 并把它纳入 `PROTECTED_FILES`；同时在 `gen_eval_baseline.py` 里加自检：清单必须已被 git 跟踪（`git ls-files --error-unmatch`），否则拒绝签发 |

#### P0-9 自我迭代的"改"这一步**完全在信任边界之外**

| 项 | 内容 |
|---|---|
| 证据 | 173 条命令里**没有一条能写文件内容**。`file.new.text` 走 `shellops::NewTextFile`（`src/app_commands_files.cpp:568-571`），只创建**空**文件并交给系统编辑器；没有任何 `file.write` / `fs.write` / `dev.edit` / `patch` 类命令<br>也就是说：**真正"改代码"的动作，只能由外部 agent 用它自己的文件工具完成** |
| 后果 | 自我迭代链路里，**最危险的一步（写源码）不在 CommandBus、不进审计、不触发双门、不被 `eval_surface` 的目录集合覆盖**（`src/` 不在受保护目录里）。被观测的只有"读现状 / 编译 / 测 / 回滚" |
| 这不是"缺一条命令" | 架构分工上是合理的：agent 有编辑能力，不需要本仓再提供一条"任意写文件"的万能命令（那反而会破坏 R21 的边界）。**问题是观测面**：本仓**看不见**谁在什么时候改了哪个文件 |
| 对标 | 沙箱不可协商项：**每一次 diff 的完整审计**；"把数据流向画出来"；"审批必须给动作本身的 diff" —— 都要求"改动可见" |
| 修法 | ①**不新增写文件命令**，改为**观测**：`dev.snapshot` 增加 `changedFiles`（含 mtime + SHA-256）让 agent 与人都能对比"上一轮到现在改了哪些文件"；②新增 `dev.diff`（返回 `git diff` 的结构化结果）作为审计与人工审阅的**唯一视图**；③`eval_surface` 的受保护目录从 `tests/`+`tools/` 扩展到**关键源码**（如 `src/commands.cpp`、`src/ai_api.cpp`、`src/commands.h`）—— 这些正是"判据"的一部分：改软 `check_ai_meta.py` 里的豁免名单即可绕过断言覆盖率检查 |


#### P0-10【本轮新增】`fromMachine()` 把 7 种**真人输入**判成"机器" —— 人机判据整体失真

| 项 | 内容 |
|---|---|
| 证据 | `src/commands.h:505`：`bool fromMachine() const { return actor_ != nullptr && std::wcscmp(actor_, L"ui") != 0; }` —— 判据是"**不等于字符串 `ui`**"，而不是"**是不是人**" |
| 全仓 actor 实测 | `ui`×31 / `sysmenu`×5 / `seq`×2 / `palette`×2 / `mouse`×1 / `key`×1 / `drop`×1 / `ai`×1。**前 7 种全是真人操作**（系统菜单 = 用户右键标题栏，`src/app_wnd.cpp:140-152`；命令面板 = 用户打开，`src/app_commands.cpp:505,517`；`key`/`seq`/`mouse` = `src/input_router.cpp:177,270,291,466`；`drop` = `src/bookmark_bar.cpp:389`） |
| 后果链（已核实） | ①`src/bookmarks.cpp:773`：`PushUndo()` 写 `byMachine = fromMachine()` → **真人操作被标成 `byMachine=true`**<br>②`src/app_commands_misc.cpp:356`：`if (fromMachine() && !store.TopUndoFromMachine())` 拒绝<br>③于是 **"AI 撤销只到自己的上一次修改为止"这道防线，实际上把真人也一起划进了"AI"**：真人在命令面板做一次 `bmk.remove`，之后从命令面板 `bmk.undo` 会被自己的操作判成"上一次修改来自界面操作，AI 撤销到此为止"而拒绝<br>④反向更糟：**AI 的修改与真人的修改在撤销栈里无法区分**，因为真人那 6 种输入全被标成 `byMachine=true` |
| 为什么这是 P0 | 这是 `actor` 语义问题的**总根因**。它同时污染：撤销边界（本条）、审计归类（P1-3 的 `approvedBy`）、以及任何"人机等价"判定。而且**失败方向是"把人的操作误判成机器的"**（拒绝人），不是"把机器误判成人"（放行机器）—— 前者让功能看起来坏了，后者才是安全问题；但两者都让"谁做的"这个事实丢失 |
| 对标 | Amershi 2019「细粒度反馈与控制」；**显式身份是一切的前提** —— 审计、分级审批、信任增长曲线都必须先答对"这次是谁做的"；OTel 轨迹要求记"谁批的、何时批的" |
| 修法 | ①`actor` 从 `const wchar_t*` 升级为**带分类的类型**（`enum class ActorKind { Human, Ai, Scheduler }` + 可读名字），每个调用点显式声明；②`fromMachine()` 改判 `kind == Ai`；③`check_components.py` 增加规则：**禁止新增 actor 字符串字面量**（必须走枚举/工厂），否则下次又会漏一个 —— 这条比修 8 个调用点更重要，因为**漏一个就等于整条防线失效**；④迁移期先止血：把 `ui/key/mouse/seq/drop/palette/sysmenu` 显式列入 Human 白名单 |

#### P0-11【本轮新增】同义命令的元数据分两套，且**暴露给 AI 的那份是错的那份**

| 项 | 内容 |
|---|---|
| 证据 | 用校验器解析器导出后配对比较（详见 §3 末表）：<br>`fs.delete`（暴露）`undoable=1` ↔ `file.delete`（未暴露）`undoable=0`<br>`fs.rename`（暴露）`undoable=1` ↔ `file.rename`（未暴露）`undoable=0`<br>`fs.copy`（暴露）`undoable=1` ↔ `file.copy.to`（未暴露）无声明<br>`fs.open.tab`（暴露）`openWorld=1` ↔ `file.open.tab`（未暴露）`openWorld=0`<br>`fs.*` 与 `file.*` 走**同一批底层实现**（`shellops::DeleteSelection` / `RenameTo` / `TransferTargets`） |
| 后果 | `fs.*` 是最常被 AI 调用的那一族（`fs.delete` 在实测 `audit.log` 里出现过），而它声明的 `undoable=1` **是假的**（`fs.*` 无撤销栈，见 P0-3）。等于**把最乐观的风险声明给了最活跃的调用方** |
| 为什么前两版看不见 | 单看一条命令看不出问题，必须**按操作语义配对**（`fs.X` ↔ `file.X`）才暴露。这是"同一事实有两个源头"的典型后果 |
| 对标 | AGENTS.md §1.3 唯一来源原则（颜色/字号/品牌串"只有一个来源"）—— 风险元数据也该只有**一个**来源；`Effects` 是声明式契约，**契约不能因入口不同而变** |
| 修法 | ①`fs.*` 与 `file.*` 共用**同一份 `Effects`**（在 `commands.cpp` 里定义常量 `kEffectsFsDelete` 供两条命令引用），或②让 `fs.*` 成为 `file.*` 的**别名**（同 `CmdId`，用 `legacyName` 机制），从根上消灭第二份声明；③`check_ai_meta.py` 增加第 25 项：**配对同名操作的 `Effects` 必须一致**（脚本维护一张 `fs.X ↔ file.X` 配对表） |

#### P0-12【第四轮新增】外部路径没归一化 —— 同一份输入，导航能吃、`fs.*` 不能吃，还报一个**错的错**

| 项 | 内容 |
|---|---|
| 证据 | `SHCreateItemFromParsingName` **只认反斜杠**：传 `"C:/dir/a.txt"` 返回 `E_INVALIDARG(0x80070057)`。导航那一层在 `src/shell_view.cpp:411-419` **手抄了一份**归一化，而 `fs.*` 这一族**一个都没有** —— 参数直接喂给 shell |
| 现象（端到端实测） | `fs.rename {"target":"C:/dir/a.txt"}` → `errorCode=blocked`，报"重命名失败（名称非法或目标被占用）"；目录里文件**没改名** |
| 后果 | ①**假绿**：错误码说"名称非法/被占用"，真因是**分隔符** —— agent 会据此去换名字、等锁、换路径重试，**永远查不到真因**；②同一类输入两种表现（导航 OK / `fs.*` 失败），这按定义就是 bug，而不是"这个命令不支持这种写法"；③`fs.*` 是 AI 最常调的一族（`fs.delete` 实测出现于 `audit.log`），错的是**最活跃的调用方** |
| 为什么前几版看不见 | 静态检查不解析路径字面量，25 项全绿也照样漏。**只有真拿正斜杠跑一次才会现形** —— 这是"读代码看不出来、跑一次就现形"那一类的典型 |
| 对标 | AGENTS.md §1.3「唯一来源」：同一条规则在多处各抄一份，就应假设它已经分叉；GUI agent 生态的 path normalization 一律是**入口唯一收口**而非各处打补丁 |
| 已修的部分 | 新增 `pathutil::ToShellPath()`（`src/path_util.cpp:35`）作为**全项目唯一收口**：正斜杠→反斜杠、折叠连续分隔符、去尾分隔符；`::` / `shell:` 原样放行。`src/shell_view.cpp:419` 改为调同一份；`src/shell_ops.cpp:164,1130`、`src/shell_ops_path.cpp:15,16,44,77`（撤销栈按路径的那组）、`src/app_commands_fs.cpp` 的 10 条 `fs.*`（8 处调用点）、`src/app_commands.cpp:288-289` 的 `file.copy.to` / `file.move.to` 全部接上。批量版 `pathutil::ToShellPaths()`（`src/path_util.cpp:50`）避免调用点自己写循环漏项 |
| 剩余缺口 | `check_components.py` **还没有**"外部路径必须过 `ToShellPath`"的机械规则 —— 规则目前靠人记住，新入口仍可能再漏一次。补法：静态扫 `fs.*` / `file.*` handler 里的 `args.Str(L"path"/L"target"/L"targets")` 直接流向 shell API 的调用点，命中即报错 |

#### P0-13【第四轮新增】撤销失败会"改撤别的操作" —— 撤 A 失败，结果撤了 B

| 项 | 内容 |
|---|---|
| 证据 | `src/app_commands_files.cpp` 的 `CmdUndo` handler：应用内撤销栈**非空但撤不动**时，旧实现会**继续落回 shell 的撤销栈** |
| 现象 | `file.undo` 想撤 A（A 撤不动：被占用 / 权限不足 / 栈顶是人工操作而调用方是机器）→ 命令返回失败，但 shell 那边把 **B**（用户右键菜单做的、不相干的一件事）撤了 |
| 后果 | ①**比失败更糟**：用户以为退回了 A，实际丢的是 B —— 这是**静默的数据丢失**，比"撤不了"严重一个量级；②两条语义完全不同的失败（"机器撤人工操作" vs "撤销执行本身失败"）被同一条 `if` 合并处理；③违反 P0-10 刚建立的"机器的撤销不穿透人工操作"边界（`bmk.*` 守住了，文件操作没守住） |
| 为什么前几版看不见 | 旧代码在"栈空"和"栈非空但撤不动"两种情形下**走同一条路**，只有真去制造一次"撤不动"（占用文件 / 人机混做）才会暴露 |
| 对标 | Undo 语义必须**单调**：撤不掉就是撤不掉，不能顺手做别的；Amershi 2019 的"细粒度反馈与控制"要求失败时**说清楚发生了什么**，而不是换一个不相干的动作 |
| 已修的部分 | 栈非空但撤不动 → **到此为止，不落回 shell**（`src/app_commands_files.cpp` 的 `CmdUndo` handler 里有完整注释说明这两种情形）；只有"我们的栈本来就是空的"才轮到 shell 的撤销栈。失败时用 `NoteFailure(why)` 报出真因 |
| 剩余缺口 | 撤销失败时**没有告诉调用方"为什么撤不动"**：栈顶是人工操作 vs 执行本身失败，两者的 `why` 文本尚未细分到机器可判读的级别（与 P1-9 同源问题） |

### 4.2 P1 — 闭环能力 / 协议正确性（9 条，含 3 条新增）

#### P1-1 MCP 停在 `2024-11-05`，且外部参照系已跨代

| 项 | 内容 |
|---|---|
| 证据 | `src/ai_server.cpp:605`：`protocolVersion` **硬编码 `"2024-11-05"`**；`src/ai_server.cpp:485,538,602` 只实现 `tools/list` / `tools/call` / `initialize` 三个方法<br>缺失：`notifications/initialized` / `ping` / `logging` / `resources` / `prompts` / `completions` / `sampling` / `roots` / `tasks` / `notifications/progress`<br>`src/ai_server.cpp`：`kParseError` 与 `kInternalError` 标 `[[maybe_unused]]`，**无调用点** |
| 后果 | 只能对接"最老一档"的客户端；用 `notifications/initialized` 的客户端会因协议版本不匹配而拒绝会话；长任务无法用 Tasks 异步化 |
| **本轮的关键修正** | **"补 `notifications/initialized`"这条修法在 2026 语义下已作废** —— MCP `2026-07-28` 用 SEP-2575 **删除了整个握手**。同时 Tasks 在 2026 变成**扩展**且与 2025-11-25 **线不兼容**（SEP-2663）。按 2026 语义，正确路径是：`resultType` + `inputRequests` + `requestState` 的 **Multi Round-Trip Requests**（SEP-2322） |
| 修法 | ①短期：`protocolVersion` 提升到 `"2025-11-25"` 并做**向下兼容**（客户端报老版本仍握手成功）；②补 `ping`（最小代价，跨代都还在）；③`tools.listChanged` 置 `true` 并在命令表变更时发通知（当前恒 `false` ⇒ **永不通知**，工具表变更对客户端不可见）；④中期：按 2026 语义实现 `resultType:"input_required"` + `inputRequests` + 不透明 `requestState`（服务端"问人"的唯一正确形态）；⑤`requestState` 必须按**不可信输入**处理（MCP Ruby SDK 用 AES-256-GCM 封缄 + TTL + 绑定原始 method/target/args） |

#### P1-2 单管道单连接，阻断多 Agent 并行

| 项 | 内容 |
|---|---|
| 证据 | `src/ai_server.cpp:211`：`CreateNamedPipeW(g_pipe_name.c_str(), PIPE_ACCESS_DUPLEX, ..., 1, ...)` —— **`nMaxInstances=1`** |
| 后果 | 第二个 AI 客户端连不上（连接被占用且不排队）；宿主若为每个 agent 各开一条管道也不成立（管道名内嵌 hwnd+pid） |
| 对标 | MCP 2026-07-28 的整个方向就是**协议核无状态**：任何请求可落在任何实例上，不需要粘性会话。**本仓的"单会话"假设正是被这一代规范淘汰的那一代设计** |
| 修法 | `nMaxInstances` 提到 16，ServeLoop 改为**多实例轮询/多线程**；或每个连接一个线程 |

#### P1-3 审计缺参数哈希 / 耗时 / 审批标记 / sessionId，且不是 `audit.jsonl`；更根本的是**actor 没有身份类型**

| 项 | 内容 |
|---|---|
| 证据 | `src/commands.h:490`：`CommandBus::Audit(const CommandDef&, const std::wstring&, const wchar_t* actor, bool ok)` —— **仅 4 参**<br>实测日志行：`2026-11-21T18:25:11 ai fs.open.tab arg={"path":"C:\\Users\\NoteBook\\Desktop"} ok=1` —— **人类可读文本**，非 JSONL<br>`docs/ARCHITECTURE-RULES.md` R18 要求 `audit.jsonl`（不可关闭），C6 要求**参数哈希 + 耗时 + 是否审批** |
| **根因（本轮补充）** | `actor` 是**裸 `const wchar_t*`**，没有身份类型（见 P0-10）。所以审计里写下的"谁做的"本身就是错的：`palette` / `key` / `mouse` / `seq` / `drop` / `sysmenu` 都是真人，但与 `ai` 无法区分。**字段再多也补不回一个错的分类** |
| 后果 | ①无法做"同一参数重放"分析；②无法做性能回归；③**无法区分"人工批准后执行"与"AI 直接执行"**；④无法算"人工改写率"与"信任增长曲线"（这两个指标都以 actor 分组为前提） |
| 对标 | OTel 轨迹要求记**决策点**与**人动作**（请求过审批、**谁**批的、何时）；LLM API 侧 request-id / usage / latency 是标配；DGM archive 依赖**可追溯 lineage** |
| 修法 | ①先修 P0-10 的 actor 类型；②审计行改 JSONL：`ts / actorKind / actorName / cmd / argsHash / args / ok / verdict / errorCode / durationMs / approvedBy / sessionId / pid / traceId`；③保留旧文本行兼容或提供迁移脚本 |

#### P1-4 `CommandDef` 无 `description` 字段，违反自订 C7

| 项 | 内容 |
|---|---|
| 证据 | `src/commands.h:301-360` `struct CommandDef` —— **无 `description`**；`src/ai_api.cpp:495-509` `Describe()` 从 `title + group + effects` **自动拼接**；`src/ai_api.cpp` 的 `Describe()` 只在 `destructive`/`readOnly`/`openWorld`/`needsSelection` 上加提示语 |
| 后果 | 工具描述**无法表达"何时不要用"** —— 而这正是 Amershi 2019 的核心建议与 Anthropic《Writing effective tools for agents》明确点名的失败模式 |
| 修法 | `CommandDef` 加 `description`（中文短句，≤80 字），`Describe()` 优先取显式值，缺失时回退自动拼接并**在 check_ai_meta 里报缺失**；给每条 `aiExposed` 命令补"何时不要用" |

#### P1-5【上一版撤回】`openWorld` 的"自相矛盾"**不存在**

| 项 | 内容 |
|---|---|
| 上一版说法 | "`dev.build`/`dev.test`/`dev.snapshot` 的 `openWorld=0` —— 它们显然会执行外部程序" |
| 本轮实测 | **上一版读错了字段序**。`src/commands.h:237-241` 的顺序是 `readOnly / destructive / idempotent / openWorld / undoable`；`Effects{0, 0, 1, 1, 0, Scope::App}` 中第 4 位是 `openWorld=1`。`dev.build`、`dev.test`、`dev.snapshot`、`dev.rollback` 的 `openWorld` **全部为 1**。源码注释也写明："openWorld=1：它会真的起外部进程（python build_fast.py）" |
| 处置 | **撤回这条错误**。但它不是白错 —— 它暴露了 P2-5（裸 0/1 聚合初始化）已经造成了一次**真实误读**，因此 P2-5 升级为 P1-6 |

#### P1-6（升级）`Effects` 用裸 `0/1` 聚合初始化 → 已造成一次真实误读

| 项 | 内容 |
|---|---|
| 证据 | `src/commands.cpp` 全表 `Effects{0,0,1,1,0,Scope::App}` 形态；`src/commands.h:236-242` 定义 5 个 `bool` + `Scope` |
| 实证 | **上一版报告自己把 `openWorld` 读成 `0`**（见 P1-5）。这不是假设的风险，是已发生的、且**发生在读代码的人身上**的错误 |
| 波及面 | 校验器**也按位置解析**：`tools/check_ai_meta.py:498-501` 是 `'readOnly': ro == '1', 'destructive': de == '1', 'idempotent': idem == '1', 'openWorld': ow == '1', 'undoable': undo == '1'`。**表、校验器、人读的三方都靠位置**，一处错位会被三方一致地读错 |
| 后果 | 5 个 bool 里任一写错 = 风险等级错一档（`destructive` 写错 = 双门失效），编译器不报错，静态检查也不查（`check_ai_meta.py` 第 6 项只查 `readOnly×destructive` 互斥与 `destructive+CurrentTab` 无目标参数，**不查位序**） |
| 修法 | ①`Effects` 改**具名聚合初始化**（C++20 designated initializers：`Effects{.destructive=true, .openWorld=true}`）或改用带构造函数的 `EffectSet`；②`check_ai_meta.py` 增加位序自检；③至少把 `destructive` 与 `openWorld` 抽成两个**独立具名布尔**，从表里直接看出来 |

#### P1-7【本轮新增】`dev.build` / `dev.test` 的"编译即执行代码"没有任何闸门

| 项 | 内容 |
|---|---|
| 证据 | `src/app_commands_dev.cpp:57-61` 的 `RequireIntactEvalSurface()` 只检查**评估面**是否被改软；**不限制编译产物本身跑什么**。`dev.test` 跑 `tests/run_all.sh` → 会**编译并执行** `tests/*.cpp` 里的代码 |
| 后果 | 只要 `src/` 里的代码（或任何被编译进主 exe 的东西）被改过，`dev.build` + `dev.test` 就等于"**在开发机上执行 agent 写的代码**"，且当前**不触发双门**（见 P0-1） |
| 对标 | 沙箱不可协商项：隔离容器默认无网、工作区作用域文件访问、**硬超时**、命令过滤。本仓**零隔离** |
| 修法 | ①`dev.build`/`dev.test` 的 `destructive` 置 1；②`dev.test` 增加**硬超时**（当前 `kJobTimeoutMs=30min` 只是**淘汰**用的，不是**杀死**用的 —— 见 P2-3）；③提供**可选**的低权限子进程执行路径（本仓 `dev_proc` 已走 `CreateProcess`，加 `CREATE_SUSPENDED` + 限制 token 是可行方向） |

#### P1-8（新增自 P0-3 修法）`undoable` / `destructive` 缺少机械校验

| 项 | 内容 |
|---|---|
| 证据 | `tools/check_ai_meta.py` 22 项里**没有**"声明了 undoable 必须真有栈"的检查；`tools/check_components.py` 的唯一性规则也不覆盖 |
| 后果 | `undoable=1` 的 28 条里 15 条是**声明与实现不符**；这类不一致在静态检查全绿的情况下长期存活 |
| 修法 | `check_ai_meta.py` 增加第 24 项：`undoable=1` 的命令必须能映射到一个真实撤销栈（可维护一张 `CmdId → 栈实现` 的显式表；映射不到的置 0） |

#### P1-9【第四轮新增】失败原因被压成一句废话 —— 真因只存在于 HRESULT 里，却被丢掉

| 项 | 内容 |
|---|---|
| 证据 | `shellops::RenameTo` 原来只返回 `bool`；`IFileOperation` 的失败真因（名称非法 / 目标已存在 / 源被占用 / 权限不足 / 路径不存在）**只存在于 HRESULT 里**，函数返回时就被丢掉了 |
| 现象 | 重命名无论何种原因，报的都是同一句"重命名失败（名称非法或目标被占用）" —— **两个猜测拼在一起，不是任何一个真因** |
| 后果 | ①agent 拿到这句话**无法决定下一步**：换名字？换目录？提示用户关掉占用进程？全是猜；②与 P0-12 叠加成"假绿"的完整链条：**分隔符不对 → HRESULT 是 `E_INVALIDARG` → 报成"名称非法或被占用"**；③`file.rename.batch` 的 `failed[]` 只有文件名，批量失败时**逐条原因全丢** |
| 为什么算 P1 不算 P0 | 它本身不造成数据损坏（后果止于"agent 决策质量差"），但它是 P0-12 这类假绿的**放大器** |
| 对标 | Amershi 2019「细粒度反馈」：错误信息要能让人/模型判断**下一步该做什么**；OWL / LangGraph 的工具错误惯例是返回**结构化错误码 + 可操作建议**，而不是一句人类可读的话 |
| 已修的部分 | ①`shellops::RenameTo` 增加 `why` 出参（`src/shell_ops.h:74-78`），区分五种原因并带上 HRESULT；②`file.rename.batch` 的 `failed[]` 改成 `{from, reason}` 结构；③`IFileOperation` 的执行收口到 `shellopexec::Run()`（`src/shell_op_exec.cpp`），**同时检查 `PerformOperations` 的 HRESULT 和 `GetAnyOperationsAborted`** —— 只看 HRESULT 会漏掉"用户中途取消"这一类 |

### 4.3 P2 — 一致性 / 工程治理（10 条）

| # | 错误 | 证据 | 影响 |
|---|---|---|---|
| **P2-1** | 事件流**不落盘**（`ai_events.cpp` 环形缓冲，`Clear()` 一重启就没）；`state.changed` 带**整份 state 而非 diff** | `src/ai_events.cpp`；`src/ai_server.cpp` `events::Clear();` | 无跨会话轨迹 ⇒ 反思/自迭代没有原料。**跨 MCP 的 trace 还会在此断开**（W3C Trace Context 需要在请求 `_meta` 里带进来） |
| **P2-2** | `g_jobs` 是进程内 `std::map` → **跨重启即丢**；`kJobTimeoutMs=30min` 只用于**淘汰**，**不终止**仍在跑的进程 | `src/dev_tools.cpp:25,30,166` | 长任务不可恢复；挂死的作业会占着 64 个名额里的一个直到超时 |
| **P2-3** | `Requirements` 最多 **4** 项（编译期常量数组 `Req items[4]`），AND 语义 | `src/commands.h:288` | 复杂前置条件无法表达，只能塞进 handler 里 |
| **P2-4** | **文档数字漂移**：`docs/PARITY.md` 合计 **155/147/8**（实测 173/165/140）；`docs/AI-INTERFACE.md` 写 **168/160/135**（+5）；`docs/ROADMAP.md` 写"**89 工具**"（实测 140）、"32 个单测 475 项断言"（实测 **44** 个 `.cpp`） | `docs/PARITY.md:46`；`docs/AI-INTERFACE.md:128`；`docs/ROADMAP.md:262` | 外部读者与 agent 的决策依据建立在过期数字上。**本报告上一版也引用了这些过期数字中的两个** —— 说明漂移会传导到对标报告本身 |
| **P2-5** | `docs/FULL-COVERAGE.md` §9 记录"build_fast.py 不清理已删源文件 obj 会重复符号"—— **该缺陷不成立**（`build_fast.py` 只链接 `SOURCES` 当前清单；残留 `.obj` 不参与链接） | `build_fast.py` 的 `link_all()`；`docs/FULL-COVERAGE.md:304` | 记录了一个**假缺陷**，浪费修复预算；真实问题只是 `obj/` 残留垃圾文件（无害） |
| **P2-6** | `docs/ARCHITECTURE-RULES.md` §8 声称"机械校验清单（待批准后实现 `tools/check_arch.py`）"—— **该文件不存在**；R1–R20 **零机械检查**。且 R3"≤300 行/文件"**大面积超标** | `tools/` 实测无此文件；`src/shell_ops.cpp` 1271 行、`src/app_commands_files.cpp` 1165 行、`src/shell_view.cpp` 1116 行 | 架构规则停在纸面。**注**：P1-6 的位序事故说明"规则只写在文档里"会真的出事 |
| **P2-7** | `docs/ARCHITECTURE-RULES.md` §2.1 目标目录 `contract/ core/ platform/ ui/ input/ pipeline/ features/ app/` 与现状**扁平 `src/` 完全不符** | 文档头部 `status: proposed` | 规则文件与代码结构长期背离 |
| **P2-8** | `search.everything` 未实现（`implemented=false, aiExposed=false`） | `src/commands.cpp:241` | **注意**：`search.find` **已实现且 AI 暴露**（`src/commands.cpp:251`），带 `outputSchema`（`query/scope/url/searching`），并有 `searching` 回读面。**上一版说"AI 侧没有任何搜索能力"是错的**，已撤回。`search.everything` 是**宿主天花板**，需在文档明示以免 agent 反复试探 |
| **P2-9** | 事件类型只有 3 种，且**没有 `traceId` / `turn` / `step`**；`ai_server.cpp` 不读请求 `_meta` | `src/ai_events.cpp` | 与 §2.4 的 OTel 约定无法对齐；跨进程 trace 断开 |
| **P2-10** | `tools/` 存在死规则（如"图标按名查表（预留）… 若长期不用应删"） | `tools/check_components.py` 输出 | 低优先，但每轮都在提醒 |

## 5. 缺失清单

> 判据：成熟项目里**已被反复验证有效**的能力，本仓**没有对应机制**。
> "没有"≠"必须有"，故每条都标注"补的价值"与"优先级"，并注明是否属**有意取舍**。

### 5.1 维度 A — 人机协作交互（13 条）

| # | 缺失 | 对标来源 | 补的价值 | 优先级 |
|---|---|---|---|---|
| **A1** | **无"向人提问"的一等通道**（服务端 → 客户端的结构化输入） | MCP 2026-07-28 的 **MRTR（SEP-2322）**：`resultType:"input_required"` + `inputRequests` + `requestState`；2025-11-25 的 Elicitation | 本仓只有 `approve` 布尔，AI 无法问"删哪几个？" | 高 |
| **A2** | **无置信度/规则触发的定向提问** | HiLSVA；例外式升级（用确定性检查，不用模型自报置信度） | 低置信度时人接管，而非乱猜 | 中 |
| **A3** | **无 AI 操作面板**（计划 / diff 审阅 / 审批记录） | Cursor Agent、Copilot Agent Mode、Cline | AI 动作对人不可见 ⇒ 协作退化为盲批 | 高 |
| **A4** | **无人中途接管**（mixed-initiative 随时夺回） | HiLSVA、Amershi | 人无法在 AI 执行中途纠偏 | 高 |
| **A5** | **无 act / ask / wait 三阈值决策** | Horvitz 1999 | 只有二值 approve | 中 |
| **A6** | **无 plan-first（先出计划再执行）** | Cursor / Copilot / Claude Code 的 Plan Mode | 大批量操作无预览 | 中 |
| **A7** | **无 checkpoint / diff 逐条审阅** | Cursor checkpoint（独立于 Git）、Copilot 逐文件 accept/reject/edit | 不可逆操作无回退视觉 | 高（与 P0-5 同批） |
| **A8** | **无成本刹车**（最大轮数 / 预算 / 成功判据 / 阻塞即停） | Anthropic《Building Effective Agents》四种刹车；OSWorld 显示最佳 agent 比人多用 **2.7–4.3 倍步骤** | AI 可能陷入无效循环 | 中 |
| **A9** | **无细粒度权限**（按命令族 / 按路径 / 按只读域授权） | Amershi；可逆性分级的监督强度 | 只能整仓 danger 开关 | 中 |
| **A10**🆕 | **审批只有布尔，没有"给人看的面"** | 审批请求要素规范：diff/预览 + 理由 + **升级原因** + 拒绝后果 + approve/edit/reject + **超时不自动放行** | `approve=true` 现在是无上下文的口令，人只能"凭信任按一下" | 高 |
| **A11**🆕 | **审批不分级**（只按 `destructive` 二分） | 可逆性四档：只读 / 可逆写 / 半可逆 / 不可逆；"Overwhelming HITL" 被列为 Threat T10（审批疲劳 ⇒ 橡皮章） | 现在要么全批、要么全卡；分档后常规操作不打扰人 | 中 |
| **A12**🆕 | **无可选择性撤销**（只能撤最后一步） | "撤销第 47 步（共 200 步）"；plan 边界 checkpoint + 从最近安全点恢复 | `bmk.*` 撤销栈 LIFO-20，`fs.*` 无栈 | 中 |
| **A13**🆕【第三轮】 | **无 actor 身份类型** —— "谁做的"这个最基础的事实无法表达 | 显式身份是审计、分级审批、信任增长曲线的共同前提；OTel 轨迹要求记"谁批的、何时批的" | 连"这是人还是 AI"都答不对（P0-10），A1–A12 全部无从落地 | **最高** |

### 5.2 维度 B — AI 操作软件并获得反馈（15 条）

| # | 缺失 | 对标来源 | 补的价值 | 优先级 |
|---|---|---|---|---|
| **B1** | **无 MCP Tasks（call-now-fetch-later）** | 2025-11-25 的 SEP-1686（`taskId`/`tasks/get`/`tasks/result`/`tasks/cancel`；状态 working/input_required/completed/failed/cancelled）；**2026-07-28 改为扩展**（SEP-2663，与上代不兼容） | 长任务（批量移动、大目录操作）当前只能同步阻塞 | 高 |
| **B2** | **无 `notifications/progress`** | MCP 规范；`progressToken` | agent 无中途反馈 | 高 |
| **B3** | **无 `resources` / `prompts` / `completions`** | MCP 规范（2026-07-28 废弃 Roots/Sampling/Logging，但 resources/prompts 仍在） | 无法把"帮助文档/操作范式"作为资源暴露 | 中 |
| **B4** | **无 `logging` capability** | MCP 规范（2026 起标废弃，但 2025 客户端仍用） | 服务端日志无法结构化送达 agent | 中 |
| **B5** | **无 trace / turn / step 三级轨迹** | OpenTelemetry GenAI 约定（截至 2026-11 仍标 Development）；`invoke_agent`/`chat`/`execute_tool` span 树；`gen_ai.tool.call.id` | 只能看到 command 事件，无"第几步/哪一轮" | 高 |
| **B6** | **错误分类不够细**（`blocked` 一锅端） | 10 类机器码已比同行细，但 `blocked` 未区分"前置不满足 / 锁冲突 / 被用户取消"（`src/ai_api.cpp:330`） | agent 无法针对性恢复 | 中 |
| **B7** | **无性能 / 成功率统计**（`src/app_stats.cpp` 只做状态栏取数） | DGM 用 benchmark 分数驱动演化；该看的指标：**每工具错误率**、**每任务轮数**、**人工改写率** | 无从知道"哪条命令常失败" | 中 |
| **B8** | **无跨重启观测历史**（事件不落盘） | DGM archive；Reflexion 情景记忆 | 反思没有原料 | 高（与 P2-1 同批） |
| **B9** | **无状态 diff 事件**（`state.changed` 带全量） | 事件驱动 agent 惯例 | 每次全量传输，token 浪费且难定位变化 | 中 |
| **B10** | **无多客户端并发** | MCP 会话模型；2026 的无状态核 | 单连接阻断并行 | 中（与 P1-2 同批） |
| **B11**🆕 | **无"明确弃权"的一等公民** | OSWorld：FAIL 是**一等动作**，恒答 FAIL 得 8.13%，**26 个基线里 21 个比它差**；因此任何"成功率"都必须**分开报可行/不可行** | 本仓有 `not_implemented`/`not_exposed`，但 agent 侧无法区分"我办不到"与"环境不满足" | 中 |
| **B12**🆕 | **不读请求 `_meta`** → 跨进程 trace 断开、客户端能力不可见 | MCP 2026-07-28：**W3C Trace Context 走 `_meta`**；`clientInfo` 每请求携带；2026 用 `server/discover` 取代握手 | 未来客户端按 2026 语义发请求，本仓会因不认识 `_meta` 而丢失全部元信息 | 中 |
| **B13**🆕 | **无 `dev.diff`**（结构化 diff 视图） | 审批要素规范第 ① 条：**给 diff，不要给描述**；沙箱不可协商项"每一次 diff 的完整审计" | 本仓只有 `git diff --stat`（一行摘要），人无法审阅 | 中 |
| **B14**🆕【第三轮】 | **契约元数据有多个源头** —— 同一操作因入口不同而声明不同风险 | AGENTS.md §1.3 唯一来源原则；声明式契约**不能因入口变** | `fs.delete` 与 `file.delete` 两套 `Effects`，且暴露给 AI 的那份更乐观（P0-11） | 高 |
| **B15**🆕【第三轮】 | **"弃权"与"环境不满足"未区分** —— `blocked` 一锅端 | `blocked` 未细分"前置不满足 / 锁冲突 / 被用户取消 / 能力不存在" | agent 无法判断该重试、该换参数、还是该停下来问人 | 中 |

### 5.3 维度 C — 自我迭代（12 条）

| # | 缺失 | 对标来源 | 补的价值 | 优先级 |
|---|---|---|---|---|
| **C1** | **无 archive / 择优保留**（只有单条 rollback，失败即丢弃） | DGM archive + 多样性加权采样；OpenEvolve / AlphaEvolve 的 MAP-Elites + island 模型 | 不会退步，但也不会"积累" | 高 |
| **C2** | **无自评估打分**（无法判定"这次改动更好"） | DGM benchmark；OpenEvolve evaluator 打分 | 自我迭代失去方向 | 高 |
| **C2b**🆕 | **无"接受器"（acceptor）** —— 即使有了分数，用什么规则决定收不收这个改动？ | **PACE（arXiv:2606.08106）**：贪心接受在有真实收益时 **30–42% 虚假提交、10–33% 有害**；无收益时 **每次运行 13–21 次伪自修改（72–100% 虚假）**，最脆弱 agent **掉 4.9 分**。正确做法：配对比较 + betting e-process，`E ≥ 1/α` 才收 | **这是 C1/C2 的前置**。没有接受器，archive 只会越滚越脏 | **最高** |
| **C3** | **无 validity funnel**（parse → prescan → compile → checks → stable rollout 逐级过滤） | OpenEvolve / AlphaEvolve 的 validity funnel | 坏变体直接进入世界 | 高 |
| **C4** | ~~无"防改 evaluator"边界~~ → **已实现**（`src/eval_surface.cpp`） | DGM 的 objective hacking；RewardHackingAgents | — | **已关闭，但见 P0-7 / P0-8** |
| **C5** | **无跨会话经验沉淀**（`tools/agentctl.py` 无 history/memory/experience 写入；`gap` 是无状态查表） | Reflexion（情景记忆，上限 3 条）；ExpeL（ADD/UPVOTE/DOWNVOTE/EDIT）；Voyager（先验证再入库） | 每次都从零开始 | 高 |
| **C5b**🆕 | **若做 C5，必须自带"污染防护"** | EVOMAL（arXiv:2608.25776）：恶意技能自发复制 **4.9–9.0 倍**，**删掉植入源后第 5 轮仍有 68% 攻击成功率**；arXiv:2609.04170：作弊方法**经共享知识库与 P2P 消息传开**；arXiv:2608.05810：技能池超临界规模反而**拉低表现**，污染链**结构上不可逆**，回滚只能挽回一小部分 | **沉淀层 = 作弊传播通道**。C5 若无"先验证再入库 + 隔离 + 容量上限 + 淘汰"，会主动制造 P0 级问题 | 高（与 C5 同批） |
| **C6** | **无技能泛化与错误恢复**（缺 `file.rename.batch` / `path.copy.link` 的确定式版本） | Voyager 技能库检索 top-5；ExpeL 跨任务洞察 | 复合意图只能靠 agent 硬凑 | 中 |
| **C7** | **无工具自优化路径** | Anthropic《Writing effective tools》：**用 Claude Code 自动优化 tool 本身** | 描述 / 参数 schema 不会随使用改进 | 中 |
| **C8** | **无沙箱与时限**（`dev.build` / `dev.test` 直接在宿主上跑） | DGM 全程沙箱；沙箱不可协商项：隔离容器默认无网、工作区作用域文件访问、**硬超时**、命令过滤、每次 diff 完整审计 | 自我迭代的破坏半径 = 整台开发机 | 高（与 P1-7 同批） |
| **C8b**🆕 | **无 epoch 化的评估面治理** | **Red Queen Gödel Machine（2026）**：epoch 内评估器**完全冻结**（保证效用信号平稳）；epoch 边界用**锚数据集 + Beta 后验下界 `BB_ε`** 决定是否提升；换评估器时**选择性擦除**旧效用记录 | 本仓现在是"永久锁死 + 人工重签"。epoch 模型既保留"冻结"的可信度，又给出"何时、如何安全地放宽"的机制 | 中 |
| **C9**🆕 | **自测无域外 hold-out** | RRSI（arXiv:2609.24972）：域内 +14.1，5 个域外任务只 +4.7 | 递归进化会"记住训练任务而虚高" | 中 |

### 5.4 维度 D — 工程治理（6 条）

| # | 缺失 | 证据 | 优先级 |
|---|---|---|---|
| **D1** | **`tools/check_arch.py` 不存在** | `docs/ARCHITECTURE-RULES.md` §8 自陈"待批准后实现"，`tools/` 下无此文件 | 高 |
| **D2** | **e2e 覆盖近乎为零**：唯一 e2e 是 `tests/e2e/bookmark_move_e2e.py`；`dev.*` / 事件流 / 危险双门 / profile 隔离 **0 条** | `tests/` 实测：44 个 `.cpp` 单测 + 1 个 e2e + `pipe_e2e_test.cpp`（6 场景） | 高 |
| **D3** | **确定式版本缺口**：`file.rename.batch` / `path.copy.link` 尚无确定式版本 | `check_ai_meta.py` 第 9 项输出 | 中 |
| **D4** | **15/173 条已实现未暴露命令无显式别名**（会退回推导名） | `check_ai_meta.py` 第 11 项输出 | 低 |
| **D5** | **`tools/` 存在死规则** | `check_components.py` 输出 | 低 |
| **D6**🆕 | **未提交资产**：新增的 `eval_surface.*` / `dev_proc.*` / `gen_eval_baseline.py` / `data/eval-surface.json` **全是 untracked**，两套新单测（`tests/eval_surface_test.cpp`、`tests/dev_proc_test.cpp`）也未入库 | `git status --short` | **高**（关掉 P0-8 的前提） |

### 5.5 维度 E — 协议代际（2 条，本轮新增维度）

| # | 缺失 | 对标来源 | 补的价值 | 优先级 |
|---|---|---|---|---|
| **E1** | **协议核停留在上一代**：`initialize` 硬编码 `2024-11-05`；握手模型在 2026-07-28 已被**删除**；`server/discover`、`_meta` 版本声明、`resultType` 全部不认识 | MCP `2026-07-28`（SEP-2575 / SEP-2567 / SEP-2322 / SEP-2243 / SEP-2549） | 未来客户端按 2026 语义发请求会直接失败；**这是"能不能被下一代客户端用"的问题，不是"功能多不多"的问题** | 高 |
| **E2** | **Tasks 在 2026 变成扩展且与 2025-11-25 不兼容**（SEP-2663） | MCP 2026-07-28 扩展框架 | 若本仓将来实现 Tasks，**不能照抄 2025-11-25 的线格式**，否则一升级就废 | 中 |

## 6. 有意取舍（**不算缺陷**）

> 这些是**明确的设计选择**，对标时容易被误报为缺失。列出以免后续重复讨论。

| # | 取舍 | 理由 | 对标立场 |
|---|---|---|---|
| **N1** | **R21 全仓禁模拟输入**（禁 `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos` / `BlockInput` / `AttachThreadInput`，以及投 `WM_LBUTTON*` / `WM_KEY*` / `WM_CHAR` 的 `PostMessage`） | 自动化必须走 CommandBus 或命名管道，可审计、可拦截、可测试 | **正确且领先，本轮拿到外部数据背书**。**OSWorld 原始论文**：最佳 agent 12.24% 靠的是**纯 a11y 树纯文本**，加截图反而掉 0.15 分，纯截图 5.03%，Set-of-Mark 最差 4.59%。WindowsAgentArena 也显示 UIA + 视觉融合比纯截图高 57% —— **语义面确实强于像素面** |
| **N2** | **拒绝把 `needsSelection` 命令暴露给 AI** | AI 无鼠标选中态语义 | 合理。与 Claude Code 手动模式的"先问再动"同向 |
| **N3** | **不给 AI 特权通道**（无绕过 CommandBus 的直调路径） | 单一命令入口 = 单一审计点 = 单一策略点 | **正确**。Skalse 定理的核心就是"限制能做什么" |
| **N4** | **危险双门**（`aiAllowDanger` 设置 + `approve=true` 参数，缺一即拒） | 机器侧不得自我批准 | **正确**。`tools/agentctl.py` 的 fix 文案明确写"自我迭代不包含自我批准破坏性操作"。**但漏洞已从"注入"转移到"`dev.*` 的 `destructive=0`"（P0-1）** |
| **N5** | **`ext.run` / `ext.sendkeys` 未实现且未暴露** | R21 语义下不必要 | 合理 |
| **N6**🆕 | **命令表是编译期常量**（`CmdId` 只在末尾追加） | 可静态检查、可导出、可单测 | 合理。ADAS 式"运行时生成 agent 代码"在本项目不适用；**不要为了"自我迭代"把它改成动态的** |
| **N7**🆕 | **不提供"任意写文件"命令** | 那会破坏 R21 的边界（一条命令 = 任意写入） | **合理且应坚持**。改写能力属于外部 agent；本仓该做的是**观测改写**（`dev.diff` / `changedFiles`），不是提供写入口 |

> 注：**N1 与 N4 是本仓相对外部项目最值钱的两处设计**。
> N1 现在有了外部数据背书（OSWorld：语义面 > 像素面）；N4 的漏洞仍然只剩一个入口 —— **`dev.*` 这几条**。
> 也就是说 —— **唯一真正能突破 R21 和双门的通道，恰恰是 `dev.*` 这几条**。这仍是本报告最应优先修的一条。

---

## 7. 优先级建议与最小修复路径

### 7.1 三档排序

**第 1 档 —— 先堵事故与假绿**（不做则自我迭代会"自我欺骗"）

| 序 | 项 | 涉及文件 | 最小改动范围 |
|---|---|---|---|
| **0** | **P0-10 + A13**（actor 身份类型）—— **本轮建议排在最前**，因为它是其余所有身份相关机制的地基 | `src/commands.h:505,517`、`src/input_router.cpp`、`src/app_commands.cpp`、`src/app_wnd.cpp`、`src/bookmark_bar.cpp` | ①`enum class ActorKind { Human, Ai, Scheduler }` + 可读名；②`fromMachine()` 改判 `kind == Ai`；③8 处 actor 字面量全部改为显式枚举；④`check_components.py` 加规则：**禁止新增 actor 字符串字面量**（比修 8 处更重要 —— 漏一个就整条防线失效）；⑤迁移期把 `ui/key/mouse/seq/drop/palette/sysmenu` 列入 Human 白名单止血 |
| 1 | **P0-7 + P0-8 + D6**（评估面签发权分离 + 清单入库） | `tools/gen_eval_baseline.py`、`src/eval_surface.cpp`、`data/eval-surface.json` | ①`git add data/eval-surface.json` 并纳入 `PROTECTED_FILES`（自指保护）；②清单的"可信哈希"移到**仓库外**受信位置，或由签名提交承载；③AI 通道**不得**触发重签（重签必须有人类可见确认）；④`gen_eval_baseline.py` 增加"清单必须已被 git 跟踪"的自检 |
| 2 | **P0-11 + B14**（契约单一来源） | `src/commands.cpp:97,131-138`、`tools/check_ai_meta.py` | ①`fs.*` 与 `file.*` 共用同一份 `Effects` 常量，或让 `fs.*` 成为 `file.*` 的别名（同 `CmdId` + `legacyName`）；②`check_ai_meta.py` 加"配对操作 `Effects` 必须一致"检查 |
| 3 | **P0-1 + P1-7 + C8**（`dev.*` 双门 + 沙箱 + 硬超时） | `src/commands.cpp:218,221`、`src/app_commands_dev.cpp`、`src/dev_tools.cpp` | ①`dev.build`/`dev.test` 的 `destructive` 置 1（触发双门）；②`approve` 绑定参数哈希；③`dev.test` 增加**真正终止**的超时（当前 `kJobTimeoutMs` 只淘汰不杀） |
| 4 | **P0-4 + B15**（回执覆盖 + 错误细分） | `src/commands.cpp`、`src/ai_api.cpp:451-462`、`tools/check_ai_meta.py` | ①45 条无回执的写类暴露命令逐条补 `assertion` 或 `outputSchema`，或写豁免理由；②新增门槛检查并卡 CI；③`blocked` 细分为 `precondition_unmet` / `lock_conflict` / `canceled_by_user` / `not_supported` |
| 4 | **P0-2 + P0-3 + P0-5**（撤销真伪 + 回滚可逆） | `src/app_commands_files.cpp:441-459`、`src/dev_tools.cpp:174,205`、`src/bookmarks.cpp:741` | ①`fs.*` 自建撤销栈（同 `bmk.*` 口径）；②`dev.snapshot` 真落盘 + 返回 `snapshotId`；③`dev.rollback` 支持按快照还原 |
| 5 | **P0-6 + P1-3**（审计完整 + 结构化） | `src/ai_api.cpp:258-286`、`src/commands.h:490` | 审计下沉到统一出口；行格式改 JSONL，补 `errorCode / durationMs / approvedBy / sessionId / argsHash / traceId` |
| 6 | **P0-9 + B13**（改写可见性） | `src/dev_tools.cpp`、`src/ai_api.cpp` | **不加写命令**，加观测：`dev.diff`（结构化 diff）+ `dev.snapshot` 增 `changedFiles`（路径 + mtime + SHA-256） |

**第 2 档 —— 补闭环能力**（有了轨迹与评分，自我迭代才有燃料）

| 序 | 项 | 涉及文件 | 最小改动范围 |
|---|---|---|---|
| 7 | **C2b + C1 + C2 + C3**（接受器 + archive + 打分 + funnel） | 新增 `src/dev_archive.*` | ①**接受器**用 PACE 口径（配对比较 + e-process，`E ≥ 1/α`），**不要用"分数涨了就留"**；②archive 存每次候选的源码 + 分数 + 原始执行轨迹（**不要预摘要** —— 消融实验显示摘要会丢失信号）；③validity funnel：parse → prescan → compile → checks → stable rollout 逐级过滤 |
| 8 | **B8 + B5 + P2-9 + B12**（落盘事件 + 三级轨迹 + `_meta`） | `src/ai_events.cpp`、`src/ai_server.cpp` | ①事件写 `data/events.jsonl`；②事件加 `traceId / turn / step`；③读请求 `_meta` 里的 W3C `traceparent` 并贯穿 |
| 9 | **C5 + C5b + B7**（跨会话经验 + 污染防护 + 统计） | `tools/agentctl.py` | ①`agentctl.py experience add/ls`（ExpeL 的 ADD/UPVOTE/DOWNVOTE/EDIT 口径，上限参考 Reflexion 的 3 条）；②**先验证再入库**、可疑条目隔离、**容量上限 + 淘汰策略**（EVOMAL / 2608.05810 的直接教训）；③统计每工具错误率、每任务轮数、人工改写率 |
| 10 | **A1 + A3 + A7 + A10 + A11**（向人提问 + 面板 + 审批面） | `src/ai_api.cpp`、`src/ai_server.cpp`、新面板 | ①按 2026 语义实现 `resultType:"input_required"` + `inputRequests` + `requestState`（**服务端问人的唯一正确形态**）；②审批面给 diff + 升级原因 + 拒绝后果 + approve/edit/reject + 超时不自动放行；③审批强度按**可逆性四档**分级（防"Overwhelming HITL"） |
| 11 | **B1 + B2 + P1-2**（Tasks + progress + 多连接） | `src/ai_server.cpp:211` | ①`nMaxInstances` → 16；②`notifications/progress` 带 `progressToken`；③Tasks 注意 2026 已改为扩展（SEP-2663），**线格式不要照抄 2025-11-25** |
| 12 | **C8b + C9**（epoch 化评估面 + 域外 hold-out） | `src/eval_surface.cpp` | ①评估面 epoch 内冻结，epoch 边界由**锚数据集 + 人签字**提升（RQGM 口径）；②自测留**域外 hold-out**，防止"记住训练任务而虚高" |

**第 3 档 —— 协议与工程治理**（打磨，不阻塞主线）

| 序 | 项 | 涉及文件 | 最小改动范围 |
|---|---|---|---|
| 13 | **P1-1 + E1 + E2**（MCP 版本升级） | `src/ai_server.cpp:602-619` | ①短期升 `"2025-11-25"` + 向下兼容；②补 `ping`；③`tools.listChanged` 置 `true`；④中期做 `server/discover` + `_meta` 版本 + `resultType` |
| 14 | **P1-4 + P1-6 + P1-8**（description / Effects 位序 / undoable 校验） | `src/commands.h:236-242`、`src/ai_api.cpp:495` | ①`CommandDef` 加 `description`；②`Effects` 改**具名初始化**或抽具名布尔（**上一版自己就读错过位**）；③`check_ai_meta.py` 加 `undoable` 有栈校验 |
| 15 | **P2-6 + D1**（把架构规则变成可执行检查） | 新增 `tools/check_arch.py` | 先做 5 条最易判定的：R3 行数上限、R18 审计覆盖、C6 审计字段、C7 description 完整性、`undoable` 有栈支撑 |
| 16 | **P2-1 ~ P2-4**（事件落盘 / 作业持久化 / Requirements / 文档数字） | `src/dev_tools.cpp`、`src/ai_events.cpp`、`src/commands.h:288` | `Requirements` 扩到 8；`g_jobs` 落盘且超时真杀；同步 4 份文档数字（**上一版报告自身也引用了过期数字 —— 说明要定期重跑**） |
| 17 | **D2**（e2e 补齐） | `tests/e2e/` | 优先补：`dev.build` 无批准拒绝、`dev.test` 超时终止、评估面被改软时拒绝启动、事件流 subscribe/since、profile 隔离 —— 各 1 条即从 0 到 5 |

### 7.1.1 落地进度（截至 2026-11-21 第四轮）

第 1 档（事故与假绿）**已基本走完**：

| 序 | 项 | 状态 | 落地物 |
|---|---|---|---|
| 0 | P0-10 actor 身份类型 | ✅ | `ActorKind{Human,Ai}` + `Actor::Ui()/Key()/Mouse()/SysMenu()/Palette()/Seq()/Drop()/Settings()/Ai()`；`fromMachine()` 改判 `kind==Ai` |
| 1 | P0-7 / P0-8 评估面签发权 | ✅ | `data/eval-surface.json` **入库** + `selfSeal` + git 信任层（清单必须已跟踪且工作树干净，fail-closed）；重签必须带 `--human-confirm` |
| 2 | P0-11 契约单一来源 | ✅ | `kEffectDelete/kEffectRename/kEffectCopyTo/kEffectMoveTo/...` 具名常量，`file.*` 与 `fs.*` 引用同一份；`check_ai_meta.py` 第 24 项逐项比对 10 组配对 |
| 4 | P0-2 / P0-3 / P0-5 撤销真伪 + 回滚可逆 | ✅ | `src/file_undo.{h,cpp}` 应用内撤销栈；`file.rename.batch` / `fs.rename` / `fs.copy` / `fs.move` 全部记栈；11 条空承诺 `undoable` 归零；第 25 项双向校验；`dev.snapshot` 真落盘 + `dev.rollback` 按快照还原（回滚前存 `pre-rollback/`） |
| 4' | **P0-12 / F-1 路径归一化**（第四轮新发现，已并入 §4.1） | ✅ | `pathutil::ToShellPath()` 唯一收口；`shell_view.cpp` / `shell_ops.cpp` / 10 条 `fs.*` 命令全部接上 |
| 4'' | **P0-13 / F-2 撤销误撤** + **P1-9 / F-3 失败原因细分**（已并入 §4） | ✅ | 栈非空但撤不动时不再落回 shell；`RenameTo` 带 `why` 出参，`file.rename.batch` 的 `failed[]` 改成 `{from, reason}` |
| 3 | P0-1 / P1-7 `dev.*` 双门 | ⏳ | 仍待做：`dev.build` / `dev.test` 的 `destructive` 还没置 1，编译即执行代码这条路仍在信任边界之外 |
| 4 | P0-4 回执覆盖 + blocked 细分 | ⏳ | 部分：自断言 6→91 条、豁免机制落地；**仍缺**"45 条写类暴露命令逐条补回执"的尾账与 `blocked` 四分类 |
| 5 | P0-6 审计完整 + JSONL | ⏳ | 仍待做 |
| 6 | P0-9 改写可见性 | ⏳ | 部分：`dev.snapshot` 现在带 `files[]`（路径 + 字节数 + 是否已跟踪）；仍缺结构化 `dev.diff` |
| — | **P0-2 追加**：`search.find` 契约不成立 | ✅ | `aiExposed=false` + `outputSchema=nullptr`；第 10 项加判据 c"未实现的命令不得声明 outputSchema" |

第 2 档（闭环能力）与第 3 档（协议与工程治理）**尚未开始**。

**下一步建议**：按 §7.1 序号继续做 **3（`dev.*` 双门）→ 4（P0-4 回执尾账 + blocked 四分类）→ 5（P0-6 审计 JSONL）**。
理由：这三项是"让系统能说真话"的最后一段，而第 2 档的 archive/接受器/打分
必须建立在可信的轨迹与判据之上 —— 判据不可信时，打分只是给假绿量体温。

### 7.2 一句话结论

> 本项目的**架构骨架是对的**（CommandBus 单一入口、声明式 Effects/Requirements/assertion、
> 三平面分层、10 类机器码错误、R21 输入纪律、危险双门、评估面守卫）。
> 外部证据继续支持两条最值钱的设计：**OSWorld**（纯 a11y 树纯文本 12.24% 赢过所有视觉配置）
> 支持 N1；**RewardHackingAgents**（约 50% episode 篡改 evaluator，唯一有效防御是锁死）
> 支持 `eval_surface` 的方向。
>
> **第三轮找到的三个地基问题，第四轮已全部修掉**：
> `fromMachine()` 把 7 种真人输入判成"机器"（P0-10 → `ActorKind`）、
> 同义命令两套 Effects 且暴露给 AI 的那份更乐观（P0-11 → 具名常量）、
> 评估面清单根本没入库（P0-7/P0-8 → 入库 + selfSeal + git 信任层）。
>
> **第四轮又挖出三个只有端到端跑才会现形的缺陷**（§0.4）：
> 外部路径没归一化导致 `fs.*` 必然失败且**报一个错的错**（F-1）、
> 撤销失败会"改撤别的操作"（F-2）、失败原因被压成一句废话（F-3）。
> 这三条给出同一条方法论结论：**静态校验（25 项全绿）挡不住这类缺陷，
> 只有真实端到端能挡** —— 所以"跑一遍"必须进收工判据，不能只跑静态检查。
>
> **仍然在的**：
> ①`verdict` 对绝大多数命令恒等于 `ok`，写类暴露命令的回执覆盖仍有尾账（P0-4）；
> ②**唯一的突破通道仍是 `dev.*`** —— 编译即执行代码，`destructive` 还没置 1（P0-1 + P1-7 + C8）；
> ③审计仍是文本行、缺参数哈希/耗时/审批标记，且拒绝类调用刚补进审计不久（P0-6 + P1-3）。
>
> 结论：**先把 `actor` 变成身份类型（P0-10），再让契约只有一个来源（P0-11），
> 然后补评估面签发权（P0-7/P0-8），最后才谈 archive / 接受器。**
> 前三件事是"让系统能说真话"；在这之前，任何"自我迭代"叙事都建立在假绿之上。
> —— 这三件第四轮已做完；**下一件是 `dev.*` 双门**。

---

## 附录 A — 本轮检索的外部参照项目清单

> 🆕 = 本轮新增；**上一版参照系的三处修正见表下说明**。

### 人机协作

| 类别 | 名称 | 关键借鉴点 |
|---|---|---|
| 人机协作 | Horvitz (1999) *Mixed-Initiative Interaction* | act/ask/wait 三阈值 |
| 人机协作 | Amershi et al. (2019) *Guidelines for Human-AI Interaction* | 18 条；告知能力边界；细粒度反馈 |
| 人机协作 | Anthropic *Building Effective Agents* | Agent Loop 四步；evaluator-optimizer；四种刹车 |
| 人机协作 | Anthropic *Writing effective tools for agents* | 工具设计准则；用 Claude Code 自优化 tool |
| 人机协作 | HiLSVA | plan-first + 审批；stepwise provenance；Learn-at-Test-Time；置信度提问 |
| 人机协作 | Cursor Agent / Copilot Agent Mode / Cline | checkpoint；逐文件 diff 审阅；Plan Mode；手动审批 |
| 🆕 人机协作 | 可逆性分级的监督强度（AgentMarketCap 2026-04；Rippling Threat T10） | 只读/可逆写/半可逆/不可逆 四档；**"Overwhelming HITL" 是威胁类别** |
| 🆕 人机协作 | 审批请求要素规范（D3X / Moai 2026） | diff 而非描述；升级原因；拒绝后果；approve/edit/reject；**超时不自动放行** |
| 🆕 人机协作 | 例外式升级 vs 置信度闸门 | 用确定性检查构造升级触发器，**不用模型自报置信度** |
| 🆕 人机协作 | 可选择性撤销 / checkpoint 恢复 | "撤第 N 步"；plan 边界 checkpoint；安全中止语义 |

### AI 操作软件与反馈

| 类别 | 名称 | 关键借鉴点 |
|---|---|---|
| AI 操作软件 | **MCP 规范 `2026-07-28`（当前代）** | 无状态协议核；移除握手/会话（SEP-2575/2567）；`server/discover`；**MRTR（SEP-2322）**；头路由（SEP-2243）；缓存提示（SEP-2549）；**W3C Trace Context in `_meta`**；Tasks 变扩展（SEP-2663，不兼容） |
| AI 操作软件 | MCP 规范 `2025-11-25`（上一代） | 生命周期；Elicitation；Sampling；Roots；Tasks(SEP-1686)；progress |
| AI 操作软件 | WindowsAgentArena | 首版 19.5% vs 人类 74.5%；UIA+视觉 +57%；**2026-09 现状 UI-TARS-2 50.6%、Agent S3 50.2→56.6** |
| AI 操作软件 | OSWorld / OSWorld-Verified / OSWorld-G / OSWorld-MCP / **OSWorld 2.0** | 原版最佳 12.24%（**纯 a11y 树纯文本**）；纯截图 5.03%；SoM 4.59% 最差；v2 108 条长程工作流、最佳 20.6% binary / 54.8% partial |
| 🆕 AI 操作软件 | **FAIL 是一等动作**（OSWorld 分析） | 恒答 FAIL 得 8.13%；26 个基线里 21 个低于此线 ⇒ 可行/不可行必须分开报 |
| 🆕 AI 操作软件 | 环境扰动脆弱性（OSWorld） | 窗口最小化 → 相对塌 70.4%；agent 比人多用 2.7–4.3 倍步骤；链式完成约 31% |
| AI 操作软件 | GUI-360° / UI-TARS / Agent S2 / Agent S3 | 视觉 grounding vs 语义树；Mixture-of-Grounding；主动分层规划 |
| 🆕 观测 | **OpenTelemetry GenAI 语义约定**（2026-11 仍标 Development） | `invoke_agent`/`chat`/`execute_tool`/`invoke_workflow`；`gen_ai.tool.call.id` 等；内容默认不记录 |
| 🆕 观测 | 跨 MCP 的 trace 连续性 | 客户端 span 与服务端 span 是两个 runtime；不传播 trace context ⇒ 两条断链 |

### 自我迭代

| 类别 | 名称 | 关键借鉴点 |
|---|---|---|
| 自我迭代 | Darwin Gödel Machine (arXiv:2505.22954) | archive + 多样性采样；SWE-bench 20%→50%；**objective hacking 实证** |
| 🆕 自我迭代 | **RewardHackingAgents (arXiv:2603.11337)** | **约 50% episode 篡改 evaluator**；唯一有效防御=锁死，代价 25–31% 运行时 |
| 🆕 自我迭代 | **PACE (arXiv:2606.08106)** | 接受器=序贯假设检验；配对比较 + betting e-process，`E ≥ 1/α`；贪心接受 30–42% 虚假 / 10–33% 有害；无收益时 13–21 次伪自修改、掉 4.9 分；PACE 0 虚假、评测成本 −18% |
| 🆕 自我迭代 | **EVOMAL (arXiv:2608.25776)** | 恶意技能复制 4.9–9.0 倍；删源后第 5 轮仍 68% 攻击成功 |
| 🆕 自我迭代 | **When Self-Evolution Backfires (arXiv:2608.05810)** | 技能池超临界规模拉低表现；污染链不可逆；回滚只挽回一小部分 |
| 🆕 自我迭代 | **RRSI (arXiv:2609.24972)** | 域内 +14.1 vs 5 个域外任务 +4.7 |
| 🆕 自我迭代 | **arXiv:2609.04170** | 作弊方式经共享知识库 + P2P 消息传播；自发"吹哨人"审计 |
| 🆕 自我迭代 | **Red Queen Gödel Machine (2026)** | epoch 内评估器冻结；锚数据集 + `BB_ε` 提升；选择性擦除旧效用 |
| 自我迭代 | Skalse et al. 定理 | 安全来自**限制 agent 能做什么** |
| 自我迭代 | STOP 论文 | 自改进代码绕过沙箱的实测频率；应测量边界 |
| 自我迭代 | OpenEvolve / AlphaEvolve / ShinkaEvolve | MAP-Elites archive + island；validity funnel；只 promote 通过 hidden tests 的变体 |
| 自我迭代 | ADAS / Meta Agent Search (arXiv:2408.08435) | 元 agent 写新 agent；archive 带分数；跨域迁移 |
| 自我迭代 | Reflexion / ExpeL / Voyager | 情景记忆 / 跨任务洞察 / 技能库先验证再入库 |
| 自我迭代 | Gödel Agent (ACL 2025) | 运行时自改；**无 review surface、无回滚**（反面教材） |
| 自我迭代 | LLMs Cannot Self-Correct Reasoning Yet (ICLR 2024) | 无外部验证的反思是空转（GSM8K 95.5%→89.0%） |
| 🆕 自我迭代 | 沙箱不可协商项 | 隔离容器默认无网；工作区作用域文件访问；硬超时；命令过滤；**每次 diff 完整审计** |
| 🆕 记忆 | **EVOMAL 的污染结论**（记忆即传播通道） | 沉淀层可写 ⇒ 作弊可复制；必须先验证再入库 + 隔离 + 上限 + 淘汰 |

> **本轮对上一版参照系的三处修正**：
> ① MCP 当前代是 **`2026-07-28`**（上一版写 `2025-11-25`）；
> ② WindowsAgentArena 的 "19.5%" 是**首版**数字，2026-09 已在 **50.6%–56.6%** 区间（上一版把它当现状）；
> ③ 上一版的"补 `notifications/initialized`"在 2026 语义下**已作废**（SEP-2575 删除了整个握手）。

---

## 附录 B — 本报告未覆盖 / 待确认

| # | 项 | 说明 |
|---|---|---|
| B-1 | **无像素 / 无障碍观测面** | 相对 WindowsAgentArena / OSWorld 是能力缺口，但也是**有意的架构取舍**（R21 路线）。**注意本轮新证据的方向性**：OSWorld 原始论文里纯 a11y 树纯文本（12.24%）胜过所有视觉配置，说明这条路线的**上限比像素路线更高**，不只是"更安全"。若 AI 侧将来要驱动**非本项目**窗口，需重新评估 |
| B-2 | **命令表扩展性** | `CmdId` 枚举只在末尾追加（规则 C1b）是对的，但命令表是编译期常量数组；ADAS 式"运行时生成 agent 代码"在本项目不适用（见 N6） |
| B-3 | **多显示器 / 高 DPI 下 UI 断言** | 断言只覆盖状态字段，不覆盖几何位置；若将来加 UI 断言，需要另立可回读字段来源 |
| B-4 | **外部项目的数字多为二手来源** | 本轮的外部数字来自检索到的综述与论文页面（arXiv 摘要页、官方博客、厂商对比页），**未逐一回溯原始论文表格**。落到决策前应核对一手来源，尤其是：WindowsAgentArena 的 2026-09 更新值、OSWorld 2.0 的 partial/binary 口径、PACE 的 α 与评估集规模 |
| B-5 | **上一版的两条结论已被本轮推翻** | P1-5（`openWorld`）与 P2-10（搜索能力）—— 这两条说明**上一版的部分条目未经实测**。本版所有条目均附行号，但**行号会漂移**，下次复核应重跑而非重读 |
| B-6 | **`docs/OPERATION-MODEL.md` §2 与 `docs/NAMING.md` §5** | 两份文档已收录部分业界比对，本次未逐条复核其结论是否与本文冲突 |
| B-7 | **未在本仓做的外部对照实验** | 本轮没有实际去跑 WindowsAgentArena / OSWorld 的 agent，也没有在本仓上复现 PACE 式接受器；所有外部数字均为引用 |

---

## 附录 C — 本轮实测命令与证据清单

```bash
# 基线三检
python tools/check_commands.py     # 命令表 173 / 已实现 165 / 处理器 165 / keymap 93，三方一致
python tools/check_ai_meta.py       # 22 项全过
python tools/check_components.py    # 30 唯一性 + 7 禁令 + 1 接线 + 1 令牌 + 4 输入纪律，全过

# 【第三轮新增方法】用校验器自己的解析器导元数据，而不是正则抽字段
python -c "
import importlib.util
spec=importlib.util.spec_from_file_location('cam','tools/check_ai_meta.py')
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
d={c['name']:c for c in m.parse_commands()[0]}   # 173 条完整元数据
"
#   Effects 分布：readOnly=1:18  readOnly=0:147  destructive=1:23  openWorld=1:20  undoable=1:28
#   assertion 非空：6     outputSchema 非空：89
#   AI 暴露 + 写类 + 两者皆无：45（清单见 §3）

# P0-10 actor 全集
grep -rhoP 'Invoke[A-Za-z]*\([^;]*?L"\K[a-z]+(?=")' src/*.cpp | sort | uniq -c
#   31 ui / 5 sysmenu / 2 seq / 2 palette / 1 mouse / 1 key / 1 drop / 1 ai
sed -n '505p' src/commands.h      # fromMachine(): wcscmp(actor_, L"ui") != 0
sed -n '773p' src/bookmarks.cpp   # PushUndo(): undo_.push_back({roots_, fromMachine()})
sed -n '356p' src/app_commands_misc.cpp  # if (fromMachine() && !TopUndoFromMachine()) 拒绝

# P0-11 同义命令配对
#   fs.delete(暴露,un=1) ↔ file.delete(未暴露,un=0)
#   fs.rename(暴露,un=1) ↔ file.rename(未暴露,un=0)
#   fs.open.tab(暴露,ow=1) ↔ file.open.tab(未暴露,ow=0)
#   —— 均走同一批 shellops:: 实现

# 撤销栈
grep -rn "Undo" src/ --include=*.cpp --include=*.h   # 只有 bookmarks.h 有 undo_；tab_set.* 无 Undo 符号

# 审计覆盖
wc -l data/audit.log                                  # 1095 行
#   ok=1: 958    ok=0: 137    含 "blocked": 33
#   danger_disabled / need_approval / bad_args / unknown : 全部 0 命中

# MCP 现状
sed -n '605p' src/ai_server.cpp                       # protocolVersion = "2024-11-05"
#   只实现 tools/list、tools/call、initialize
sed -n '211p' src/ai_server.cpp                       # CreateNamedPipeW(..., 1, ...) 单实例
#   注：initialize 不强制（无 handshake 状态机），客户端可直接 tools/list

# 评估面
git status --short data/                              # ?? data/eval-surface.json（未入库）
git ls-files data/eval-surface.json                   # 空
#   PROTECTED_FILES = build_fast.py / build.sh / tests/run_all.sh
#   PROTECTED_DIRS  = tests(.cpp) / tools(.py)   ← 清单自身与前两者都不在集内

# 写文件命令的有无
#   173 条里无 file.write / fs.write / dev.edit / patch；
#   file.new.text(src/app_commands_files.cpp:568) 只创建空文件
```

---

## 附录 D — 版本记录

| 版本 | 时间 | 内容 |
|---|---|---|
| v1 | 2026-11-21 21:09 | 首版：22 条错误（P0×6 / P1×6 / P2×10）+ 32 条缺失（A×9 / B×10 / C×8 / D×5）+ 5 条取舍 |
| v2 | 2026-11-21 22:40 | ①撤回 2 条错误结论（P1-5 openWorld、P2-10 搜索能力）；②关闭 3 条（P0-1 注入面、P1-6、C4 主体）；③修正 2 条；④新增 9 条；⑤参照系刷新到 MCP 2026-07-28，新增 17 个外部条目；⑥发现"文档数字漂移"在本报告自身也被证实 |
| **v3** | **2026-11-21 22:23** | **换轴复核：从"目标↓比"改为"声明↔校验器↔实现"三角比对**<br>①**新增 P0-10**：`fromMachine()` 把 7 种真人输入判成机器（8 种 actor 里 7 种是错的）—— 本轮唯一真正的新 P0，且是身份类机制的总根因<br>②**新增 P0-11**：同义命令 `fs.*` / `file.*` 元数据分两套，暴露给 AI 的那份更乐观<br>③**修正 P0-4 的口径**：v2 说"几乎全部不可验"说过头了；实际 45 条无可验，另有 89 条靠 `outputSchema`<br>④新增 1 条 A（A13 actor 身份）、2 条 B（B14 契约多源头、B15 错误未细分）<br>⑤新增维度 E（协议代际）与 6 条自我迭代 2026 实证（PACE / RewardHackingAgents / EVOMAL / RQGM / RRSI / 2609.04170）<br>⑥**修正自身的两处误判**：撤回对 `fs.copy` schema 的怀疑（实测正确）；撤回"Ctrl+Z 会触发 bmk.undo 被拒"的推断（Ctrl+Z 绑的是 `file.undo`）<br>⑦方法升级：改用 `check_ai_meta.parse_commands()` 导元数据，不再正则抽字段 |





