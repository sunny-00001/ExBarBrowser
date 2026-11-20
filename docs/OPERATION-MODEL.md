---
status: implemented-1to10
baseline: 2026-11-20
scope: 设计研究 + 第 1–10 步全部落地
---

# 操作模型：让每个操作同时对人可用、对机器可调

> 目标不是"加一个 AI 接口"，而是**把所有操作整理成一份结构化契约**，让
> 工具栏、快捷键、命令面板、外部脚本、AI 都只是这份契约的不同调用方。
>
> **进度**：第 1–10 步**全部落地**（参数对象化、效果四维化、前置条件、交互与确定分离、返回值、
> 命名空间化、对外暴露、内容补全、能力缺口补齐、偿清"只能靠选中"的债务），
> 判据均由单测 + 静态检查守住（20 个单测 / 354 项断言 + 14 组静态检查）。
> §0 只列第 1–7 步（机制层），第 8–10 步（内容层）见 §9；迁移顺序与做法见 §7。

---

## 0. 已落地部分（2026-11-20）

| 步 | 做了什么 | 判据 | 结果 |
|---|---|---|---|
| 1 | `CommandBus::Handler` 从 `function<bool(const std::wstring&)>` 改为 `function<bool(const Args&)>`；旧单值规格经 `ArgsFromLegacyArg` 白 schema 推导键名 | 新旧路径结果一致 | `tests/args_test.cpp` 22 项通过；**`ai_api` 的 `kArgKeys` 补丁表已删除** |
| 2 | `Risk{Read,Write,Danger}` → `Effects{readOnly,destructive,idempotent,openWorld,undoable,scope}`，127 条逐条重标 | 无自相矛盾；destructive 必须有目标参数 | `check_ai_meta.py` 第 6 项通过；分布：只读 14 / 破坏性 21 / 跨进程 22 / 幂等 41 |
| 3 | `Requirements` 声明式前置条件 + `CanExecute()` + `ReqReason()`，21 条常用命令已标注 | 不满足时返回可读原因而非崩溃 | `tests/precondition_test.cpp` 26 项通过 |
| 4 | 交互与确定分离：新增 `FsRename`/`FsDelete`/`FsCopyTo`/`FsMoveTo`；`shell_ops` 补 `SelectByPath`/`SelectNone`/`SelectedSinglePath`/`RenameTo` | 确定式命令有目标参数、不依赖选中 | `tests/fs_ops_test.cpp` 44 项通过；债务 4/7 已消化 |
| 5 | 返回值：`RegisterWithResult`/`InvokeResult` 独立通道 + `CommandDef::outputSchema`；`OpListDir`/`OpStat` 落地（`shell_ops::ListView`/`StatItem`） | 有 outputSchema 的命令必须真能返回 | `tests/result_test.cpp` 20 项通过 |
| 6 | 命名空间化：`src/cmd_naming.{h,cpp}` 131 条旧名 ↔ 点分名映射，`FindCommand` 双向解析 | 两种名解析到同一操作 | `tests/naming_test.cpp` 31 项通过 |
| 7 | 对外暴露：`ai_server` 加 JSON-RPC 2.0 / MCP 兼容层（`initialize`/`tools/list`/`tools/call`），工具补 `description` | MCP 客户端能直接连上说话 | `tests/rpc_test.cpp` 21 项 + `tests/pipe_e2e_test.cpp` 35 项（真管道往返）通过 |

> 表里的项数是**该步完成时**的断言数；后续步骤会往同一文件里加断言
> （如 `precondition_test` 26 → 43、`naming_test` 31 → 25 条被合并重写），
> 当前全量是 20 个单测文件 / **354 项断言**。以 `tests/` 里的实际数量为准。

三个已消除的旧缺陷：

1. **参数被静默丢弃**：`TabNew` 声明了 `path` 却在 handler 里没读。新检查项
   （`check_ai_meta.py` 第 3 项）会比较"schema 声明的键"与"handler 实际 `args.Int/Str/Bool` 取用的键"，
   不一致即失败。
2. **空 schema 的不安全默认**：122 条原为 `"{}"`，按 MCP 规范语义是"接受任意参数"。
   已全部改为 `{"type":"object","additionalProperties":false}`。
3. **前置条件缺失导致的哑失败**：`Invoke` 现在先过 `CanExecute`，拒绝时
   审计记 `(blocked: <原因>)`，日志也有。以前"无历史时点后退"是完全静默的。

### 0.1 已登记的设计债务

`CmdDelete` / `CmdCut` / `CopyToFolder` / `MoveToFolder` / `BatchRename` / `PathCopyLink`
这 6 条是**破坏性操作却只能作用于"当前选中项"**，没有目标参数。第 4 步已为其中 4 条
提供确定式版本（`FsDelete` / `FsCopyTo` / `FsMoveTo` / `FsRename`），剩余 3 条见 §7
的债务表。旧命令仍留在 `KNOWN_NO_TARGET` 里 —— 它们本身没有目标参数这件事没有变，
只是机器现在有别的入口可用了。

---

## 1. 结论先行

> **以下是 2026-11-20 早间（第 1 步开工前）的现状快照**，保留它是为了让"为什么当时要动"
> 有据可查；右列随之全部翻绿，对照见 §0 与 §9。

业界在"如何把操作做成可编程契约"上已经收敛出七条共识。按重要性排序：

| # | 共识 | 我们目前 |
|---|---|---|
| 1 | **操作是数据，不是代码分支** | 半数据：表在 `commands.cpp`，但元信息不全且混着展示字段 |
| 2 | **参数是具名类型化的对象**，不是位置字符串 | ❌ 单字符串 `arg`，靠 `_wtoi` 猜 |
| 3 | **效果用多个正交布尔描述**，不是一个 risk 等级 | ❌ 单轴 `Risk{Read,Write,Danger}` |
| 4 | **运行前可判定"此刻能不能做"**，并给出原因 | ❌ 只有编译期 `implemented` |
| 5 | **交互式与确定式分开**（问参数 vs 用给定参数） | ❌ 混在一起，是 `CmdRename` 等无法被机器调用的根因 |
| 6 | **读操作能返回值**（结构化） | ❌ handler 只返回 `bool` |
| 7 | **命名带命名空间且稳定可版本化** | ⚠️ `NavBack` 这种 PascalCase，无命名空间、无版本 |

**一句话**：我们不缺命令表，缺的是**命令表里的字段**。当前设计把"人能看懂"做够了，
把"机器能安全调用"该有的四样东西都省了：类型化参数、运行前条件、正交效果、返回值。

---

## 2. 参照物：五家各自解决了什么

选它们不是随机的 —— 它们分别代表"AI 工具协议""IDE 命令系统""桌面创作软件"
"文件管理器""操作系统外壳"，恰好覆盖我们要面对的全部调用方。

### 2.1 MCP（Model Context Protocol）— 工具对模型如何可信

MCP 的 `tools` 是当前 LLM 工具调用的实际标准。它强制/推荐的东西：

| 字段 | 约束 | 为什么重要 |
|---|---|---|
| `name` | 1–128 字符，仅 `A-Za-z0-9_-.`，服务器内唯一 | 可被程序稳定引用 |
| `title` | 人类可读名 | 展示与 API 名分离 |
| `description` | 给模型看：做什么、**什么时候不该用** | 模型靠它避免误用 |
| `inputSchema` | **必须是合法 JSON Schema 对象**，不能是 `null` | 参数可校验，不用猜 |
| `outputSchema` | 可选；给了就必须按它返回 | 结果可校验 |
| `annotations` | `readOnlyHint` / `destructiveHint` / `idempotentHint` / `openWorldHint` | 客户端据此决定是否弹确认 |
| `structuredContent` | 结构化结果；同时建议回一份序列化 JSON 文本 | 兼容只认文本的客户端 |

三条容易被忽略的细节，恰好是设计要害：

1. **无参数的工具也必须给合法 schema**：推荐 `{"type":"object","additionalProperties":false}`，
   而不是空对象或省略。空 schema 的语义是"接受任意参数"，是不安全的默认值。
2. **annotations 是 hint，不是保证**。规范原文要求客户端"除非服务器可信，否则必须把
   annotations 当作不可信"。所以安全判断不能只靠自报的 `readOnlyHint`。
3. **错误分两类**：协议错误（未知工具、参数非法 → JSON-RPC error）与执行错误
   （业务失败 → `isError: true` 的结果体）。混在一起会让调用方无法区分"我调错了"
   和"环境不满足"。

> 我们该抄的是**字段设计与错误二分**，不是 JSON-RPC 本身。

### 2.2 VS Code Commands — 命令如何同时服务 UI、键位、扩展

VS Code 的命令系统是"一个 ID 多处消费"的成熟范例：

- `registerCommand(id, handler)` 绑定实现；`executeCommand(id, ...args)` 编程调用。
- **ID 即契约**：`extension.commandName` 点分命名，跨扩展稳定。
- **注册与声明分离**：`registerCommand` 在代码里；`contributes.commands` 在 `package.json`
  里声明 `title` / `category` / `icon` / `enablement`。声明缺失 → 命令不出现在面板，
  但仍可被 `executeCommand` 调用（内外部两套可见性）。
- **`when` 子句管可见性，`enablement` 管可用性**，两者不是一回事：前者防止菜单塞满
  无关项，后者灰掉当前场景不可用的项。
- **一个命令的入参随调用来源而变**：编程调用按顺序传；命令 URI 传 JSON 数组；
  命令面板不传参；键位传 `args`；菜单传上下文（如选中的 URI）。

> 关键启发：**"能不能看见""能不能用""怎么传参"是三件独立的事**，
> 而我们现在只有 `implemented` 一个布尔，三件事都没表达。

### 2.3 Blender Operators — 交互式操作如何同时可脚本化

Blender 的 operator 模型是我见过对"人机同一操作"处理最细的：

| 机制 | 作用 | 对照我们的缺失 |
|---|---|---|
| `bl_idname = "namespace.op"` | 点分命名 | ❌ 无命名空间 |
| `poll(context)` | **运行前**判定能否执行；失败时给出"为什么灰"的提示 | ❌ 完全没有 |
| `execute()` | 用**给定参数**确定地执行（可脚本化） | ⚠️ 我们有，但参数是字符串 |
| `invoke()` | 向用户**问参数**（弹窗、取鼠标位置）后转 `execute()` | ❌ 与 execute 混在一起 |
| 注册的 `Property` | 参数同时是：API 入参、UI 控件、undo 记录 | ❌ 参数无类型 |
| 返回值 `{'FINISHED','CANCELLED','RUNNING_MODAL','PASS_THROUGH'}` | 区分成功/取消/模态/未处理 | ❌ 只有 `bool` |
| `undo` 标志 | 是否进撤销栈 | ❌ 无 |
| 上下文覆盖 | 显式指定作用对象，**不依赖当前选中** | ❌ 只能作用于"选中的" |

Blender 官方文档里还有一条对架构的明确告诫：**"operator 只应调用高层的、可单测的
函数，不在 exec/invoke 里写业务逻辑"**，并称把 `context` 到处传是"像癌症一样扩散"。
这条正好对应我们的 R2「UI 不实现业务」。

> 关键启发：**`invoke` 与 `execute` 必须拆开**。一件事如果需要问用户才能确定参数，
> 它就不是一个可编程的操作；但它可以拆成"可编程的确定性操作 + 一个负责问参数的 UI 壳"。
> 我们 `CmdRename`、`CopyToFolder`、`BatchRename` 全都卡在这里。

### 2.4 Directory Opus — 命令是产品 API

Opus 的立场最彻底：内部命令是**唯一动作通道**。

- 工具栏、菜单、热键、脚本、外部调用，全部落到同一张命令表（`Go`、`Copy`、`Select`、`Set`…）。
- 命令带**修饰符**（`@confirm`、`@sync`、`@ifset`、`@keydown`），把"执行策略"从命令名里
  分离出来。
- 外部调用走 `dopusrt.exe /cmd <Command> <args>`，只依赖稳定命令行契约，
  不需要知道内部协议。
- 工具栏/菜单是**声明式定义**（可被用户改、可导入导出），不是写死在 C++ 里。

> 关键启发：**修饰符（modifier）是好设计**。"确认/同步/条件"这些执行策略不该
> 变成命令名的变体（`DeleteWithConfirm`），而应是调用时的参数。

### 2.5 操作系统外壳（explorer.exe）— 最小可用的外部契约

即便粗糙如 explorer，也有稳定契约：`explorer.exe /select,"<path>"`、`/root,`、
`shell:Downloads` 这类**别名**。教训有两条：

- **别名比 GUID 好**：`shell:Downloads` 跨机器、跨语言稳定，CLSID 不稳定。
  我们有 `::{20D04FE0-...}` 这种 GUID 散落在代码里，属于同一类问题。
- **语法坑会被用户承受**：`/select,` 的逗号必须紧贴、路径必须带引号 —— 这些"正确但反直觉"
  的细节，在我们这里等价于"参数是一个需要自己拼的字符串"。**结构化的意义就是消灭这类坑。**

---

## 3. 收敛出的设计原则

把上面五家抽象掉实现细节，剩下的共识可以写成七条：

**P1 操作即数据。** 每个操作由一份结构化描述（名字、参数 schema、效果、返回 schema、
前置条件）定义；实现只是挂在这个描述上的回调。描述与实现分离，描述可被导出、被校验、
被机器读取。

**P2 参数是具名对象。** 不用位置参数、不用拼接字符串。原因不是好看，而是：位置参数
无法表达可选参数、无法做类型校验、无法向后兼容地加参数、无法自动生成 UI。

**P3 效果是多维正交描述。** 至少四个独立维度：
- 是否只读（`readOnly`）
- 是否可能不可逆（`destructive`）
- 是否幂等（重复调用是否等价）
- 是否作用于系统外部（`openWorld`，如启动进程、访问网络）

单轴 risk 做不到：`CmdPaste` 是"写"但不易撤销；`NavRefresh` 是"写"但幂等；
`OpenTerminal` 是"写"但作用于外部世界。三者的安全策略完全不同。

**P4 运行前可判定。** 每个操作提供"此刻能否执行"的判定，并给出**人能读懂的原因**
（Blender 的 disabled hint）。这让人看到灰按钮知道为什么，让机器在调用前就知道会失败 ——
而不是靠"调用并观察异常"来试错。

**P5 交互与确定分离。** 问参数的行为（弹窗、拾取鼠标）是**调用方式**，不是操作本身。
操作本体必须能用完整参数确定地执行；UI 只是"帮用户把参数填出来"。

**P6 读操作返回值。** 只读操作应当能返回结构化数据。否则"列目录""查属性"这类需求
就只能长成另一套并行的 API，破坏 P1 的单一契约。

**P7 命名空间 + 版本。** 名字点分、稳定、可演进。破坏性变更靠新名字（`fs.delete@2`）
而不是改语义。

---

## 4. 我们的现状与差距

### 4.1 已有的（不算差的地基）

- `CommandBus` 已是唯一入口，带 `actor`，已落审计 —— 这是 P1/P4 的前提。
- `CommandDef` 表已有 `name` / `title` / `group` / `implemented` / `risk` / `argsSchema` /
  `aiExposed` / `needsSelection`。
- 快捷键、序列键、鼠标规则都映射到同一张表。

### 4.2 差距（逐条对应上面的原则）

| 差距 | 具体表现 | 违反 | 后果 |
|---|---|---|---|
| **参数是单字符串** | `Handler = function<bool(const std::wstring&)>`；`ViewMode` 靠 `_wtoi(arg)` | P2 | 无法表达多参数/可选参数；加参数就得改所有调用点；我为此加了 `kArgKeys` 映射表，那本身就是这个缺陷的症状 |
| **`scope` 是中文展示串** | `L"标签栏"` / `L"文件视图"` | P1 | 机器无法 switch；展示字段污染 API 契约；无法本地化 |
| **risk 单轴** | `Read / Write / Danger` | P3 | 无法区分"幂等写"与"不可逆写"；`CmdPaste` 与 `NavRefresh` 同归 Write |
| **无运行前条件** | 只有编译期 `implemented` | P4 | 机器只能"调用→失败→重试"；人看到可用按钮点了没反应（我们已有这类 bug：无历史时点后退） |
| **交互混在操作里** | `CmdRename` 直接调 shell 的 rename 动词弹输入框 | P5 | 该命令**永远**无法被机器调用；`CopyToFolder` / `BatchRename` 同理 |
| **无返回值** | handler 返回 `bool` | P6 | `OpListDir` / `OpStat` 这类查询命令只能另起一套；现有 127 条里 7 条 `Op*` 全是 `implemented=false` |
| **命名无命名空间** | `NavBack` / `TabClose` / `CmdDelete` 三种风格并存 | P7 | `Cmd` 前缀是历史残留（区分"命令"与"导航"），语义上无意义 |
| **argsSchema 是字符串** | `const char* argsSchema` | P1 | 无法在编译期/加载期校验；无法与 handler 签名比对；拼错了运行时才发现 |
| **能力表与实现可能漂移** | `implemented` 手写 | P1 | 已靠 `check_commands.py` 兜住，但那是补丁不是契约 |

### 4.3 一个具体例子说明差距的代价

以"删除选中的文件"为例，业界做法与本项目做法对比：

| 步骤 | 业界做法 | 我们 |
|---|---|---|
| 调用 | `fs.delete({targets:[...], permanent:false})` | `CmdDelete` + 无参数 |
| 目标 | **显式传参** | 隐式取"当前选中" |
| 前置 | `poll()` 检查有选中、可写 | 无 |
| 效果声明 | `destructive=true, idempotent=false, undoable=true` | `Risk::Danger` |
| 机器可用 | ✅ | ❌（不知道用户选了什么） |

这就是为什么我上一轮把 `CmdDelete` 从 AI 白名单里剔除了 —— **不是不该暴露，
而是当前形态无法被安全暴露**。正确的修法是按 P2 给目标加参数、按 P4 加前置条件，
而不是永久禁止。

---

## 5. 目标形状

分七层。每层都是**数据**，不是代码分支。

### 5.1 操作身份

```
名字        点分小写 + 可选版本：nav.back / tab.close / fs.delete@2
标题        中文展示名（可换语言）
描述        给人和机器看：做什么、什么情况下不要用（MCP 的 description 惯例）
分组        稳定的机器标识（nav / tab / view / fs / win / app），不是中文串
```

命名要给旧名留映射：`NavBack ↔ nav.back`。迁移期两者都认，稳定后只留新名。

### 5.2 参数

一份 JSON Schema 对象（不是字符串），约定：

- 无参数也必须给 `{"type":"object","additionalProperties":false}`
- 每个参数有 `type`、`description`；枚举用 `enum` 而非约定整数
- 目标对象用**显式路径/ID 数组**，不依赖"当前选中"

参数从"单字符串"变成"对象"是本轮最核心的改动；它一次性解决多参数、可选参数、
类型校验、自动生成 UI、向后兼容加参数五件事。

### 5.3 前置条件

每个操作提供 `可以执行吗 → (bool, 原因)`：

```
nav.back        → 无历史时：false，"已经是第一条历史"
fs.delete       → 无选中时：false，"未选中任何项目"
fs.paste        → 剪贴板空时：false，"剪贴板没有可粘贴的内容"
```

人侧：按钮灰显 + tooltip 给出原因。
机器侧：调用前就能问，或在返回体里拿到 `precondition_failed` + 同一个原因。

### 5.4 效果

四个正交布尔 + 一个作用域：

```
readOnly      不改任何状态
destructive   可能不可逆（删除、覆盖、批量改名）
idempotent    同参数重复调用等价（刷新、排序：是；粘贴、新建：否）
openWorld     作用于进程外（启动程序、访问网络、写注册表）
scope         currentTab / app / system   ← 决定"需不需要点名目标"
undoable      是否进撤销栈
```

`scope=currentTab` 且 `readOnly=false` 的操作，**必须**能用参数指定标签，否则无法安全被机器调用。

### 5.5 返回

只读操作给 `outputSchema` + 结构化结果；写操作至少返回"做了什么"的可核对描述
（改了哪些对象、是否可撤销），而不是一个 `bool`。

### 5.6 调用方式

把"交互"从操作里剥出来，作为**入口层**的概念：

```
execute(op, args)     确定式：用给定参数执行，绝不弹窗、绝不读选中
invoke(op)            交互式：由 UI 收集参数后转 execute
```

`CmdRename` 拆成：`fs.rename({target, newName})`（确定式，可编程）+ 一个
"重命名…"命令负责弹输入框、填好 `newName` 后调 `fs.rename`。人用的还是那个命令，
机器用新的确定式操作。**两者共用同一份实现**。

### 5.7 传输入口

一份契约，多个入口，全部走到 `execute`：

```
工具栏按钮 ─┐
快捷键     ─┤
命令面板   ─┼→ execute(op, args) → 前置检查 → 执行 → 审计
脚本/CLI   ─┤
IPC/AI     ─┘
```

这一层我们已经有（`CommandBus`），不需要改结构，只需要把参数和判定补齐。

---

## 6. 抄什么、不抄什么

| 做法 | 抄？ | 理由 |
|---|---|---|
| MCP 的字段设计（inputSchema/outputSchema/annotations） | **抄** | 它是当前最完整的"机器可调用操作"字段集，且不绑死传输层 |
| MCP 的**错误二分**（协议错误 vs 执行错误） | **抄** | 调用方必须能区分"我调错了"和"环境不满足" |
| MCP 的 JSON-RPC / 传输层 | 不抄（内部） | 内部调用不需要序列化；只在对外暴露时再包一层 |
| VS Code 的"注册与声明分离" | **抄** | 描述可导出即文档、可校验、可给机器读 |
| VS Code 的 `when` / `enablement` 二分 | **抄** | 可见性与可用性分开，我们已经混在一起 |
| Blender 的 `poll()` + disabled hint | **抄** | 这是"人看到灰按钮知道为什么"的唯一干净解法 |
| Blender 的 `exec` / `invoke` 二分 | **抄** | 解决 `CmdRename` 类命令无法编程调用的根因 |
| Blender 的 operator 只调高层函数 | **抄**（已符合） | 我们的 handler 已经是薄的 |
| Opus 的"命令唯一入口" | **抄**（已有） | 已实现 |
| Opus 的命令修饰符（`@confirm` 等） | **抄概念** | 变成调用参数 `{confirm:true}`，不是命令名变体 |
| Opus 的插件 ABI / 脚本引擎 | 不抄 | 引入的是另一个工程 |
| explorer 的 `shell:` 别名 | **抄** | 用别名替代散落的 CLSID |
| 一次性做完全部 127 条 | 不抄 | 见 §7 的渐进路线 |

---

## 7. 迁移顺序（只排顺序与判据，不含实现）

**原则：新契约先覆盖"能用增量方式接入"的部分；每条改动都必须可独立验证、可回退。**

| 步 | 做什么 | 判据（可验证） | 状态 |
|---|---|---|---|
| 0 | 定名与字段规范（本文 §5），写进 `docs`，不动代码 | 规范能被 `check_*.py` 机械校验 | ✅ |
| 1 | **参数对象化**：`execute` 收 JSON 对象；旧的单字符串入口保留为适配层 | 新旧两条路径对同一操作结果一致（可单测） | ✅ |
| 2 | **效果四维化**：把 `Risk` 拆成 4+2 个字段，逐条重标 127 条 | 无 `readOnly=false && scope=currentTab && 无目标参数` 的条目 | ✅ |
| 3 | **前置条件**：给"最常被机器调用"的 21 条加判定 | 每条有一条单测：不满足时返回原因而非崩溃 | ✅ |
| 4 | **exec/invoke 拆分**：新增 `FsRename`/`FsDelete`/`FsCopyTo`/`FsMoveTo` 四条确定式命令（目标由参数点名，绝不弹框、绝不读选中），与旧的交互式命令共用 `shell_ops` 同一份实现 | 同一操作两条入口都在；确定式命令有目标参数且不依赖选中 | ✅ |
| 5 | **返回值**：`CommandBus` 加一条独立的"带结果"注册通道（`RegisterWithResult` / `InvokeResult`），`CommandDef` 加 `outputSchema`；`OpListDir` / `OpStat` 成为第一批产出结构化结果的只读命令 | 有 outputSchema 的命令必须真的能返回 | `tests/result_test.cpp` 20 项通过 |
| 6 | **命名空间化**：`src/cmd_naming.{h,cpp}` 提供 131 条旧名 ↔ 点分名双向映射；`FindCommand` 两个方向都认 | 两种名都解析到同一操作 | `tests/naming_test.cpp` 31 项通过 |
| 7 | **对外暴露**：`ai_server` 加 JSON-RPC 2.0 / MCP 兼容层（`initialize` / `tools/list` / `tools/call`），旧简化格式保留；工具补 `description` 与 MCP 形状的 `inputSchema` | MCP 客户端能直接连上说话 | `tests/rpc_test.cpp` 21 项 + `tests/pipe_e2e_test.cpp` 35 项（真管道往返）通过 |

第 1 步是所有后续步骤的地基；第 2 步决定安全策略能否成立；第 3 步是"机器能安全调用"
的门槛。**1–7 全部完成。**

### 第 7 步的做法与取舍

**协议层用 JSON-RPC 2.0，与 MCP 对齐。** 上一轮的自定义格式（`{"token":..,"method":..}`）
对 MCP 客户端不可用 —— 每个客户端都得自己写转换层，那就失去了"开箱可连"的意义。
现在靠**有无 `jsonrpc` 字段**区分两种协议：MCP 客户端走标准路径，既有本地脚本走旧路径，
互不干扰。

**令牌放在 `params.__token`。** MCP 没有令牌概念，但本地进程间调用需要护栏
（否则同机任何程序都能驱动这个窗口）。约定成自定义字段，并保留顶层 `token` 兼容旧格式。

**错误必须分两类**（MCP 规范的硬要求）：
- 请求本身不合法（版本错、缺 method、令牌错）→ JSON-RPC `error`，带 `code`
- 工具执行失败（缺参数、未知工具、命令返回 false）→ `result` 里 `isError: true`

混在一起客户端就分不清"我调错了"和"环境不满足"，只能盲重试或盲放弃。

**令牌检查放在方法分派之前。** 否则调用方能靠"这个方法返回 method-not-found
还是 bad-token"来枚举有哪些方法 —— 这是能力面泄露。单测里专门断言
"未知方法与已知方法的未授权响应完全一致"。

**`description` 从结构化字段生成，不手写。** MCP 里模型靠它判断何时该用。
手写的描述会与 `annotations` 脱节（改了 `destructive` 忘了改描述），
两者不一致时模型会做错判断。现在描述是拼出来的，"说的"与"标的"永远一致。

### 第 6 步的做法与取舍

**为什么是"加别名"而不是"改名"？** 改名会打断已有的 keymap、序列、鼠标规则、
用户脚本里的引用。做法是加一层映射，`FindCommand` 两个方向都认 ——
旧路径零改动，新客户端可以开始用 `nav.back` 这类稳定名字。

**命名规则**：全小写、点分、两段（`域.动作`）。域取自 `CommandDef::group` 的小写
—— 不另造分类，否则两套"分组"迟早不一致。单测里有一条断言"每个点分名的域前缀
都必须与该命令的 group 对应"，这条在写测试时立刻抓出了我把 `AiTogglePanel`
错标成 `tool.*`（它是 `group=Window`）。

**为什么两段而不是三段？** 131 条命令按域分组后每组最多 28 条，两段足够区分。
再加一段会出现 `tab.close.other.left` 这种读起来吃力的名字。

**大小写不敏感是有意的。** 旧名查找走 `_wcsicmp`，如果点分名偏偏敏感，就会出现
"`NavBack` 和 `navback` 都能用，但 `NAV.BACK` 不行"这种半吊子行为。
`LegacyOf` 因此先转小写再查。

**确定式命令用 `fs.*` 而不是 `file.*`**：它们的语义是"对指定路径做文件系统操作"，
与"对当前选中做点什么"的交互式命令不是同一件事。两者同名域会让人以为只是参数不同。

**`tools/list` 现在返回规范名 + `legacyName`**：新客户端只用 `name`，
既有脚本/审计日志用 `legacyName` 对照，不需要两边都学。

### 第 5 步的做法与取舍

**为什么不让所有 handler 都返回 `json::Value`？**
131 条命令里绝大多数是"做了/没做成"。让它们都构造 JSON 是纯负担，还容易让
写操作假装有返回值。做法是**单独一条注册通道**：需要返回值的用
`RegisterWithResult` 注册，其余保持 `bool`。`CommandBus` 内部记两套表，
`HasResult()` 供 UI/文档判断要不要展示"结果"区域。

**为什么 `result` 是不带 schema 的 `json::Value` 而不是 C++ 结构体？**
定义 131 个 struct 是过度设计，而且会让返回值变成"只有 C++ 能读懂"的私有格式。
统一用 JSON 对象，能直接序列化成 MCP 的 `structuredContent`。

**无返回值的命令必须给空对象，不能留旧数据。** 调用方复用同一个 `json::Value`
时若不清空，会读到上一次的结果 —— 这比"没有结果"更危险，因为它看起来像真的。
单测里专门有一条断言这件事。

**`OpListDir` / `OpStat` 查的是"当前视图"而不是按路径扫盘。** 好处：与用户看到的
完全一致（含虚拟命名空间、搜索结果、过滤状态），且不需要重新判断访问权限。
代价是只能查当前目录 —— 但这恰好是命令面板/机器最需要的语义（"我现在看到的
这些是什么"）。

### 第 4 步的做法与取舍

**为什么不给旧命令加参数，而是新增一组命令？**
旧命令的语义里"弹框问用户"是不可分割的一部分 —— `F2` 重命名的体验就是"弹出输入框"。
加参数会变成"传了参数就不弹框"的隐式分叉，调用方很难预期。
拆成两条后：人和机器各走各的入口，逻辑共用。

**为什么确定式版本是"先选中再调既有实现"，而不是直接按路径调 `IFileOperation`？**
直接按路径做会产出第二份文件语义实现（回收站、冲突、权限、撤销都得重写一遍），
迟早与既有实现分叉 —— 这正是架构规则 R6「单一实现」要防的。
先选中再复用则只有一份实现，代价是会改变用户选中状态（可接受：命令本身有副作用，
且调用方既然点名了目标，选中集随之变化符合直觉）。

**新增的 `shell_ops` 能力**（`SelectByPath` / `SelectNone` / `SelectedSinglePath` /
`RenameTo`）：`RenameTo` 是唯一需要新写文件语义的一个，因为旧路径只有
`InvokeSelectionVerb("rename")`（弹框）。它用 `IFileOperation` + `FOF_SILENT`
+ `FOF_NOERRORUI` —— 确定式调用不该卡在没人看的对话框上；冲突时**返回失败**
而不是弹框，因为明确的失败比弹窗更可预期。

### 债务消化进度

`check_ai_meta.py` 第 9 项追踪此事：

| 旧命令（只能作用于选中） | 确定式版本 | 状态 |
|---|---|---|
| `CmdRename` | `FsRename` | ✅ 已覆盖 |
| `CmdDelete` | `FsDelete` | ✅ 已覆盖 |
| `CopyToFolder` | `FsCopyTo` | ✅ 已覆盖 |
| `MoveToFolder` | `FsMoveTo` | ✅ 已覆盖 |
| `CmdCut` | — | ⬜ 待做（剪切语义特殊：改的是剪贴板，不是文件） |
| `BatchRename` | — | ⬜ 待做（等 PowerRename 集成） |
| `PathCopyLink` | — | ⬜ 待做（写磁盘，低优先级） |

---

## 8. 当时的待决策（现已全部落定）

| # | 问题 | 结论 |
|---|---|---|
| 1 | 旧 `arg` 字符串是否永久保留？ | **保留为适配层**（`InvokeLegacy` / `ArgsFromLegacyArg`），键名从 schema 推导，不再需要手工映射表。见第 1 步。 |
| 2 | `scope=currentTab` 是否强制 `tabId`？ | **未做**。仍然只作用于当前标签。这是真实的限制：机器无法指定"对另一个标签做"。（可与 §4.3 的 `targets` 一起考虑） |
| 3 | 前置条件放描述还是 handler？ | **声明式枚举**（`Requirements` + `CanExecute`）。可被静态检查与 UI 直接读。见第 3 步。 |
| 4 | `Op*` 那 7 条是否按新契约实现？ | **做了 2 条**（`OpListDir` / `OpStat`）验证返回值设计，其余按需。见第 5 步。 |
| 5 | 命名空间前缀？ | **与 `group` 对齐**（`tab.` / `nav.` / `file.` / `fs.` / `view.` / `win.` / `ext.` / `bmk.` / `session.` / `tool.` / `search.` / `go.` / `path.`），不重划。见第 6 步。 |

---

## 9. 内容补全（第 8 步：从"有能力"到"每条都能用"）

七步做完了**机制**，但机制不等于内容。第 8 步逐条判断每条命令该不该暴露给机器。

### 结果

| 指标 | 之前 | 之后 |
|---|---|---|
| AI 可调用（已实现且暴露） | 45 / 100 | **78 / 100** |
| `tools/list` 工具数 | 47 | **78** |

### 暴露的判据（三条，缺一不可）

1. **已实现** —— 否则会出现在 `tools/list` 里但调不动
2. **不依赖当前选中** —— 机器无法指定"选哪个"，暴露出去就是"AI 以为删了 A，实际删了 B"
3. **不是纯人机 UI** —— 弹菜单、开关面板、退出程序，机器调了没有意义

这三条已写成 `check_ai_meta.py` 第 14 项，任何违反都会报错。

### 22 条仍不暴露，每条有理由

| 理由 | 命令 |
|---|---|
| **依赖选中**（17 条） | `PathCopy*`(4)、`CmdOpenWith`、`CmdCopy`、`CmdCut`、`CmdDelete`、`CmdRename`、`CmdProps`、`CmdOpen`、`CmdOpenInNewTab`、`CmdOpenInNewWindow`、`CopyToFolder`、`MoveToFolder` |
| **纯人机 UI**（6 条… 实为 5 条 + NavViewMenu） | `NavViewMenu`（弹菜单）、`QuitExplorer`、`FolderOptions`、`MapDrive`、`UnmapDrive`、`CommandPalette`、`HotkeyManager` |

依赖选中的那 17 条**不是遗漏，是设计**：它们需要"用户当前选了什么"这个信息，
而机器没有。第 4 步已为其中 4 条提供确定式版本（`FsRename`/`FsDelete`/`FsCopyTo`/`FsMoveTo`），
机器应该用新入口而不是旧命令。剩余 `CmdOpen`/`CmdOpenInNewTab` 等**尚无确定式版本**，
是真实缺口（见 §4.3 债务表）。

### 这次补全暴露的一个设计问题

`PathCopyDir` / `PathCopyTab` 早先和 `PathCopy` 一起标了 `needsSelection=true`，
但它们的实现用的是 `ActivePath()` / `tabSet_` —— **根本不看选中**。
标注与实际行为不符，导致这两条被错误地排除在机器能力之外。

已改正。这提醒：`needsSelection` 是**人写的标注**，必须对照实现核实，
不能因为"名字像"就照抄。

---

### 9.2 补齐能力缺口（第 9 步）

按三项优先级依次推进，每项都先看成熟项目怎么做再动手。

#### (1) `FsOpen`：机器也能"打开指定文件"

`CmdOpen` 依赖选中，机器无法表达"打开第 3 个文件"。新增 `FsOpen {path}`，
语义与 `CmdOpen` 一致：文件夹在当前标签进入，文件交 shell 的默认程序。

**为什么不加 `mode` 参数**：VS Code 把"打开"拆成 `open` / `openWith` /
`revealInExplorer` 三条独立命令，而不是一条带 `mode` 的。理由是它们代表三种不同
意图，各值得一个名字；塞进枚举会让调用方先查"有哪些 mode 可用"。

**一个刻意的行为**：路径不存在时直接返回失败，不交给 `ShellExecute`（它会弹
"找不到文件"的系统框）。确定式调用不该卡在没人看的对话框上。

#### (2) 前置条件 16% → 30%

参照 VS Code 的 `when` 子句 + `enablement`：把"边界处会静默失败"的条件声明出来，
UI 据此灰掉按钮、审计留下可读原因。

新增 4 个条件（都来自实际实现里的静默防御）：

| 条件 | 为什么需要 |
|---|---|
| `CanMoveTabLeft` / `CanMoveTabRight` | `TabMoveLeft` 的实现有 `toIndex<0 → 0` 钳制，已在最左时"按了没反应" |
| `HasClosedTabs` | `TabReopenClosed` 无历史时 `if (PopClosed(...))` 静默跳过 |
| `HasMultipleTabs` | 单标签时"上一个/下一个/关左侧"无意义 |
| `HasSelectionSingle` | `CmdProps`/`CmdOpenWith` 只接受单个，多选时应灰掉 |

加了 18 条命令的条件，其中 `GoTab1..9` 补 `HasTabs`、`CloseLeftOne`/`CloseRightOne`
补 `CanMoveTab*`、`CmdRename` 补 `HasSelection`（原先完全没声明，无选中时会弹框）。

#### (3) `OpTopN` / `OpDiskUsage`：目录体积统计

新增 `src/disk_scan.{h,cpp}`，三条硬约束（都来自会踩的坑）：

1. **必须能中途停** —— 给 deadline，超时带已有结果返回。几万个文件的递归要几十秒，
   卡在 UI 线程就是假死。
2. **必须报告完整性** —— 结果里带 `truncated` / `timedOut` / `elapsedMs` /
   `scannedFiles` / `scannedDirs`。只给数字而不说"没算完"，调用方会把不完整值当真。
3. **不跟随重解析点** —— 跟 junction 会无限递归，且把链接目标的大小算到链接处，数字是错的。

**为什么是两条命令而不是一条带 mode**：`OpTopN` 答的是"什么最占空间"（看 `entries`），
`OpDiskUsage` 答的是"这目录占多少"（看 `totalBytes`）。Unix 上 `du`（总量）与
`ncdu`（占比排序）也是两个工具。

**一个容易忽略的细节**：`totalBytes` 统计的是**截断前**的全部，否则
"总量 < 各子项之和"会让人困惑。

#### 指标变化

| 指标 | 第 8 步 | 第 9 步 |
|---|---|---|
| 命令总数 | 131 | **132** |
| 已实现 | 100 | **103** |
| 有前置条件 | 21（16%） | **40（30%）** |
| 有 `outputSchema` | 2 | **4** |
| AI 可调用 | 78/100 | **81/103** |
| 测试断言 | 248 | **257** |

---

### 9.3 偿清"只能靠选中"的债务（第 10 步）

债务表原本只登记了 4 条，实际"依赖选中且已实现"的有 15 条 —— 大部分从没被登记。
补齐后逐条做确定式版本。

#### 新增 5 条确定式命令

| 新命令 | 对应 | 关键决定 |
|---|---|---|
| `FsOpenInNewTab {path}` | `CmdOpenInNewTab` | 复用 `App::NewTab`；目标不存在直接失败 |
| `FsOpenInNewWindow {path}` | `CmdOpenInNewWindow` | 复用 `OpenPathInNewWindow` |
| `FsCopy {targets[]}` | `CmdCopy` | 写剪贴板，**不产生副本** |
| `FsCut {targets[]}` | `CmdCut` | 写剪贴板 + 标 `DROPEFFECT_MOVE` |
| `FsProperties {target}` | `CmdProps` | 唯一**故意弹 UI** 的确定式命令 |

#### 三个值得记的技术点

**1. `FsCopy` 的语义界限要写清楚。** 它和 `Ctrl+C` 完全同义 —— 只是"准备好，等粘贴"，
**不产生副本**。想一步到位复制到某目录要用 `FsCopyTo`。这两件事很容易被误解成
同一件，所以在命令注释、文档、别名（`fs.copy.toclip` vs `fs.copy`）三处都标了。

**2. 造 `IDataObject` 用 `CIDLData_CreateFromIDArray` 而不是 `SHCreateDataObject`。**
后者被 `NTDDI_VERSION >= 0x06000000` 包着，某些配置下不可见；前者无条件可用，
参数也正好是"父目录 + 相对子 PIDL 数组"。两者都比自己拼 `CF_HDROP` 好 ——
后者只表达文件名列表，丢掉"来自哪个命名空间"（回收站项、库项），粘贴到某些目标会失败。

**3. 一个路径解析不了就整批失败。** 漏掉一个会造成"我复制了 5 个，实际只复制了 3 个"，
而调用方以为全成了。宁可整批失败也不静默少做。

#### 又一次撞上"静态检查看不见间接取用"

给这 5 条写 handler 时，我先把公共逻辑抽成了 `openTarget` / `copyPaths` 两个 lambda。
结果 `check_ai_meta.py` 第 3 项立刻报"声明了参数但 handler 没取用"——因为参数在
**被调用的 lambda 里**读，不在 `reg()` 的 lambda 体里。

第 9 步（`OpTopN`）已经踩过同一个坑，当时的处理是登记进 `INDIRECT_ARGS` 例外表。
这次我改成**不抽公共函数**，三个 handler 各写三行重复代码。

理由：例外表越长，检查能挡住的问题越少。重复三行换检查覆盖，值得。
这条写进注释了，免得后来人"优化"回去。

#### 顺带修掉一个检查器的 bug

`COVERS` 的遍历外面套了 `if old in KNOWN_NO_TARGET`，导致 `CmdRename`/`CmdDelete`
明明在覆盖表里却从不被打印 —— **覆盖情况看起来比实际更差**。已去掉那层筛选。

#### 指标变化

| 指标 | 第 9 步 | 第 10 步 |
|---|---|---|
| 命令总数 | 132 | **137** |
| 已实现 | 103 | **108** |
| AI 可调用 | 81 | **86** |
| 债务表未覆盖 | 3 条 | **2 条**（`BatchRename`/`PathCopyLink`） |
| 依赖选中且已实现 | 15 条 | **10 条**（`PathCopy*` 4 条 + `CmdDelete`/`CmdRename`/`CopyToFolder`/`MoveToFolder`/`CmdOpenWith` 6 条已由 `Fs*` 覆盖） |

---

## 10. 事后审查（2026-11-20，第 7 步完成后）

七步做完后逐处复查，找到 5 个真实缺陷。都不是"测试没通过"，而是测试覆盖不到的地方：

| # | 缺陷 | 后果 | 修法 |
|---|---|---|---|
| 1 | `SelectNone` / `SelectedSinglePath` 混用两个索引空间：`ItemCount(SVGIO_SELECTION)` 给的是"选中个数"，`Item(i)` 的 `i` 是"视图全局索引" | 若第 5、9 项被选中，会去取消第 0、1 项 —— **取消错的项** | 改用 `Items(SVGIO_SELECTION, IID_IShellItemArray)` |
| 2 | `RenameTo` 先 `SelectByPath` 再改名，但 `IFileOperation::RenameItem` 根本不需要选中 | 白白改动用户选中；失败时选中已被改，用户莫名发现选中变了 | 改成只读地确认目标在视图内，不动选中 |
| 3 | `ValidateAgainstSchema` 完全不校验 `type:"array"` | `FsDelete` 传非数组会一路走到 handler 变成"命令执行失败"，错误信息指不到真正原因 | 补数组校验 + 逐项 `items` 类型校验 |
| 4 | `unsigned long long` 的文件大小被强转 `int` 后存 JSON | **超过 2 GB 的文件显示成负数**（3 GB → -1073741824），且 JSON 里看起来"就是个数字"，客户端不会察觉 | 加 `json::Value::MakeNum(double)`，改用浮点存（2^53 内精确） |
| 5 | 未注册 state probe 时，前置条件返回 `HasTabs` 的原因 ——"没有打开的标签" | 误导：标签可能好端端开着，真实原因是状态未就绪 | 加 `Req::StateUnavailable`，文案"界面尚未就绪" |

顺带修掉两处"会埋雷但暂时没炸"的：

- `FsDelete` / `FsCopyTo` / `FsMoveTo` 早先是循环里"选一个做一次"，N 次 IFileOperation + N 次刷新，中途失败留下半完成状态。改为 `SelectByPaths` 一次选中全部、一次操作。
- `Register` 与 `RegisterWithResult` 撞车时（同一命令两边都注册），带结果的静默胜出、另一个被忽略。现在注册时打警告日志，避免变成"我注册了 handler 却没跑"。

**回归测试**：新增 `tests/schema_array_test.cpp`（11 项）与 `tests/json_bignum_test.cpp`（8 项）
覆盖第 3、4 项；`tests/precondition_test.cpp` 加一条断言覆盖第 5 项。

**未修 / 已知限制**（如实记录，不假装完成）：

- 第 1、2 项修好后，`SelectNone` / `SelectedSinglePath` / `RenameTo` 的"选中有没有真的成功"
  仍只有人工冒烟能验 —— 单测跑不了窗口。
- `RenameTo` 的冲突检测（`tf::Exists` 与改名之间）有理论竞态，单用户本地场景可接受。
- `SFGAO_FOLDER` 没区分 `SFGAO_STREAM`：压缩包、库这类"既是容器又是流"的项会报
  `isFolder=true`。"能不能进去"与"是不是容器"是两件事，当前没区分。
- `ArgsFromLegacyArg` 每次按键都重新解析一次 JSON schema（实测 3.5 µs/次）。
  可缓存，但按键场景下不值得增加复杂度。

---

## 11. 来源

- MCP Tools（2025-06-18）：https://modelcontextprotocol.io/specification/2025-06-18/server/tools
- MCP Tools（2025-11-25，含 tool name 规则与 inputSchema 空参数写法）：https://modelcontextprotocol.io/specification/2025-11-25/server/tools
- VS Code Commands Guide（registerCommand / executeCommand / when / enablement）：
  https://code.visualstudio.com/api/extension-guides/command
- Blender Operator（poll / exec / invoke / 返回值 / undo / 上下文覆盖）：
  https://docs.blender.org/api/current/bpy.types.Operator.html
- Blender 开发者文档：Operators（operator 应只调高层函数、context 扩散问题）：
  https://projects.blender.org/blender/blender-developer-docs/src/branch/main/docs/features/interface/operators.md
- Directory Opus 脚本对象模型与 DOpusRT：https://docs.dopus.com/doku.php?id=reference:dopusrt_reference
- Windows Explorer 命令行（`/select,` / `shell:` 别名）：
  https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/explorer
