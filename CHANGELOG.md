## [Unreleased]

### 变更（"被拦住"不再一锅端：`blocked` 拆成四类 + 一类超时）

以前"被拦住"只有一个 `blocked`。但**被拦住有四种，下一步完全不同**：

| 类别 | 含义 | 调用方该做什么 |
|---|---|---|
| `precondition_unmet` | 前置条件不满足（没选中、目标不在视图、参数缺失） | 换目标 / 先做前置动作，**重试有用** |
| `lock_conflict` | 执行期被占用/被锁（剪贴板、文件被别的程序占着） | 关掉占用方后重试，或换一个目标 |
| `canceled_by_user` | 用户主动取消（拒绝批准、在进度窗口点了取消） | **不要重试** —— 停下来问用户 |
| `not_supported` | 该环境/该对象不支持这件事（视图模式不对、能力不存在） | 换一条路，**重试永远没用** |
| `timed_out` | 等待时限到了，后台作业**仍在跑** | 用 `tool.job.get` 继续查那个 jobId，别重发命令 |

一锅端成 `blocked` 等于让调用方去猜，而它猜错的方式通常是
"一直重试到超时"或"直接放弃"—— 两种都是错的，而且都会变成静默的行为缺陷。

- **新增 `FailKind` 枚举**（`src/commands.h`）—— 类别由**命令调用点**声明：
  `CommandBus::NoteFailure(reason, kind)`。这是关键：`ai_api` 不去匹配中文字符串
  （文案一改就断，那是文案该干的事），"剪贴板被占"在写它的地方就知道它是锁冲突。
  `FailKindCode` 是唯一的枚举 → errorCode 映射点。
- **默认值是 `precondition_unmet`** —— 绝大多数失败确实是"前提没满足"
  （没选中、目标不存在、参数缺失、id 不存在…）。不确定时**不要**硬凑一个类别：
  留默认比猜一个错类别好。
- **`CommandBus` 新增 `lastFailKind()`** —— 与 `lastBlockReason()` 同源：
  UI、AI、审计看到的是同一件事。
- **顺带修掉一个真缺陷**：作业被取消时 `job::Status` 是
  `done=true, cancelled=true, failed=false`，而 `CollectResult` 只判 `failed` ——
  于是被取消的作业会走**成功**分支，把空结果当成"跑完了"报出去
  （"查重没查到重复"其实可能是"用户中途叫停了"）。现在单独判 `cancelled`，
  报 `canceled_by_user`，并在结果里带上 `cancelled: true`。
- **`tools/agentctl.py` 的 `gap` 诊断**同步细分：四类给出四条**不同的**处置建议
  （混成一条，agent 就会把"稍后重试"用在"能力不存在"上）。
- **静态判据**（`tools/check_ai_meta.py` 第 22 项）：白名单加上五个新码，
  并加判据「`blocked` 不许退化回一锅端」—— 码表是从 `FailKindCode` 反查的，
  不是从 `ai_api.cpp` 扫的（后者看不到这几类码）。
- **新增 `tests/fail_kind_test.cpp`**：映射唯一性、类别透传、默认类别、
  前置不满足的归类，以及"被取消的作业不算成功"。
- **兼容性**：旧版本程序仍可能返回一锅端的 `blocked`，客户端按
  `precondition_unmet` 处理（`agentctl.py` 里保留了这条映射）。

### 新增（M3：五条长任务命令作业化 —— 工作线程 + 进度 + 取消 + 异步）

`tool.dupfinder` / `tool.hash` / `tool.compare` / `tool.diskusage` / `tool.topn`
过去**同步跑在 UI 线程**上。一个几万文件的目录能把界面冻住十几秒，
用户连"取消"都按不了 —— UI 线程被占着，连定时器都不转。
`budgetMs` 只保证"不会假死到没完"，不解决"中途不能停"，也不解决"界面不冻结"。

按 `docs/ROADMAP.md` §4.5 的判断，正确形态是**把这五条挪到工作线程 + 进度窗口**，
而不是再造一个 helper 进程（helper 进程要把结果整份搬回主进程，
而这里要的就是"边跑边看进度、随时能停"）。

- **新增 `src/job.{h,cpp}`** —— 进程级作业服务：一条命令对应一个作业，
  作业在**工作线程**上跑，产出结构化结果，并给出**进度**与**取消**。
  三条硬约束：工作线程产出 / UI 线程取走（共享记录 + 互斥锁，调用方永远拿到
  **快照**，不会看到写了一半的中间态）；取消是**协作式**的（置原子位，
  数据层在循环里查 —— 已经被系统调用卡住的那一步中断不了，这是诚实的边界）；
  完成与进度都可订阅（钩子 → 事件推送，客户端不必轮询）。
- **新增 `src/scan_ctl.h`** —— 数据层与命令层之间的控制通道
  （取消标志 + 进度回调）。取消与进度这两件事在三个数据层都要用，
  各自加一份字段就是三份实现，而三份实现迟早分叉。
- **新增 `src/job_ui.{h,cpp}`** —— 进度窗口（自绘进度条 + 「取消」按钮）。
  人工路径弹窗（模态，owner 被禁用），机器路径用 `HWND_MESSAGE` 静默等待 ——
  没人盯着屏幕，却把整个界面锁住，比冻结好不到哪去。
  **人侧不再"等超时就把窗口关掉"**：作业还在后台跑却把窗口关了，
  用户会以为"停了"，于是既看不到结果也不敢做别的事。
  现在人侧等到作业自己结束或用户按「取消」为止；机器路径保留时限。
- **五条长任务命令全部走作业**：准备参数在 UI 线程（`ActiveView()` 是 COM 调用，
  只能在 UI 线程），重活在工作线程，收尾（弹报告）在 UI 线程。
- **新增 `async` 参数**（五条都有）：`true` = 立刻返回 `{jobId, state:"running"}`。
  这是**必须的**：AI 管道的监听线程只有 10 秒超时，同步扫一个大目录必然超时，
  而超时之后命令仍在跑 —— 调用方拿不到结果，也没法叫停。
  异步把"跑多久"与"等多久"拆开。
- **新增三条作业命令**：`tool.job.get`（查状态与结果）、`tool.job.cancel`
  （请求取消）、`tool.job.list`（列出全部）。
  作业是**进程级**对象，所以 scope 是 App —— 写成 CurrentTab 会骗调用方。
- **新增事件 `tool.job.progress` / `tool.job.done`**：完成事件**带结果**，
  否则订阅者收到"做完了"还要再调一次才拿到东西，那等于把轮询换成了两段式轮询。
- **结果新增 `cancelled` 字段**：与 `timedOut` **分开**。超时是"没那么多时间"
  （可以加大预算重试），取消是"我不要了"（重试只是白费力气）。
  把两者混成一个字段，调用方就没法决定下一步该做什么。
- **时间预算的语义统一了**：`budgetMs` 不传 = **不限时**（异步与人工路径的默认）；
  机器同步调用仍默认 1500ms（管道 10 秒就超时，等太久没有意义）。
  三层数据层对"0"的原有解释并不一致（摘要那层 0 = 不限，体积统计那层 0 = 立刻超时，
  且 `tests/disk_scan_test.cpp` 钉着这个行为），所以"不限时"走**新增的 `noTimeLimit` 标志**，
  由命令层统一翻译 —— 不动任何历史语义。
- **顺带补上人侧的空白**：`tool.topn` / `tool.diskusage` 过去**没有任何人可见的输出**
  （命令面板只调 `Invoke` 不调 `InvokeResult`，返回值被丢弃）——
  人在界面上点了"最大的 N 项"，什么都没发生。现在跑完给一份可读报告。
- **命令层按职责拆开**：`app_commands_files.cpp`（1356 → 759）里那五条长任务命令
  搬去了 `app_commands_scan.cpp`（体积统计 + 摘要）与 `app_commands_compare.cpp`
  （查重 + 比较），作业域的命令与事件接线在 `app_commands_jobs.cpp`。
- **新增 `tests/job_test.cpp`**：钉住作业服务的关键契约 ——
  正常路径 / 失败路径 / 取消（取消**不是**失败）/ 进度快照 /
  取消已结束的作业会失败 / 等待超时不装作等到 / `Shutdown` 取消全部并 join。
  退出时不 join 的话，工作线程会往已析构的 Service 上写 ——
  崩在关窗口那一刻，而且崩得莫名其妙（用户只看到"关窗口时闪退"）。

### 变更（按职责拆开两个超长文件：行数棘轮第一次真的收紧了）

上一笔提交引入了"单文件 ≤500 行"的棘轮，但**基线是在编辑中途量的**，
记的行数比真正提交进去的少（如 `shell_ops.cpp` 记 1384、实际提交 1502）。
于是棘轮在自己诞生的那笔提交上就报错 —— 判据从第一天起就是假的。

本轮先把两处按职责切开，再把账改正：

- **`shell_ops.cpp`（1502 → 1338）** 拆成三份：
  - `shell_ops.cpp` —— **视图作用域**的 shell 操作（选中/排序/分组/隐藏/新建…）；
  - `shell_ops_path.{h,cpp}`（新）—— **路径作用域**的文件操作（撤销栈专用：
    `RenamePath` / `MovePathTo` / `CopyPathTo` / `DeletePath`）；
  - `shell_op_exec.{h,cpp}`（新）—— `IFileOperation` 的**唯一**执行器
    （`Run` + `HexHr`）。
  两种作用域混在一个文件里时，读的人要一路翻到文件末尾才发现还有另一套语义；
  而"执行 + 判成败"这套判据（必须同时看 `PerformOperations` 的 HRESULT **和**
  `GetAnyOperationsAborted`）抄一半就会漏 —— 同一件事在删除路径上查 aborted、
  在改名路径上不查。
- **`app_commands_files.cpp`（1356 → 1140）** 拆出 **`app_commands_fs.{h,cpp}`**：
  `fs.*`（按路径点名目标）与 `file.*`（作用于用户碰巧选中的项）**入参语义就不同**，
  混在一起时读的人要翻到中段才知道"这些命令的入参根本不是选中集"。
- **`pathutil::ToShellPaths()`**：批量版归一化。`fs.*` 的 `targets` 本来就是数组，
  批量版让"整批一起归一化"成为**一次**调用 —— 逐项在调用点写循环，
  迟早有人漏掉其中一项（漏掉的那一项就是那个报 `E_INVALIDARG` 的）。
- **`data/file-length-baseline.json` 改正**：记账值改为**提交态的真实行数**。
  棘轮本身没有被绕过（已验证：压低记账值仍会报错；普通检查与构建脚本都不改写基线），
  错的只是量表。
- **`mcp_stdio.cpp` 进"长路径前缀"白名单**：它开的是命名管道
  （`\\.\pipe\...`），不是文件系统路径，加 `\\?\` 前缀会彻底打不开 ——
  与 `ai_server.cpp` 同理。

两个最大文件都降到了**记账值以下**，棘轮第一次真的收紧了（`shell_ops.cpp` 1384→1338、
`app_commands_files.cpp` 1240→1140）。

### 修复（P0：AI 请求队列的写已释放栈）

监听线程把请求交给 UI 线程执行并同步等结果，**10 秒**超时。请求对象原先是
`RunOnUiThread` 的**栈上局部变量**，队列里存裸指针。于是这条路径成立：

1. UI 线程已把请求取走，开始执行（handler 跑得久，比如一次大目录的磁盘扫描）；
2. 监听线程等到 10 秒超时 → 返回 → **栈帧销毁**；
3. UI 线程这时才处理完 → 往 `job->response` 写结果
   → **写已释放的栈**（踩的是监听线程 `ServeLoop` 的栈帧，未定义行为：
   可能当场崩，也可能悄悄改掉别人的数据）。

触发条件很现实：任何跑得比等待时限长的命令都会走到（大目录扫描、大树复制）。

- **请求对象改成共享所有权**（`shared_ptr`）：提交方、队列、UI 线程各持一份引用，
  谁先放手都不影响对象寿命。超时放弃的只是"等结果"这件事，
  UI 线程仍然会把那个请求安全地处理完。
- **抽成独立模块 `src/ai_queue.{h,cpp}`**：这段线程契约（谁拥有请求、提交方能等
  多久、超时之后谁还在用它）是最容易写错的地方，值得单独一个文件、单独单测。
  `ai_server.cpp` 顺带瘦了 30 行。
- **`tests/ai_queue_test.cpp`**：钉住超时路径 —— 提交方超时走人之后，
  UI 线程仍能安全处理完那个请求，队列之后仍然可用。
- 附赠可观测性：`Queue::Abandoned()` 统计"客户端超时之后才处理完"的请求数，
  写进日志 —— 这就是"界面卡了多久"的账。

### 修复（P0：超时响应用错了协议格式）

超时响应过去不论请求是什么格式，一律回 `{"ok":false,"error":"timeout"}`。
对 JSON-RPC / MCP 客户端来说这是个**非法响应**：没有 `id`，客户端无法把它
认成"我那条请求的回复" —— 要么当乱码丢掉，要么当服务端推送，
然后**永远等一个不会来的结果**（挂死）。

MCP stdio 桥接更是直接栽在这里：它靠"有 id、无 method"判响应，
旧格式的超时响应两条都不满足 → 被当成推送转发出去，客户端那头就一直挂着。

- 现在按**请求的格式**给：JSON-RPC 请求回
  `{"jsonrpc":"2.0","id":<id>,"error":{"code":-32000,"message":"请求超时…","errorCode":"timeout"}}`；
  旧格式请求仍旧回 `{"ok":false,"error":"timeout"}`。
- `code` 用 -32000（JSON-RPC 留给实现自定义的区间），语义是"服务端没能在时限内
  处理完"，不是"请求不合法"（那是 -32600）。
- `errorCode` 一并带上：项目口径是 `errorCode` 才是给机器分支的稳定标识。

### 修复（P0：模态框开着的时候仍在派发 AI 命令）

模态框有自己的消息循环，而那个循环里**照样收得到 `WM_TIMER`** ——
于是 AI 通道的泵定时器会在模态框开着的时候照常触发，命令 handler
**重入**地跑在模态框的循环里。那不是"慢一点"，是能出事的：

- handler 可能再开一个模态框（框套框，用户看到两个）；
- handler 可能改选中集/视图，而那个模态框正在等的就是这份状态
  （"重命名"对话框正开着，AI 把它正在改的那一项换掉了）。

- **`modal::Depth()` / `modal::Scope`**：模态层数计数（文件内唯一一份，
  自写模态循环的地方用 `Scope` 包住，别自己维护计数）。
- **派发点避让**：`app_wnd.cpp` 的定时器分支在 `modal::Depth() > 0` 时**不派发**
  AI 请求 —— 请求留在队列里等模态框关掉，客户端那边照旧按 10 秒时限拿超时
  （语义不变，只是不再有重入）。
- **`tests/modal_depth_test.cpp`**：钉住计数必须配平（少减一次 → AI 泵被永久
  关掉；多加一次 → 防护形同虚设。两种都不报错，只是功能不对）。

### 修复（P1：`tool.hash` 没有时间预算，大文件能把界面按死）

`filehash::HashFile` 一直有 `cancel` 参数，但**命令层从来没传过**，
也没有时间预算 —— 于是 `tool.hash` 一个文件就是算到底。
这条命令跑在 **UI 线程**上（AI 请求由定时器泵派发，人按快捷键也是 UI 线程）：
一个几十 MB 的文件要十几秒，那段时间整个界面没有响应，
用户**连取消都按不了**（UI 线程被占着，定时器都不转）。

- `filehash::HashFile` 新增 `budgetMs`（0 = 不限），每 64 KB 检查一次，
  超了就停并置 `Result::timedOut`。
- `tool.hash` 新增 `budgetMs` 参数（默认 1500，与 diskscan / filetools 一致，
  夹在 100–30000）。**预算按整次调用计，不是每个文件一份** ——
  否则 N 个文件就是 N 倍预算，界面照样能卡很久。
- 结果里新增 `timedOut` / `complete`，**如实说"没算完"**：
  只给一串哈希而不说算没算完，调用方会把"预算用完"当成"这些文件就是这些摘要"。
- 预算耗尽时**绝不给半截哈希**（`hex` 留空），只报"未计算"。

### 修复（外部路径没归一化：`fs.*` 收到正斜杠路径就失败）

`SHCreateItemFromParsingName` **只认反斜杠**，收到 `C:/dir/file.txt` 直接返回
`E_INVALIDARG(0x80070057)`。而正斜杠在 Windows 里是完全合法的路径写法
（`CreateFile` 也接受），AI 也天然会这么写 —— 于是同一类输入：
`fs.open "C:/x"` 能导航（那一层有归一化），`fs.rename "C:/x/a.txt"` 却失败。

实测症状：`fs.rename` 报"重命名失败（名称非法或目标被占用）"，
而真因（分隔符）在错误信息里完全看不出来 —— **报一个错的错比报错更糟**。

- **新增 `pathutil::ToShellPath()`**（`src/path_util.{h,cpp}`）：全项目
  "外部路径 → shell API"的**唯一收口**（正斜杠→反斜杠、折叠连续分隔符、
  去尾分隔符；`::` / `shell:` 命名空间原样）。
- `shell_view.cpp` 里那份手抄的归一化删掉，改调同一份。
- `shell_ops.cpp` 的路径入口（`RenameTo` / `CopyPaths` / `RenamePath` /
  `MovePathTo` / `CopyPathTo` / `DeletePath`）全部归一化；
  `fs.*` 的 10 条命令在命令层也归一化（这样撤销记录与回执里存的是规范形式）。
- `tests/path_util_test.cpp` 补 `ToShellPath` 的用例（正斜杠、连续分隔符、
  盘根、UNC 前缀、命名空间路径）。

### 修复（失败原因细分：重命名不再"猜"错因）

`RenameTo` 以前只返回 `bool`，失败原因**只存在于 shell 的 HRESULT 里**，
于是所有失败都被压成同一句"重命名失败（名称非法或目标被占用）"——
这句话对每种原因都不算错，对使用者却毫无用处（该换名？该关掉占用它的程序？
还是权限问题？）。

- `shellops::RenameTo` 增加可选的 `std::wstring* why`，区分五种原因：
  目标不在当前视图 / 目标已存在 / 无法定位源文件（带 HRESULT）/ 重命名请求被拒 /
  文件操作被中止。
- `file.rename.batch` 的 `failed[]` 从"字符串数组"改成"对象数组"
  （`{from, reason}`），`outputSchema` 同步更新 —— 失败项必须带原因，
  只报文件名对使用者没用。
- `RunOp` 拆出 `RunOpEx`：把 `PerformOperations` 的 HRESULT 与
  `GetAnyOperationsAborted` 一起报出来（后者为 TRUE 时 S_OK 也是失败）。

### 修复（撤销失败不再"改撤别的操作"）

`file.undo` 在**应用内撤销栈非空但撤销失败**时会落回 shell 的撤销栈 ——
于是"撤 A 失败"会变成"居然撤了 B"。用户以为退回了 A，实际丢的是 B。

现在：栈非空但撤不动时**到此为止**（两种情形都必须停）：
① 栈顶是人工操作而调用方是机器（让机器撤人工的操作 = 穿透人工操作）；
② 撤销执行本身失败（记录还在栈里，可重试）。
只有"我们的栈本来就是空的"才轮到 shell 的撤销栈。

### 新增（文件操作撤销栈：`file.undo` 真能撤）

`file.undo` 以前只有一条兜底路径：去 shell 视图背景菜单里找 undo 谓词。
那条路对"右键菜单做的操作"有效，对 `fs.*` 这类**按路径点名**的操作无效
（菜单里根本没有对应项）。

- **`src/file_undo.{h,cpp}`**：逆操作式撤销栈（LIFO，上限 20，同 `bmk.*` 口径），
  纯策略、无 COM，可单测。
- 记栈点：`fs.rename` / `file.rename.batch`（批量=一条）/ 交互式 `file.rename`
  （按目录清单差集推新旧名）/ `fs.copy` / `fs.move` / `file.copy.to` / `file.move.to`。
- 撤销用**按路径**的那组 shell 操作（`src/shell_ops.{h,cpp}` 新增
  `RenamePath` / `MovePathTo` / `CopyPathTo` / `DeletePath`）——
  撤销时目标已经不在当前视图里，`Selection` 语义与 `RenameTo` 的
  "必须在视图里"校验都用不了。
- 两条边界规则：机器**不能**撤销人工的操作；撤销失败**不弹栈**
  （部分失败时把已撤成功的项剔掉，让重试只试剩下的）。
- `tests/file_undo_test.cpp` 覆盖：空栈 / LIFO / 上限 / 机器不穿透人工操作 /
  失败不弹栈 / 部分失败 prune / 空记录不入栈。

### 新增（`dev.snapshot` 真落盘 + `dev.rollback` 可逆）

`dev.rollback` 以前直接 `git checkout -- <path>`：**不可逆**（改到哪算哪，
回滚掉的东西没有副本），而文档指引的 `dev.snapshot` **不保存内容**
（只记路径 + 大小 + mtime），照着做等于"先扔掉备份再撤销"。

- **`src/dev_snapshot.{h,cpp}`**：快照把改动文件的**内容副本**写到
  `<repo>/snapshots/<id>/files/<rel>` + `manifest.json`；
  单文件 ≤4MB、单份快照 ≤64MB。
- `dev.snapshot` 现在真的落盘，返回 `snapshotId` / `snapshotDir` /
  `createdAt` / `savedFiles` / `totalFiles` / `files`。
- `dev.rollback` **必须带 `snapshotId`**（缺了就明确失败），按快照还原；
  三分支：有副本 → 写回 / 快照时干净且 git 跟踪 → `git checkout -- path` /
  未跟踪 → 不动并如实报告。
- **回滚本身也可逆**：回滚前先把当前内容存进 `pre-rollback/`，撤错了还能撤回来。

### 修复（`undoable` 不许说谎）

`Effects::undoable` 的语义是"**我们**能不能撤销"，不是"系统理论上能不能"。
11 条命令声明了 `undoable=1` 却没有撤销栈（`tab.move.*` / `tab.pin` /
`file.undo` / `session.named.*`）—— 对调用方说谎。

- 这 11 条的 `undoable` 归零，各带解释注释。
- `tools/check_ai_meta.py` 新增**第 25 项**"可逆性不许说谎"：
  按 `UNDO_MECHANISM` 清单做**双向**校验（声明了必须有机制；
  登记了也必须真声明 `undoable=1`）。

### 修复（`search.find` 的契约不成立）

宿主是 `IExplorerBrowser`（没有搜索带），`search-ms:` 协议走不通 ——
上一轮把 `implemented` 降级为 `false` 时只翻了那一个字段，
`aiExposed` 仍是 `true`（AI 看得到、调不动）、`outputSchema` 仍声明着
（承诺兑现不了）。现已 `aiExposed=false` + `outputSchema=nullptr`，
并在 `check_ai_meta.py` 第 10 项加判据 c：**未实现的命令不得声明 outputSchema**。

### 新增（MCP over stdio 入口：`--mcp-stdio`）

AI 通道原本只有命名管道一种入口，那是**给本机脚本**用的；标准 MCP 客户端
（Claude Desktop / VS Code / Cursor）只认 stdio 或 HTTP。业界现状是
Eagle 随附本地 HTTP MCP 服务器、微软给资源管理器做了官方 MCP 连接器、
Windows ODR 把 `odr mcp add --manifest` 做成系统级标准 —— 没有 stdio 入口，
别的 MCP 客户端要接本程序就得先自己写一个转发器。

- **`src/mcp_stdio.{h,cpp}`**：stdin 一行 → 注入 `params.__token` → 命名管道
  → 管道回一行 → stdout。是**桥接**而不是"再实现一遍协议"：协议、鉴权、
  `errorCode`、自断言、事件流全走原来那条路，stdio 与管道不可能出现两套行为。
  零依赖，只用 Win32。
- **`--mcp-stdio` 启动语义**：等价于 `--new-window` —— 本进程自己当服务器，
  **不**转发给已有实例（转发 = 进程立刻退出 = 客户端只看到管道断开）；
  同时强制开启 AI 通道（客户端用 stdio 拉起它，本身就说明要 AI 通道）。
- **JSON-RPC 通知按规范不回响应**，也不会污染响应流。
- **断开即退**：stdin EOF → 客户端断开 → 进程静默退出（退出码 0），
  不留半个进程在后台；MCP 模式下不弹"确定退出吗"（会把客户端晾在原地）。
- **退出时的 `std::terminate()` 已修**：原先 `Stop()` 从未被调用，进程退出时
  静态 `std::thread` 析构时仍 joinable → `libc++abi: terminating`，退出码 3。
  现在 `Stop()` 由 `App::OnDestroy` 与 `App::Run` 收尾各调一次（幂等），
  用 `CancelSynchronousIo` 唤醒阻塞在 stdin 上的读；线程赖着不走时改为
  detach 而非 join（join 会卡死 UI 线程，detach 后析构也不会 terminate）。
- **`tools/probe_mcp_stdio.py`**：端到端验证（真实进程 + 真实管道，
  不注入键鼠，符合 R21）。覆盖握手、工具清单形状、`tools/call`（含
  `structuredContent`）、未知工具的 `errorCode` 归类、`exbar/state` 扩展方法、
  通知不串位、断开后干净退出。
- 文档：`README.md` §3.2 / §3.5、`docs/AI-INTERFACE.md` §3「stdio 入口」。

### 新增（静态关卡：快捷键冲突）

同一组合被绑两次时，后一条**永远按不出来**（`InputRouter::BuildBindings`
的 `add()` 是先到先得且静默丢弃）。原先只有管理界面能看出来，源码里查不到。

- **`tools/check_commands.py`**：新增冲突检查，覆盖**全部三个来源** ——
  `data/keymap.json` 热键、`src/keymap.cpp` 的 `kSysKeys`（系统原生兜底）、
  `src/input_router.cpp` 的 `kExtra`（应用内附加）。检查器里复刻了一份
  `chord::Parse`（含修饰键别名、大小写、`PgUp`/`PageUp`、单字符落到 VK 表
  等），所以 `Alt+Left` 与 `Alt+←` 会被认出是同一个键。
- **`src/input_router.cpp`**：冲突同时写进日志，不再只标在管理界面上 ——
  `[keymap] 快捷键冲突：Ctrl+C 被绑了 2 次（先到先得，后一条永远按不出来）：…`。

### 新增（静态关卡：单文件 500 行）

`ARCHITECTURE-RULES` §3 要求单文件 ≤500 行，但没有任何机器检查，
靠人记着 → 事实上已经漂了。

- **`tools/check_components.py`**：新增行数检查 + **棘轮**。
  超限文件记进 `data/file-length-baseline.json`（git 跟踪，改它就是一次可见的 diff），
  之后只许降不许升。18 个历史超限文件（`shell_ops.cpp` 1384 行等）先记账，
  从此每长一行都要有人显式承认。

### 修复（文档数字过期）

`README.md` 里的规模数字全部是旧的（写着 2.33 万行 / 39 个单测 / 20.2 MB），
与仓库实际（3.14 万行 / 46 个单测 / 31.5 MB）对不上。已按实测校正，
并补了两条口径命令，下一个读者几秒内能自己复核：

```bash
ls -l ExBarBrowser.exe | awk '{printf "%.1f MB\n", $5/1048576}'
python tools/check_ai_meta.py | grep -cE '^[0-9]+\.'
```

### 新增（AI 通道：失败可机器分类 + 能力面自述）

让"新会话 → 提需求 → 调命令 → 缺什么补什么"这条闭环真正落在产品通道里，而不是外部脚本里：

- **`errorCode`（10 类）**：`tools/call` 的响应与 `command` 事件新增机器码，与中文 `error` 并存。
  `unknown_tool` / `not_exposed` / `not_implemented` 三态分离 —— 以前同一句
  "未知或未暴露给 AI 的命令" 混着三种修法完全不同的情况（新增命令 / 补元数据 / 写 handler）。
  另有 `bad_args` / `schema_invalid` / `danger_disabled` / `need_approval` / `blocked` /
  `failed` / `effect_not_met`（handler 成功但断言不符）。
- **`exbar/capabilities`（旧格式也可 `capabilities`）**：直接由命令表算出
  `{total, implemented, aiExposed, notImplemented[], notExposed[]}`。
  客户端不再需要 grep 源码推断"是没这条命令，还是有但没开放"。
- **`check_ai_meta.py` 第 22 项**：钉住 errorCode 白名单、三态可区分、
  两条通道都暴露能力面、能力面必须数命令表（不另存清单）、单测必须守着。
- `tests/ai_api_test.cpp`：新增 errorCode 五类断言与能力面计数一致性
  （`aiExposed` 必须等于 `tools/list` 条数，两个面不能各说各话）。

### 修复

- `app_commands_misc.cpp` 缺 `#include "prompt.h"`：`NamedSessionSave` 调用 `PromptText` 时
  `use of undeclared identifier`，整仓编不过（其余用 `PromptText` 的分片都包含该头）。
- `tests/run_all.sh` 的链接库补 `-luiautomationcore`：`shell_input` / `tab_uia` 引入 UIA 后，
  主构建的 `LIBS` 有这一项而测试脚本漏了，导致全量单测在链接期
  `undefined symbol: UiaReturnRawElementProvider`（与任何单个测试无关）。

### 新增（导航栏能力缺口：历史 / 最近访问 / 复制内容回读）

导航栏全操作面复核（AI 通道逐条实测）列出的缺口，逐条补成结构化入口：

- **`nav.history.list`**：列出当前标签可后退/可前进的条目（最近的在前）。过去下拉历史只有
  鼠标能看，机器侧只能盲按 `nav.back` 再回读路径 —— 跳到第 3 条得试三次，且不知道会路过哪。
- **`nav.history.jump`**：`{depth, backward?}` 跳到第 N 条。越界先拒绝（`GotoHistory` 对越界
  是静默忽略，机器侧会误以为跳过了）；`to` 取自历史条目而不是跳完再读 `activePath` ——
  导航是异步的，立刻回读只会读到出发地。
- **`nav.history.clear`**：与地址框下拉底部「清除历史记录」同一实现（`App::ClearHistory`），
  返回被清掉的「最近访问」条数。破坏性，需 `aiAllowDanger` + `approve`。
- **`nav.recent.list`**：地址框下拉里「最近访问」的结构化读面。
- **`path.copy.dir` / `path.copy.tab` 返回 `text`**：剪贴板没有读回面（也不打算给 AI 读剪贴板
  ——里面可能有密码），"到底复制了什么"只能由命令自己回报。
- **`state` 新增 `searching` / `searchScope` / `searchQuery`**：搜索态的唯一可回读来源；
  `check_ai_meta.py` 第 17 项把它登记进 `WRITABLE_STATE`，缺回读会直接 FAIL。

### 修复（导航栏复核发现的 5 处缺陷）

- **选中路径错挂到桌面（严重）**：`ShellView::SelectionPaths` 拿 `IFolderView::Items` 的子 PIDL
  去 `SHCreateItemFromIDList` 解析，等于按桌面根拼接 —— 在 `E:\ExBarBrowser` 选中 10 个文件夹，
  回读全成了 `C:\Users\<me>\Desktop\src`。改用 `SHCreateItemWithParent(当前 folder, pidl)`。
  受影响的不只是观测面：`bmk.add.selection` 直接采信这些路径且不校验存在性，
  会把桌面前的假路径写进书签。
- **搜索"假成功"**：`search.find` 与导航栏搜索框都返回 ok + `searching:true`，但视图原地不动
  （零结果查询后 `itemCount` 一个不少）。根因是 `search-ms:` 是协议处理器 URL，
  `IExplorerBrowser` 没有搜索带，`BrowseToObject` 返回 S_OK 却不切视图。
  现在 `ShellView::DoNavigate` 明确拒绝 `search-ms:`，`RunSearch` 只在导航成功后才记
  `searchFrom_/searchQuery_`（先记账再导航会留下假搜索态）。**真正的搜索仍是缺口**，
  见 docs/AI-INTERFACE.md §8。
- **`fs.open` 错误不透明**：目标不存在时只报"命令执行失败"，而人侧 `NavigateActive` 会弹
  "无法打开：…"。补 `NoteFailure("目标不存在：<path>")`；`fs.open.tab` / `fs.open.window` 同步。
- **`go.powershell` 名实不符**：标题"PowerShell 目录"、分组 `Go`（跳转到已知位置），
  实现却是 `RunTerminal(当前目录)` —— 起终端、不导航，且与 `ext.terminal` 完全重复。
  改为真导航到 `%SystemRoot%\System32\WindowsPowerShell\v1.0`（不存在则退回 `System32`）。
- **`nav.up` 在命名空间根的拒绝文案偏**：站在控制面板上按向上得到"已经是顶层（此电脑或网络根）"，
  与现场不符。改为"已经是顶层（此电脑、命名空间或网络根）"。

### 新增（开发期进程生命周期：`tools/agentctl.py`）

闭环的主路径是产品通道（`CommandBus` + 命名管道），**本工具只负责产品不该管的那一段**：
换 exe 必须跨进程，所以需要有人把新实例拉起来并保活。

- **`serve`**：构建（仅当源码比 exe 新）+ 以 `--ai` + `EXBARBROWSER_MULTI=1` 启动 + 等通道就绪，
  然后常驻持有实例。本环境会回收命令的子进程，一次性启动的实例在命令结束时就被收走。
- **`status`**：exe/源码新鲜度（`stale`）、pid 存活、通道可用、工具数、当前 profile。
  `stale=true` 是"正在验证旧 exe"的唯一可靠信号。
- **`gap`**：读产品的 `errorCode`（不解析中文、不 grep 源码）并翻成"改哪几处"；
  依赖选中项的命令先读状态再决定调不调（诊断工具不得有副作用）。
- **profile 感知**：`MULTI` 会把发现文件写到 `data\profile-multi\`，本工具在两个候选目录里
  找"pid 还活着且连得上"的那一份，不写死 `data\ai-pipe.txt`。
- **`down` 只关自己启的实例**（`data\.agentctl.pid` + 管道名里的 hwnd，走 `WM_CLOSE`），
  用户手开的窗口一律不碰。

### 新增（`AGENTS.md`）

给 AI Agent 的工程入口：三条不可协商（R21 / 无第三方库 / 视觉与品牌串单一来源）、命令速查、
代码规则、命名与命令契约、加东西的五条路径、真实坑位表、收工判据、文档索引。
§8「闭环流程」明确写成**通道优先**：结构化 = `tools/list` + `CommandBus` + `verdict/assertions`
+ `errorCode` + `exbar/capabilities`，外部脚本只做进程生命周期。

### 修复

- `app_wnd.cpp` 的 `WM_TIMER` 分支把成员函数 `App::hwnd()` 当成 HWND 值传给 `KillTimer`
  （`WndProc` 是静态成员，没有 `this`），编译报 "call to non-static member function without
  an object argument"；改用消息参数 `h`。

### 修复（结构化审计缺陷 #1–#4：撤销边界/路径校验/返回值/错误透明）

- **撤销按 actor 划边界**：快照栈带来源标签，AI 的撤销只到自己的上一次修改为止；
  顶快照来自界面操作时机器侧 undo 被拒（"上一次修改来自界面操作，AI 撤销到此为止"），
  不再穿透回滚用户几天前的手工整理。界面侧撤销不变（完整 LIFO）。
- **shell 命名空间路径入口校验**：`::{8-4-4-4-12}` 纯格式校验（40 字符），
  `bmk.add.path` / `bmk.edit` 双入口拦截——缺 `}` 的 CLSID 曾照单落盘，
  之后"打不开 + 通用图标"两头排查（当日真实踩坑两次）。
- **三条旧命令补返回值契约**：`bmk.add.path` / `bmk.add.current` / `bmk.newfolder`
  声明 outputSchema 并真实返回 `{"id": ...}`（add.path 移动场景返回 `{"id","moved"}`）；
  连带修复 AI 调 add.current 遇已收藏路径会弹 MessageBox 卡管道线程的问题
  （机器侧改结构化失败）。
- **8 个 handler 补 NoteFailure**：缺少参数 / 幽灵 id / 路径已收藏 / parentId 非分类 /
  空书清空 / 无历史撤销，均报具体原因而非笼统"命令执行失败"。

### 新增（书签能力缺口：批量收藏与复制）

书签全操作面结构化审查确认的两处能力缺口，本次补齐（命令 + 右键菜单双入口，R20 人机等价）：

- **新命令 `bmk.add.tabs`（收藏所有打开标签页）**：把当前所有标签页的目录批量加入书签栏，
  已收藏/空标签跳过（`skipped` 计数）。返回 `added` / `skipped` / `ids`（新条目 id，
  顺序同标签顺序）。入口：书签栏空白处右键 →「收藏所有标签页」（带报数反馈）。
- **新命令 `bmk.copy`（复制书签）**：复制一条书签，分类连子树深拷贝；副本名加「副本」
  后缀、插到原条目之后，返回新条目 `id`。子树所有 id 重新分配——原样拷贝会让副本与
  原件撞 id（Find 命中前一条，删除/改名作用到别人身上）。入口：书签右键 →「复制书签」。
- 两条新命令都带 outputSchema 并**真实返回 id**——与既有 `add.current`/`add.path`/
  `newfolder` 成功时不返回 id 的缺口相反，新命令从第一天就是全返回值契约。

验证：单测 42/42 绿；`check_ai_meta` 21 项全过；`check_commands` 三方一致；E2E 实测
`add.tabs`（added=2/skipped=1 → 二次调用 added=0/skipped=3 幂等）、`copy`（书签副本
紧挨原件；分类副本内子项 id 全新）。测试产物已清理，书签恢复基线。

### 变更（多实例与 AI 通道形态）

- **移除单实例锁**：删除 `Local\ExBarBrowser.Instance` 命名互斥体，不存在任何
  "已在运行 → 拒绝启动"的分支。默认启动改为**激活优先**：找得到已有主窗口就把路径经
  `WM_COPYDATA` 转发过去开新标签并前置窗口，随后新进程退出（360 行为）；找不到
  （最长等 400ms 吃掉双击竞态）就直接新开实例。显式多开：`--new-window`
  （"在新窗口打开"命令自动带上，共享同一份数据）或 `EXBARBROWSER_MULTI`
  （同时落到独立 `data\profile-multi`）。（docs/DECISIONS.md D4 已按复检条件修订）
- **AI 通道默认开启**：`aiEnabled` 默认值由 `false` 改为 `true`；settings.json 里
  显式写 `false` 才关闭。安全口径不变：仍需令牌（`ai-token.txt`），fail-closed 的是
  `aiAllowDanger`（高危命令开关，仍默认关）。
- **AI 通道热插拔**：`aiEnabled` 从"只读、重启生效"改为运行期可切——设置面板/
  `config.set` 改完立即启停管道监听，无需重启（内部走 `WM_APP_SYNC_AI`，避免在
  通道自己的连接上执行 Stop）。
- **每实例发现文件**：新增 `ai-token.<pid>.txt` / `ai-pipe.<pid>.txt`（数据目录下），
  多实例并存时客户端可枚举全部在监听的实例；规范名 `ai-token.txt` / `ai-pipe.txt`
  指向最后启动的实例。退出时只删属于自己的文件（内容校验），不会误删别人的。

### 修复

- **宽字符 printf 的 `%s` 截断**（4 处）：此编译环境下宽字符 printf 的 `%s` 按
  `char*` 解析，`wchar_t*` 实参只剩首字节。实测后果：AI 每实例发现文件被写成
  `a.<pid>.txt`（token/pipe 两份同名互相覆盖）；JSON 报错信息只剩首字符；状态栏
  容量单位只剩首字符；.txt 关联读取异常。统一改 `%ls`（`ai_server.cpp` / `json.cpp` /
  `shell_ops.cpp` / `view_stats.cpp`）。
- AI 订阅推送由 `Sleep(20)` 轮询改为事件等待（最多 20ms）唤醒，空闲订阅者不占 CPU；
  订阅确认写回后才切推模式（客户端看到的顺序恒为「订阅成功 → 事件」）。
- **订阅者断开后通道不再被堵死**：订阅分支原先忽略 `PeekNamedPipe` 的失败返回值，
  客户端一断开，服务端就在订阅态里无限空转 —— 既不重建监听实例，也不释放那个唯一的
  管道实例，之后所有客户端都拿到 `ERROR_PIPE_BUSY`（实测 err=231），直到重启程序才恢复。
  现在断开的管道会被立刻识别并退出该连接，服务端重建监听实例。
- 无订阅者时跳过状态快照轮询：AI 通道默认开启后，快照生成开销不应在没人看时每
  100ms 付一次。
- **命令枚举顺序**：`bmk.add.selection` 从枚举中间移到末尾。`CmdId` 无显式赋值，
  往中间插会让后面所有 id 整体位移，基线（`data/commands-baseline.json`）失配
  （`check_commands.py` 的 C1b 规则）。

### 新增（观测面：选中与焦点）

实测「将选中的文件夹添加到书签」暴露的观测面缺口：`ShellView::SelectedCount()` 内部存在
但仅用于命令前置，`state` 快照看不到选中与焦点，AI 只能靠猜目标。本次补齐：

- **状态快照新增 5 个字段**：`selectionCount` / `selection`（全部选中路径）/
  `selectionFolders`（其中文件夹）/ `selectionTruncated`（超过枚举上限时的截断标记，
  上限 256）/ `focus`（焦点面板：file_view / nav_tree / bookmark_bar / address_bar /
  unknown，按 `GetGUIThreadInfo` 焦点窗口的祖先链判定）。核心在 `ShellView::SelectionPaths`
  （IFolderView 枚举 + `SHParseDisplayName` 查 isFolder）。
- **新增命令 `bmk.add.selection`**：收藏选中的文件夹（过滤非文件夹与已收藏项，返回
  added/skipped）。第一条 `needsSelection=true` 暴露给 AI 的命令——AI 先读快照确认选中，
  再执行。`check_ai_meta.py` 第 4/14 项升级：观测面读不到选中（源码无 `selectionCount`）
  时这类命令自动收紧不暴露；第 8 项顺带修复 requires 元素匹配 bug。
- **会话持久化补 `sessionSelection`**：退出保存选中、切标签恢复选中，
  重开程序不再丢失现场。
- **`ShellView::SelectPaths`**：按路径列表恢复选中（IPersistFolder2 定位文件夹 +
  ILFindChild + SelectItem），供会话恢复使用。

文档同步：FULL-COVERAGE §4.2、OPERATION-MODEL 第 12 步、AI-INTERFACE（needsSelection
例外规则 + 快照字段示例）。

### 修复（审查 explore 对齐：功能断线接线）

全面审查本项目相对 explore 的输入/命令/UI 可达性，修复 4 处"数据或链路齐备但没接上"的断线：

- **Alt+P 预览窗格开关**：`data/keymap.json` 中 `hotkeyTogglePreviewPane` 原绑 `Ctrl+Alt+P`，
  与 `keymap.cpp` 系统键兜底表声明的 `Alt+P` 不一致（快捷键管理器显示前者、用户按后者无反应）。
  改为 `Alt+P` 并重跑 `gen_keymap.py` 同步 `keymap.gen.h`，出厂文件与编译期兜底一致。
- **侧键前进/后退无默认规则**：按钮枚举、shell_input 解析、InputRouter 派发都就绪，但内置默认表
  与 `mouse.json` 都没有侧键规则，侧键完全无反应。补内置默认：X1=后退、X2=前进，覆盖
  空白/标签/导航/地址/树/状态/预览/书签等全部可达 target；文件/文件夹项留给用户自配。
- **侧键消息未接入四个自绘栏**：标签栏、导航栏、书签栏、状态栏原本都不处理
  `WM_XBUTTONDOWN/UP`，侧键落在这些栏上被 `DefWindowProc` 静默吞掉。四栏统一接 XBUTTON，
  经新增的 `XButtonFromWParam`（与 shell_input 同口径，含 PARENTNOTIFY 物理状态兜底）
  解析按钮编号后交给 `MouseSink`，规则引擎在所有区域可达。
- **系统菜单缺应用入口**：窗口有标准系统菜单（`WS_OVERLAPPEDWINDOW` 含 `WS_SYSMENU`），
  但视图菜单/命令面板/首选项/快捷键管理器/退出只有导航栏与命令面板入口。追加到标题栏右键 /
  Alt+Space 系统菜单，`WM_SYSCOMMAND` 自定义 ID 段转投命令总线，与按钮同源，不复制视图菜单实现。

新增 `tests/mouse_rules_test.cpp` 固化侧键默认规则与方向（X1=后退/X2=前进）、target 覆盖、
wParam 解析；全量 42/42 通过，静态检查（keymap/component/command）全绿，全量重编通过。

### 移除（6 条假能力命令）

- **`tool.readtext` / `tool.navigate` / `tool.reveal` / `tool.select` / `tool.open` / `tool.listview`**：
  六条 `Op*` 声明从命令表删除。它们是 `fs.open` / `tool.list` 的语义重复，且**从未有 handler、
  没有键位、没有 agent 暴露**（`aiExposed=false`）—— 挂在目录里只是永远不会兑现的假能力。
  命令表 161 → 155，未实现 14 → 8，Tool 组 7/7 全实现。
  **枚举项保留并标废弃**（C1b：`enum class` 无显式赋值，删中间项会让后续 id 位移）；
  **legacy 映射与基线旧名同步删除**：C2"legacy 名只增不减"保护的是"用户配置引用过的
  工作命令"，而这 6 条从未实现、无 handler、无键位、无 agent 暴露——命令面板过滤
  `implemented=false`，内置 keymap 也没绑，不存在"用户 keymap 会失效"的场景。
  留着指向不存在命令的别名只会让 `check_ai_meta` 报"源命令不存在"。基线 id 数不变（161），
  旧名数 150 → 147。


### 修复（AI 管道并发串线）

- **管道名进程唯一**：无窗口进程（单测、headless e2e、启动早期）原来一律用
  `ExBarBrowser_ai_default`，两个并发实例会连到同一个管道，各自拿自己的令牌去
  握手，随机报 `bad_token`。受控复现：先起一个占位进程，`pipe_e2e_test` 立刻
  12 条失败；修好后同样条件下 35/35 全绿。现在后缀 = 窗口句柄（有窗口时）
  **+ pid**；客户端本来就只读 `ai-pipe.txt`，无需改。`pipe_e2e_test` 增加
  "管道名包含本进程 pid"断言，锁住契约。


**两轮：把"按了没反应"的命令清零，再补上文件工具集。**

这两轮解决的是同一类问题：命令目录里挂着名字、keymap 里绑着键，按下去却什么都不发生，
而且**没有任何提示**。静态审计一次抓出 13 条（`implemented=false` 但 keymap 有引用），
`input_router.cpp` 对它们直接 `continue`。现在剩 0 条 —— `check_commands.py` 的 PENDING
表已空，做不到的那三条键位连同键一起删掉了（见下面「移除」）。

### 新增（文件工具集：`tool.hash` / `tool.dupfinder` / `tool.compare`）

- **`tool.hash`**：MD5 / SHA-1 / SHA-256，走系统 BCrypt，**流式读**（64 KB 块，
  内存占用与文件大小无关）。算法名认不出就失败，不静默退回 SHA-256 —— 否则
  "我算的是 SHA-512"和实际结果无法区分，拿错摘要去核对下载包会得出错误结论。
- **`tool.dupfinder`**：先按大小分组（便宜且能排除绝大多数），命中的候选才读内容算
  SHA-256。判据是"大小 + 内容"，**不靠文件名** —— 改名后的副本一样能找出来
  （实测：`dup1.bin` / `dup2-renamed.bin` / 另一个目录下的 `dup1.bin` 被归为一组）。
- **`tool.compare`**：两侧按相对路径（大小写不敏感）配对，同大小再比内容；
  `byContent=false` 只比大小，此时报告里明确写"同大小不同内容会算作相同"。
  实测能把"1024 字节但内容不同"判成 differ，不是"大小相同即相同"。
- 三条**同一套双入口**（R20）：不带参数调用（热键 / 命令面板）＝ 目标取界面现状
  （选中项 / 当前目录 / 两个路径输入框），结果进**只读报告窗口**；带参数调用
  （agent）＝ 返回结构化 JSON，不弹窗。
- 结果里必带 `scannedFiles` / `hashedFiles` / `deniedDirs` / `timedOut` / `complete`：
  只给"没有重复"而不说"扫没扫完"、"有没有目录打不开"，用户会据此删错东西。

### 新增（`session.list` / `session.delete`）与修复（无参命令从 AI 调会卡死管道）

- **`session.list` / `session.delete` 落地，语义按真实模型收窄**：本工程只有**一条**
  已保存的标签组（`session.save` 覆盖它），所以 list = 读出那一条
  （`{paths, count, active}`，人侧进只读报告窗口）、delete = 清空那一条
  （`{cleared, count}`，破坏性 → agent 需 `approve`，人侧先弹确认）。
  没有假装"多个命名标签组"——那需要名字、重名与覆盖策略，是另一件事。
  至此 Session 组 4/4 全实现；顺带补上原来缺的回读：能存、能恢复，但看不到存了什么。
- **修复：无参数命令从 AI 调用会弹模态窗口，把管道卡到超时。**
  双入口命令一直用 `args.IsEmpty()` 判断"这次是人点的"，这对**无参数**命令是错的：
  agent 传来的也是 `{}`，于是走"人侧"分支弹报告窗口，而 AI 侧在等一个永远不会来的
  应答（实测 `session.list` → `{"ok":false,"error":"timeout"}`，`tool.dupfinder {}` 同理）。
  命令总线本来就有 `actor`（`ui` / `ai`），只是 handler 看不到。现在 Dispatch 记录并
  还原本次来源（`CommandBus::fromMachine()`），双入口统一用新判据
  `IsHumanCall(args)` = 空参数 **且** 不是机器调用；9 处判定全部换掉。
  返回值改成**"成功 / 失败"之外还要能说清"谁触发的"**，这类误判才不会再靠人肉发现。

### 新增（`search.find` 接上命令总线）

- **搜索从"只有搜索框能用"变成命令**：导航栏搜索框早就存在，但命令目录里
  `search.find` 一直是 `implemented=false` —— 命令面板搜不到、键位绑不上、
  agent 完全没有"搜索"这个能力。现在 `{"query":"…"}` 走同一条实现，
  返回 `{query, scope, url, searching}`。
- **搜索是导航，不是只读查询**：结果落在同一个 `IShellView` 里（排序 / 右键 / 拖放
  照旧），所以它会换掉当前标签的内容 —— 原声明 `Effects.readOnly=1` 写错了，
  撤销方式是再搜一次空串（= 回到来源目录）。
- **URL 拼装抽成纯函数**（`searchurl::Build` / `Trim`，`src/search_url.*`）：
  原来这段长在导航栏回调里，既不可复用也不可测。`tests/search_url_test.cpp`
  按住两件事：搜索词里的引号要翻倍转义（否则 `search-ms` 会把短语提前截断）；
  范围**原样**进 `crumb=location:`（现状行为 —— 路径含 `&` 会被当分隔符，
  这是已知局限，固定成契约等有测试兜底时再改）。
- **只打空格不再当成搜索词**：搜索框提交时不做 trim，于是敲几个空格回车就进入
  一个搜不到东西的搜索视图，看起来像"搜索坏了"。现在首尾空白在 `App::RunSearch`
  统一去掉（导航栏与命令共用这一处），空白即"退出搜索"。
- 端到端：`obj/e2e_search.py`（真实管道）实测四种输入 —— 正常词、首尾空格被 trim、
  只有空格 → `searching:false, url:""`、缺 `query` → `失败：缺少必填参数：query`。

### 新增（批量重命名 `Ctrl+Shift+R`、按类型选择）

- **`file.rename.batch`**：模式串 `{name}` / `{ext}` / `{old}` / `{n}` / `{nn}` / `{nnn}`。
  三条安全约束：**全部先校验才动手**（重名 / 撞已有文件 / 非法字符 / 空名）、
  预览确认（前 20 条「旧 → 新」）、逐项 `shellops::RenameTo`（`FOF_ALLOWUNDO`，`Ctrl+Z` 可退）。
  模式里没写 `{ext}` 也**不会丢扩展名**——"改前半段"是最常见的意图，悄悄丢掉 `.txt`
  会让文件双击打不开。纯函数 `filetools::PlanBatchRename`，单测 20 项。
  没有接 PowerRename：外部依赖不保证存在，而"按了没反应"正是要解决的问题本身。
- **`file.select.type`**：以首个选中项为样板选同扩展名的文件（Total Commander 的 Num+ 口径）。
  不取"所有选中项类型的并集"——那样选两三种之后点一次就等于全选，命令会退化成"全选"。

### 修复

- **AI 侧失败不再只说 `{}`**（`ai_server.cpp`）：声明了 `outputSchema` 的命令失败时，
  `hasResult` 为真会把空的 `structuredContent` 当正文输出，真正的原因
  （`r.error`，例如"目标已存在：gamma2.log"）被丢掉。现在失败优先、原因进文本块。
- **新命令失败时给出具体原因**（`CommandBus::NoteFailure`）：`tool.hash` 的算法名打错、
  `tool.dupfinder` 的目录不存在、`file.rename.batch` 的第几条冲突 —— 不再统一报
  "命令执行失败"。名字打错和值超范围该做的下一步完全不同。

### 新增（选择族、撤销、搜索框）

- **选择族四条**：`file.select.invert`（`Ctrl+I`）、`file.select.files`、
  `file.select.folders`（`Ctrl+Alt+D`）、`file.select.pattern`（`Ctrl+Shift+F`/`Ctrl+Shift+K`）。
  匹配语义交给系统的 `PathMatchSpecExW`，不自创通配语法。
- **撤销 `Ctrl+Z`**（`shellops::Undo`）：把撤销转发给 shell 视图自己的撤销栈
  （落盘操作都带 `FOF_ALLOWUNDO`，系统栈里本来就有）。定位菜单项用**菜单文本前缀匹配**，
  不用 `GetCommandString(GCS_VERBW)` —— 后者会逐个懒加载第三方 shell 扩展，
  扩展一阻塞，跑在 UI 线程上的函数永不返回，**整个消息泵卡死**（实测踩到）。
- **搜索框 `Ctrl+F`**：默认隐藏（地址栏是高频控件，不该为偶尔搜索让位），
  显示时占地址框右侧约 1/3，回车走 `search-ms:` 协议，空串＝取消搜索回到原目录。
- **命令面板支持带参命令**：此前含 `required` 的命令在面板里被静默跳过，等于
  "只有 AI 能用"。现在按 `inputSchema` 逐项追问（`CollectParamsInteractive`）。

### 变更

- **模态消息泵收成一份**（`src/modal.cpp` 的 `modal::Run`）：`prompt.cpp`（两个函数
  各一份）、`report.cpp` 以前各写一遍同样的循环，每处都要重新踩
  "`GetMessageW` 返回 -1 是错误 / 0 是 WM_QUIT"和"处理过的按键必须 `continue`"两个坑。
  `check_components.py` 的"模态消息泵"规则现在只允许 `modal.cpp`（另有 app/popup_list
  两处历史实现），`prompt.cpp` 移出白名单。

### 移除

- **三条永远做不到的键位**：`hotkeyMergeAllWindows`（`Ctrl+Alt+W`，单窗口模型没有
  explorer 标签可合并）、`hotkeyToggleCheckboxes`（`Ctrl+Alt+M`，shell 视图无公开接口，
  DECISIONS D2）、`hotkeyAiTogglePanel`（`Ctrl+Alt+A`，本工程的 AI 是**外部 agent**
  通过命名管道驱动窗口，窗口里没有也不需要"聊天"；真该给用户看的是管道名/令牌/最近调用，
  那是另一条命令）。
  留着键位等于对用户撒谎：按了没反应，且永远不会有。至此**占位键位归零**：
  `check_commands` 的 PENDING 表已空。

### 修复（首选项面板接上命令总线）

- **`win.preferences` 从"声明了没接线"变成真命令**：设置面板早就存在，但唯一入口是
  导航栏右键 →「设置…」（动作号 900），命令目录里那条 `CmdId::Preferences` 一直是
  `implemented=false` —— 于是命令面板搜不到它、键位绑不上、agent 也看不见，
  用户被告知"有首选项"却只能靠右键碰运气。现在 `reg(CmdId::Preferences, …)` 打开
  **同一个**面板（不是另写一个），弹窗锚点复用 `NavBar::SettingsAnchor()`：
  鼠标路径与命令路径各算一份位置，布局一调就会指向不同按钮。
- **那条声明自己的三个字段都是错的**：`implemented=false`（面板明明在）、
  `destructive=1`（开一扇窗，不改任何不可逆状态）、`Scope::System`（不碰进程外）。
  元数据写错不会报错，只会让"能不能用 / 要不要确认 / 该不该在面板里出现"全错 ——
  这正是它挂了很久没人发现的原因。
- 顺带纠正文档里一句错话：旧版 PARITY 写"设置面板已存在且视图菜单能打开"，
  实际入口是**导航栏右键菜单**，视图菜单里从来没有这一项。

### 变更（测试不再把"没测成"报成"测不过"）

有两条测试长期"存在但结论不可信"，处理方式是同一条：把**环境不满足**和**代码不合格**
分开报，而不是让假红长期挂在全量跑里 —— 一条永远红的用例，最后会让真正的回归也被当成
"这条一直红"。

- **`tests/run_all.sh` 新增退出码 77 = 无法运行**（对齐 autotools 的 77 约定）：
  单列计数、不计入退出码，但一定打印。此前沿用 "非 0 即失败"，于是同一类问题
  既可能真的是 bug，也可能只是这台机器测不了，看的人无从区分。
- **`clipboard_test` 先探前置条件**：它要打开系统剪贴板，而剪贴板是**全系统独占**的 ——
  别的进程正持有、或本进程没有交互桌面权限，`OpenClipboard` 都返回
  `ERROR_ACCESS_DENIED(5)`，9 条 CHECK 会全红。实测过这个来回：同一批 CHECK 上午 9 红、
  下午两次全绿，被测代码一个字节没变。现在探不通就打印
  "OpenClipboard 失败，GetLastError=5（进程会话 N）" 后按 77 退出，一句话说清是环境。
  探针等 0.5s，比产品代码自己的 100ms 重试预算宽 —— 那个预算管"一次复制别偶发失败"，
  探针管"这台机器现在能不能测"，标准不该一样。
  （诊断文本用窄字符串：`printf("%ls")` 在 C locale 下遇到非 ASCII 就截断，
  第一次改的时候正好把最要紧的错误码截掉了。）
- **`test_store` 的夹具写入改成必须成功**：`tf::WriteText` 是"写 `.tmp` → `MoveFileEx`
  替换"的原子写，改名那一下会被 AV / 搜索索引器瞬时占用挡住，而 12 处夹具调用
  **都不看返回值** —— 写失败后 `store.Load` 读到的是上一段留下的内容，于是报出假红。
  实测过一次：全量跑时 `无重复 -> 删 0 条` / `无重复时不产生撤销快照` 两条失败，
  同一个二进制单独重跑 3 次全过。现在重试 0.5s，仍失败按 77 退出。

---

**书签栏对标补强（DOpus / Total Commander / XYplorer）。**
围绕三条"结构上缺失"的东西：导入/导出的编码会**改坏路径**、分类**只能读不能组织**、
整栏**没有键盘路径**。

### 修复

- **书签 URL 编解码按 UTF-8 还原百分号序列**（`bmk_html.cpp` 的 `UrlToPath`/`PathToUrl`）。
  旧实现把每个 `%XX` 单独 `static_cast<wchar_t>(wcstol(hex))`，于是 `%E4%B8%AD`（"中"）
  被解成三个拉丁补充字符 `ä¸­` —— **从 Chrome/Edge 导入中文书签，路径整条写坏**，
  且每导一次坏一次。现在按 UTF-8 字节序列整体还原；非法序列返回空（拒绝把坏路径写进书签）。
  UNC 同时支持 `file://server/share` 与旧版多斜杠 `file://///srv/share`；`http:` 等非文件
  协议原样返回（不改成本地路径）。
- **`Unescape` 认数字实体**（`&#39;` / `&#x27;`）。Chrome 把单引号写成 `&#39;`，
  旧注释断言"Netscape 格式不会出现"——于是导入后名叫 `Bob's` 的分类变成字面的 `Bob&#39;s`。
- **`MoveTo` 的悬空指针**：曾在 `roots_` 里 `erase` 之后继续用旧 `dest` 指针
  `push_back`，把书签拖进分类是**访问违例**（实测崩溃）。改为 erase 后重新取目标容器。
- **首次运行写不出书签**：`Load` 失败分支没有清 `fileStamp_`，文件不存在时
  `FileChanged()` 保守判为"有变化"，`Save` 永远被拦。现在读失败同时清基线。
- **「按名称排序」只排顶层**：分类内部的顺序纹丝不动，等于排了一半。改为 `SortRecursive`。
- **导入会丢空分类**：没有 `H3` 文本的分类被当成空名，`Merge` 按名字对齐时全部塌成一个。
- **溢出菜单列出全部书签**（含已显示在栏上的），等于「点了没意义」。现在只列被折叠的项。
- **分类下拉是"整棵子树拍平 + 两空格缩进"**：几十行长菜单、层级看不出来。改为逐级下钻。
- **`Save` 静默覆盖外部修改**：本程序开着时，在编辑器里改 `bookmarks.html`，
  之后本程序加一个书签就把外部修改整份冲掉。现在拒绝覆盖、把内存书签另存
  `bookmarks.html.conflict.html` 并弹窗说明，两边都不丢。
- **`test_store` 会改用户的真实书签**：测试原本读写 `<exe>\data\bookmarks.html`，
  跑一次就把用户书签换成测试种子。现在全程在自己的 `%TEMP%` 目录里，用户文件零改动
  （已用 md5 验证）。
- **单实例转发按 1024 字符截断路径**（`main.cpp`）：深路径/长命名空间被截断后解析失败，
  表现为"从资源管理器打开深目录没反应"。改为按实际长度发送（接收端本来就是变长读取）。
- **书签图标两份所有权**：`BookmarkBar` 自建 `map<路径,HICON>` 并在 `Reload` 里全部
  `DestroyIcon`，而其余栏早已走进程级 `ico::Cached`。改为统一 `Cached`，`Reload` 变纯查表。

### 新增

- **书签栏键盘操作**：`Ctrl+B` 进入本栏；`←/→`、`Home/End` 移动，`Enter`/空格打开，
  `Shift+F10`/应用键出菜单，`Esc` 退回视图。此前整栏**没有任何键盘路径**，只能用鼠标点。
- **拖书签到分类**：拖到分类中间即装进去（整项描边提示），`MoveTo` 拒绝成环
  （拖进自己的子孙会让子树整棵脱落）。此前分类只能读，不能组织。
- **递归排序 / 清理重复书签**（`bmk.sort` / `bmk.dedupe`，均可撤销）。
- **菜单动作全部补成命令**：`bmk.edit` / `bmk.remove` / `bmk.newfolder` / `bmk.undo` /
  `bmk.clear` / `bmk.move` / `bmk.add.path` / `bmk.list`（读）/ `bmk.focus`。
  菜单与拖放改为**调用命令**，于是鼠标、键盘、命令面板、AI 走同一份实现（R20）。
  `check_ai_meta` 第 16 项（反向覆盖）从 7 项缺口降到 0（书签部分）。
- **空书签栏提示行**（`Ctrl+D` 收藏 / `Ctrl+B` 聚焦），而不是一条什么都没有的灰带。
- 单测：`bmk_html_test` 51 项（新增 URL 往返 / UTF-8 解码 / UNC / 数字实体）、
  `test_store` 65 项（新增 MoveTo 成环拒绝 / 递归排序 / 跨层级去重 / 冲突另存）。

---

### 修复（P0-6：审计改结构化 JSONL；被拒的调用也必须留痕）

审计过去只有一种形状：`data/audit.log` 里一行文本，记着"谁、什么命令、什么参数、成没成"。
它撑不住三件事，于是三件事都答不了：

1. **谁做的** —— `actor` 是个字符串，`palette` / `key` / `mouse` / `seq` / `drop` / `sysmenu`
   全是真人，却和 `ai` 一样只是字符串。分不清人和机器，审批与审计就没法分级。
2. **为什么没成** —— 失败原因被压成一句中文，客户端只能猜。
3. **机器可判读** —— 文本行里没有稳定的字段名，脚本要靠正则抠。

- **新增 `src/audit_log.{h,cpp}`**：审计的**唯一事实源**是结构化的 `auditlog::Record`，
  `data/audit.jsonl`（R18 要求的那份，一行一条 JSON）与 `data/audit.log`
  （旧文本镜像，给既有脚本 grep）**由同一条记录渲染** —— 两个来源对不上这种事从此不可能发生。
- **字段补齐到 15 项**：`ts / actorKind / actorName / cmd / args / argsHash / ok / verdict /
  errorCode / durationMs / approval / approvedBy / sessionId / traceId / pid`。
  其中 `actorKind` 用 P0-10 建立的 `ActorKind`（`human` / `ai`，不再只有字符串）；
  `traceId` 取 JSON-RPC / MCP 的**请求 id** —— 客户端手里只有它自己发的 id，
  审计里记着同一个 id，两边才能对账；自己编的序号客户端无从校验，等于没写。
- **拒绝路径全部落审计**：此前 AI 通道有一整段在进 CommandBus **之前**就返回了
  （未知命令 / 未暴露 / 未实现 / schema 非法 / 参数不合法 / 危险未批准），
  这些**一条审计都不写** —— 实测连续 4 次被拒，审计文件 0 增长。
  而被拒的调用恰恰最值得追（反复撞同一个错 = 调用方用错了方法；
  反复撞 `danger_disabled` = 有人在试探破坏性操作的边界）。
  现在每条拒绝都带机器可判读的 `errorCode`：`unknown_tool` / `not_exposed` /
  `not_implemented` / `schema_invalid` / `bad_args` / `danger_disabled` / `need_approval`。
- **审批状态与"没批"分开**：`not_required` / `granted` / `required` / `denied`。
  `denied` 与 `required` 差一个字，含义差很多：前者是"根本不让批"，后者是"要批，但这次没批"。
- **参数哈希算在完整参数上**：`argsHash` 用 `json::Stringify(args.raw(), 0)` 全量算，
  不像给人看的 `args` 那样截断到 160 字符 —— 否则两组只差末尾的长参数会撞成同一个哈希。
- 单测：`tests/audit_log_test.cpp`（哈希确定性 / 差异性 / **长参数只差末尾也必须不同哈希** /
  JSON 转义往返 / 文本镜像格式 / `Scope` 出作用域恢复）、`tests/audit_test.cpp` 重写。

### 修复（P1-7：`dev.build` / `dev.test` 的"超时"从"报一句"改成"真杀"）

自我迭代的闭环里，最危险的一步是"在开发机上跑 agent 写的代码"。这条路上的闸门有两个洞：

- **超时不是硬的** —— 旧实现里 `kJobTimeoutMs`（30 分钟）只用于**查询作业时现编一个"超时"结论**，
  进程其实还在跑。于是 agent 交一份死循环的代码 = 开发机上多一个永远停不下来的进程，
  而 agent 还以为"编译卡住了"，去改本来没问题的地方。
- **读输出这一步本身没有时限** —— 旧实现先在主线程里读干管道（阻塞到 EOF），
  **然后**才去等进程。"进程不退、管道不 EOF"这一种情况会卡在读上，超时根本轮不到。
  而孙子（编译器、测试二进制）攥着管道写端不撒手，正是最容易触发这一种情况。

修法：

- **作业对象（Job Object）当进程树的天花板**：进程一启动就 `CREATE_SUSPENDED`，
  **先归组再解冻**（否则孙子在归组之前就 fork 出来了，收不进来），
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` + `TerminateJobObject` 一次收掉整棵树。
  只杀直接进程是不够的 —— 孙子照样活着，而且管道写端还被它们攥着。
- **读与等分开**：读输出放到独立线程，主线程只负责盯时限并负责杀。
  于是"到点就返回"是真的到点，不是"等它自己结束"。
- **时限是常量，不给调用方自选**：`kBuildTimeoutMs`（30 分钟）/ `kTestTimeoutMs`（15 分钟）
  写在 `dev_tools.h` 里。让 agent 自己填超时 = 让被看管的人决定看管多久，那就没有闸门了。
  真实需求（编译很慢）应该**改这里**，那道改动会留在 git 里；运行时参数不会。
- **"被超时杀掉"与"跑失败"分成两个字段**（`timedOut` / `exitCode`）：
  前者指向环境或死循环，后者指向测试没过。混成一个字段，agent 就会改错地方。
- 单测 `tests/dev_proc_test.cpp` 新增 6 项断言，其中最关键的一条用
  "**孙子迟到 3 秒写标记文件**"来验进程树：标记文件没出现，才说明整棵树真的死了
  （只杀直接进程的话它会出现）。

### 修复（`FailKindCode` 落在匿名 namespace 里 → 链接不上）

`commands.h` 对外声明了 `FailKindCode`，定义却在 `commands.cpp` 的**匿名 namespace** 内 ——
定义只有内部链接，别的 TU 链不上（症状是 `undefined symbol: FailKindCode(FailKind)`，
而头文件与实现都"看着对"）。已把定义移到匿名 namespace 之外。

### 杂项（按职责拆分与工具链修补）

- **按职责拆分**（行数棘轮不许抬基线，只许把代码搬走）：
  `commands.cpp` → `commands_audit.cpp`（审计落盘）、
  `ai_api.cpp` → `ai_api_json.cpp`（JSON 序列化段）。
  拆分的判据是"一个文件只做一件事"，不是行数。
- **`tests/run_all.sh` 剔除孤儿 obj**：脚本用 `ls $OBJDIR/*.obj` 收集对象，
  源文件改名后旧 `.obj` 留在原地 → 链接期 duplicate symbol。
  现在按 `src/<base>.cpp` 是否存在来剔（`app.res` 例外）。
  推翻了报告 P2-5「该缺陷不成立」的结论：它在主构建不成立，在测试脚本成立。
- **`RiskName` / `ScopeName` / `TraceIdOf` 提升进 `ai_api.h`**：它们是**协议的一部分** ——
  `tools/list` 里写的、结果里回的、审计里记的，必须是同一个词。各写一份迟早分叉，
  而分叉之后客户端按 `danger` 过滤、审计按 `write` 记录，两边都"对"，但对不上。

---

## [0.2.2] - 2026-11-21 · 导航栏对标补强

**导航栏（DOpus / Total Commander / XYplorer）。**
围绕"看起来能用、实际是死的"这一类：盘根与 UNC 的补全、命名空间地址栏回车、
带引号的粘贴路径、后退/前进恒亮、面包屑 UNC 塌成一段。

### 新增

- **面包屑段右键菜单**（`nav.seg.<n>` 锚点）：在新标签页打开 / 在新窗口打开 /
  复制此路径 / 在此打开终端 / 属性。动作作用于**右键的那一段**，不是当前目录 ——
  用户右键 `D:\` 却复制出 `D:\Projects\foo` 是纯粹的错。幽灵段（向上导航后被放弃的
  更深分支）不弹菜单：它对应的目录可能已被删除。
- **导航按钮气泡提示**：5 颗纯图标按钮此前没有任何文字说明。灰掉的按钮提示会追加
  「（当前不可用）」，避免"按钮画着禁用色却写着『后退』"让人以为程序卡了。
- **视图菜单标注当前状态**：8 档模式 / 4 种排序 / 4 种分组都用 `✓ ` 打勾，排序项
  再追加「（升序）/（降序）」。此前菜单完全不标当前值，用户点完不知道有没有生效。
  排序方向从**视图**现读（`shellops::GetSort`）而不是抄配置 —— 用户点列头反向排序时，
  配置里还留着旧方向。
- **弹层预选并滚到当前项**（`PopupInitial`）：地址栏 ⌄ 下拉里混着后退/前进/最近访问
  三段，当前目录常在中后段，此前要在 20 行里自己找。视图菜单预选当前档位，
  键盘用户按 ↓ 即从这里继续。
- **弹层 `Home` / `End` / `PgUp` / `PgDn`**：命令面板与标签菜单都有 20+ 行，
  此前只能靠方向键逐行挪。翻页按"一次能看见几行"跳，不是固定 5 行。
- **弹层键入首字母跳转（type-ahead）**：本列表是 `LBS_OWNERDRAWVARIABLE` 且插入的是
  空字符串（文字在 `WM_DRAWITEM` 里按行号自绘），系统默认的 `LB_FINDSTRING` 完全失效。
  自建 900ms 累积缓冲，反复按同一字母在同类条目间轮换；前缀无匹配时退回单字符重试
  （否则打错一个字母后越打越没反应）。
- **标签右键菜单接入既有字形图标体系**：新建 / 关闭 / 重命名 / 着色 / 复制 /
  移动 / 锁定 / 重新打开各项现在都有图标，颜色二级菜单同步带图标。
  扩展现有 `PopupRow` 的图标槽为 `glyph`，由 `ico::IconId` 登记表提供唯一码位来源，
  绘制直接走已有的 `ui::DrawGlyph`；不建 `HICON`、不建字体、不写第二套菜单渲染器。
  7 字段聚合初始化保持不变，禁用项图标跟随禁用色。
- **其余自绘右键菜单同步接入同一套图标**：面包屑段菜单、书签项菜单、书签空白菜单、
  状态栏菜单全部补上图标。新增的删除 / 撤销 / 新窗口 / 终端 / 排序 / 导入 / 导出
  语义码位仍只登记在 `icon_glyph.cpp`，业务代码只写 `IconId`。
  状态栏段图标抽成 `StatusSegmentIcon()` 供栏体与右键菜单共用，
  不再复制一份 `switch` —— 同一段不会出现两个不同图标。
- **视图与排序菜单也补上图标**：刷新按钮右侧的 `kView` 按钮（气泡提示「视图与排序」）
  弹出的菜单，8 档视图 / 4 种排序 / 4 种分组现在逐行带图标。
  档位图标直接挂在唯一表 `viewmode::kModes` 上，排序/分组按列语义取既有字形；
  没有为这个菜单另写一份档位到图标的映射。
- **新增统一菜单组件 `ui::Menu`，从根上收口"菜单显示 / 图标显示"**：所有自绘菜单的内容
  只由 `Add` / `AddChecked` / `AddPath` / `AddPathChecked` / `Separator` / `AppendLabel` 组装，
  `Show()` 统一交给既有 `ShowPopupList` 并返回业务 action（不是行号）。
  标签菜单、面包屑段菜单、历史/地址下拉、视图排序菜单、书签菜单、状态栏菜单、命令面板、
  书签导入模式选择全部迁移完毕；业务代码不再手写八字段 `PopupRow`，也不再各自拼 `✓ `、
  数初态行、管 `HICON` 生命周期。勾选前缀、分隔行、禁用态、空路径字形兜底只在一处实现。
- **路径图标统一走 `ico::Cached`**：`ui::Menu::AddPath` 与文件夹树 `TreeNode` 共用进程级
  `(路径, 尺寸)` 缓存，弹层只借不销毁；解析失败时两者都回退到 `ico::IconId` 字形。
  `ClearCache()` 在进程退出时统一释放。标签栏/书签栏/导航栏这些长生命周期 chrome 图标
  仍走 `ico::Resolve` 并各自管理 DPI 重取，不与弹层混用同一套所有权。
- **`tools/check_components.py` 新增两条机械判据**：业务文件里出现 `PopupRow{...}` 或直接
  调 `ShowPopupList(` 即失败；`ico::Resolve(` 只允许出现在图标入口与三类长生命周期 chrome 中，
  菜单/树必须走 `Cached`。这两条把"补一个菜单就又散一次"的回退路直接堵死。
- **`tests/nav_input_test.cpp`**（36 项断言）：UNC 分段、引号剥离、盘根/UNC 补全切分。
- **`tests/menu_component_test.cpp`**：验证统一菜单的行构建、勾选前缀、分隔行、禁用态、
  路径图标兜底与追加文案；`tests/popup_layout_test.cpp` 补树节点字形槽回归。

### 修复

- **历史/最近访问下拉不再显示裸 `::{CLSID}`**。此前 `nav::History` 与
  `Config::recentPaths` 只存解析串，而下拉直接把 `path` 当 `label`；Shell 的
  `SIGDN_NORMALDISPLAY` 对 `::{GUID}` 在本机返回空串，无法事后补救。新增
  `ico::DisplayName` / `ico::RememberDisplay` 作为全项目唯一的"解析串 → 显示名"入口：
  导航期的权威名先登记，其后依次用 CLSID 的 `LocalizedString`、注册表默认名兜底；
  后退/前进下拉、地址栏下拉、书签自动生成名、标签标题全部复用它，解析串本身仍作为
  导航目标原样保留。
- **导航到「此电脑」「回收站」时地址栏显示 `>::>`**。面包屑标签 `nav::CrumbLabel`
  用 `s[1] == L':'` 判断盘根并截取前两个字符；命名空间解析串 `::{GUID}` 的第二个
  字符恰好也是冒号，于是被误判成盘根、只剩 `::`，画出来就是「左折叠箭头 + `::` +
  分段箭头」。盘根判定改用 `nav::IsDriveRoot`（`C:` / `C:\` 才认），命名空间标签
  保持完整解析串；同时 `NavModel::Rebuild` 对命名空间单段改用 shell 友好名
  （与 `AddressText` 同一口径），地址栏和面包屑都显示「此电脑」「回收站」而不是裸 GUID。
  同类的 `ToUnc` 也已排除命名空间，避免「复制为 UNC 路径」产出 `\\localhost\:$...`。
- **激活标签不再出现焦点边框**：`WM_LBUTTONDOWN` 会把焦点留在标签栏（裸键 Shift+←/→、
  Home/End 靠它生效），于是每次点标签激活都会画一圈 XOR 点线框。焦点语义保留，
  视觉边框移除：`tab_strip_paint.cpp` 不再调 `DrawFocusRect`，
  `WM_SETFOCUS/WM_KILLFOCUS` 也不再为此触发重绘。
- **地址栏对命名空间回车是死的**。命名空间在地址栏里显示的是**友好名**
  （"此电脑"、"回收站"），用户回车交回来的也是它，而 `SHCreateItemFromParsingName("此电脑")`
  必然失败。新增 `ico::ParsingPath` 作为"友好名 → 解析串"的公共入口（幂等，
  认不出的名字原样返回，不吞掉失败原因）。
- **粘贴带引号的路径导航失败**。资源管理器「复制为路径」给的是 `"C:\Program Files"`，
  从聊天窗口复制的常带首尾空格/换行，直接丢给 shell 得到 `E_INVALIDARG` ——
  症状是"粘贴路径回车没反应"，与引号毫无关联。新增 `nav::FromUserText`：
  去首尾空白（含全角空格）、剥一层配对引号、正斜杠归一。只做不改语义的清理，
  不展开环境变量、不改大小写、不猜相对路径的基准。
- **后退/前进按钮恒亮**。`SetPath` 只更新了「向上」一颗（因为它能由路径推出来），
  后退/前进从来没被喂过状态 —— 没历史可退时按钮亮着、点了没反应。
  新增 `NavBar::SetNavState`，由 `SyncChrome` 喂入（任何导航完成/失败都会经过那里，
  视图内部发起的导航也能跟上）。"按钮灰显 == 点不动"这条口径现在三颗按钮一致。
- **面包屑把 UNC 路径塌成一段**。`NavModel::Rebuild` 只认 `X:\` 一种形状，
  `\\server\share\dir` 落进 else 分支整条变一格 —— 面包屑点不回 `\\server\share`，
  而同一路径的「上一级」按钮是好的（`ParentOf` 早就支持 UNC）。分段抽成纯函数
  `nav::Prefixes`（盘根/UNC 共享根/命名空间/相对路径四类各自成规则）。
- **地址栏 `Tab` 补全在盘根与 UNC 上失效**。补全用 `ParentOf + Leaf` 切分，而
  `ParentOf("D:\\")` 与 `ParentOf("\\\\server\\sh")` 都返回空 → 回退到"当前目录 + 整串前缀"
  → 一个候选都没有。这两处恰是用户最需要 Tab 的地方（换盘、敲共享名）。
  改为 `nav::SplitForComplete`：按**最后一个反斜杠**切。
- **地址栏输入文件路径回车报"无法打开"**。`IExplorerBrowser::BrowseToObject` 对文件项
  返回失败，于是 `C:\Windows\notepad.exe` 回车得到一句警告。资源管理器 / Opus / TC
  在同一处的语义都是"打开这个文件"。`NavigateActive` 现在先判目录：文件交给 shell
  打开，且**不压历史栈**（文件从来不是一个目录，后退不该退到刚打开的 exe）。
- **面包屑分隔符在空目录上"点了没反应"**。`ShowChildMenu` 在无子目录时直接 return，
  箭头不闪、列表不出、也不导航，看起来就是"这个三角是坏的"。现在确认是目录才直接进入
  （与书签栏 `ShowChildTree` 同口径），命名空间项不动作。
- **导航按钮在 DOWN 触发动作**。本工程其余控件（标签、书签、状态栏段）全部在 UP 执行，
  弹层"点别处只关菜单、不触发那个控件"也是靠这一点成立的 —— 导航按钮是唯一的例外，
  于是菜单开着时点它，会"关菜单 + 立刻导航"两件事一起发生。改为 UP 触发，
  并补 `SetCapture`（按下后拖出栏外再拖回，抬起仍能正确判定）与 `WM_CAPTURECHANGED` 复位。

**输入纪律立规（R21）：禁止模拟输入抢焦点，一切操作走结构化命令。**
这不是新功能，是**边界**：本程序是本机正在使用的窗口，任何"替用户按一下"的做法都会把
光标/焦点/前台从人手上夺走 —— 代价不在编译期、不在脚本日志里，而在用户的下一次点击上。
第一版规则只禁了 `SendInput` 一族，漏掉了"用 `PostMessage` 投合成点击"这条合法外衣，
本次复查一并堵上，并把唯一一个违规探针改成观察式。

### 变更

- **架构规则新增 R21「输入纪律」**（`docs/ARCHITECTURE-RULES.md` §3 + §3.1）：
  - 全仓禁止 `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos` / `BlockInput` /
    `AttachThreadInput`；自动化脚本另外禁止 `SetForegroundWindow` / `SetActiveWindow` /
    `BringWindowToTop` / `SwitchToThisWindow`。
  - **同时禁止用 `PostMessage`/`SendMessage` 投输入类消息**（`WM_MOUSEMOVE` /
    `WM_LBUTTONDOWN` / `WM_LBUTTONUP` / `WM_LBUTTONDBLCLK` / `WM_KEYDOWN` / `WM_CHAR` 等），
    以及"扫一行坐标、每隔几像素投一次点击"的循环扫描法。理由：合成点击在本程序的真实代码
    路径上**必然**命中控件 → `ShowPopupList` → 弹层内部 `SetForegroundWindow`，
    结果与 `SendInput` 完全相同。判据是"有没有让被测窗口走一遍用户点它的路径"，
    不是"有没有动光标"。这条口子是第一版规则漏掉的 —— 原规则把 `PostMessage` 写成合规做法，
    而那正是循环点击的合法外衣。
  - 允许的入口按优先级：进程内 `CommandBus::Invoke(id, args, actor)` → 进程外 AI 命名管道
    `tools/call` → 只读 `exbar/state` → 纯函数单测 → `scripts/shot.py` 截图 →
    **人工点、脚本只观察**（必须真点才成立的流程）→ `PostMessage` 投**非输入**消息
    （`WM_CLOSE` / `WM_THEMECHANGED` / `WM_APP_*`，仅验证 `WndProc` 分支）。
  - 写清了一条容易误伤的边界：`src/` 里的 `SetForegroundWindow`（`app_wnd.cpp` 单实例转发、
    `popup_list.cpp` 弹层取键盘输入、`hotkey_manager.cpp` 唤出、`main.cpp` 探活）是
    **用户触发**的产品行为，不受 R21 约束 —— 判据是"触发源是人还是脚本"，不是 API 名字。
  - 说明为什么暂不用 UIA 代替：chrome 是自绘、无原生控件，UIA 需要自己实现 provider；
    且命令总线已经覆盖"机器要做什么"，R21 正是说**不需要点像素**。
- **`docs/ARCHITECTURE.md` §12 不变量扩到 14 条**（新增第 14 条"不模拟输入、不抢焦点、不循环点击"）。
- **`CONTRIBUTING.md`** 开头的必读段落加入 R21 摘要（原来只提 R1–R20），
  验证清单加一条"没有为了让测试跑通而加模拟输入"，禁止事项表加三条
  （裸 API / 合成输入消息 / 第三方自动化库）。
- **`docs/DEVELOPMENT.md` §2.4** 从"不要用模拟鼠标点击来测试"的劝告，升级为
  规则 + 四类合规验证方式的对照表（纯函数单测 / 命令总线 / 截图 / 人工点+脚本观察）。
- **`docs/MIGRATION-PLAN.md` §8.2 与 `README.md` 验证行**：原记"真实键盘 `SendInput` + 窗口消息"
  冒烟，按 R21 改为"人工操作 + 只读证据"，并补一张"要看的项 → 只读手段"的对照表
  （`exbar/state` / 日志 / 截图 / 观察式探针）。冒烟清单本身不变 —— 它描述的一直是人做的动作。

### 新增

- **`tests/run_all.sh`：一键跑全部单测**（32 个，实际 32/32 通过，约 40 秒）。
  做法是**复用主构建的 `obj/*.obj`**（排除 `main.obj` / `*_test.obj` / `*.check.obj`）链接每个测试，
  而不是为每个测试手推一份 `.cpp` 依赖表：
  - 依赖关系交给主构建保证 → 不会出现"文档里的编译命令悄悄失效"；
  - 每条测试只花 ~3 秒链接，而不是把 60 个 `.cpp` 重编一遍的 3 分钟；
  - 工作目录自动判定：源码里出现 `L"../data/` 就去 `tests/` 跑，否则留在仓库根。
    判据取**字符串字面量**而不是注释 —— `commands_test` 头注释里写着"不是 `../data/...`"，
    按注释判会把目录判反（写脚本时真踩过，已记在 `DEVELOPMENT.md`）。
  - 这个脚本补上了一个长期空洞：文档一直说"tests/ 有 N 个单测"，但每条都要手抄一串 `.cpp`，
    抄错就链接失败，于是实际只跑记得住的几条。`keymap_test` 正是因此**红着很久没人发现**。
- **`tools/check_components.py` 增加 `INPUT_GUARD` 段（4 条规则）**，让 R21 成为机器判据
  而不是文档里的一句话。与前两类检查的区别是**扫描范围**：跨 `src/` `tools/` `scripts/`
  `tests/`（出事的多半是验收脚本，而它在 `src/` 之外）。
  - `模拟输入注入`（`scope="all"`）：`SendInput` / `mouse_event` / `keybd_event` /
    `SetCursorPos` / `BlockInput` / `AttachThreadInput`，`allow` 为空。
  - `合成输入消息`（`scope="all"`）：`PostMessage*` / `SendMessage*` 投鼠标键盘消息。
    正则同时抓"消息号写宏"与"消息号写字面量"两种写法；字面量只认
    `0x0100–0x0109`（键盘）与 `0x0200–0x020E`（鼠标）两段，不误伤 `WM_CLOSE=0x0010`。
  - `第三方自动化库`（`scope="all"`）：`pyautogui` / `pynput` / `AutoIt` 等 —— 它们底层就是
    `SendInput`，引入一行就绕过上面全部判据。要求带括号/`import` 才算，不误伤
    `CmdId::SendKeys`（那是命令枚举，不是调用）。
  - `自动化抢前台`（`scope="automation"`）：只看 `tools/scripts/tests`，`allow` 为空
    （产品代码里的前台调用是用户行为，不该被这条扫到）。
  - 三种绕过写法（字面量消息号 / 消息宏 / `pyautogui.click`）都用注入文件验证过确实会红，
    退出码 1 并报出文件名与次数，验证后已删除。
  - `--list` 输出每条规则的命中情况与作用域（`all` / `automation` / `放行`），便于核对口径。
  - 修了一处**规则自伤**：`strip_comments` 原来只剥 C 风格注释，而 R21 会扫 `tools/*.py`，
    于是检查器自己注释里写的反例（`PostMessageW(h, 0x0201)`）被自己判红 ——
    **文档里写"不要这样做"反而触发失败**。新增 `hash_comments` 参数，`py`/`ps1` 同时剥 `#`，
    `scan()` 与 `--list` 用同一口径。

### 修复

- **文档全量对账：所有规模数字重新实测，并修掉文档里跑不通的单测命令**（2026-11-21 09:05）。
  这一轮不改产品行为，只让"文档说的"和"实际是的"对上。
  - **规模数字全线更新**（`README` / `ARCHITECTURE` / `OPERATION-MODEL` / `ROADMAP` /
    `CONTRIBUTING` / `DEVELOPMENT` / `DECISIONS` / `INPUT` / `MIGRATION-PLAN`）：
    `src/` 18,486 → **22,065 行**、57 → **60 个 `.cpp`**、48 → **52 个 `.h`**；
    `tests/` 24 → **30 个**、464 → **444 项断言**；检查 15 → **16 组**；
    exe 19.4 → **20.2 MB**；`check_ai_meta` 报的 `aiExposed` 仍是 89（未变）。
  - **超 500 行文件清单从 7 个改为 10 个**（`CONTRIBUTING` §3.1 与 `ARCHITECTURE` §3 两处），
    按实测行数重排：`shell_ops` 1013 / `shell_view` 1003 / `popup_list` 948 /
    `bookmark_bar` 856 / `bookmarks` 706 / `commands` 565 / `app_commands_files` 557 /
    `shell_input` 521 / `input_router` 505 / `nav_bar` 501。原文的 7 个里
    `shell_view` 与 `shell_ops` 的先后顺序也是反的（`shell_ops` 已经超过 `shell_view`）。
  - **`docs/DEVELOPMENT.md` §2.4 的单测命令有 6 条照抄会链接失败**，逐条实测后重写：
    缺 `src/str.cpp`（`str::FromUtf8Strict`）、`src/log.cpp`（`LogDbg`）、
    `src/platform_win.cpp`（`win::ThemeControl` → 还要 `-luxtheme -ldwmapi`）、
    `src/cmd_naming.cpp`（`AliasOf`/`LegacyOf`）、`src/icon_glyph.cpp`（`ico::Glyph`）、
    `src/nav_history.cpp`（`History::Goto`）的情况全都不存在。现在提供一个 `FLAGS` / `LIBS`
    变量，十段命令一律实测退出码 0。另外补了一句"单测为什么比主构建脆"的原因
    （PCH 会把缺失的 `#include` 补齐，单测不会）。
  - **`tests/keymap_test.cpp` 硬编码 `hotkeys != 70` 就失败**，而实际早在 83 条 ——
    这条单测长期是红的（退出码 3），只是没人跑到。改为 `kExpectedHotkeys = 83` 常量
    并注释"加键位要同步改这里"，同时补上完整依赖链的编译命令（原注释只列 3 个 TU）。
  - **`docs/ARCHITECTURE.md` §3 模块表漏了 4 个已存在的模块**：`icon_resolve`
    （图标识别的唯一入口）、`popup_toggle`（再点即收状态）、`folder_tree`（子目录树唯一入口）、
    `bookmark_io` / `bookmark_drop`（导入导出与 OLE 拖放）。补行并写清各自"明确不做"什么。
  - **键位规模从"70 热键 + 38 序列"改为"83 + 38"**（`INPUT` / `ARCHITECTURE` / `MIGRATION-PLAN`），
    并在 `INPUT.md` 补一条容易踩的事实：**83 条里有 13 条指向 `implemented=false` 的命令**，
    按下无反应是预期行为（`check_commands.py` 会逐条列出）。
  - 文档 `baseline` 统一到 `2026-11-21 09:05`（`README` 现状表另行标注 09:55 快照）。
  - **易变数字不再写死**：`src/` 行数、超 500 行文件的具体行数这类"每天都在涨"的数字，
    改为**只记名单 + 给出可直接复制的度量命令**。理由是这轮整理本身证明了问题 ——
    整理过程中并行会话又新增了 `.cpp` / `.h`、命令表 140 → 152、单测 24 → 32，
    写进文档的 22,065 行在 50 分钟内就过期了三次。规则改为：**数字必须带日期与口径，
    且口径要能一条命令复现**；无法复现的数字不如不写。
  - `README` 现状表新增一条"命令数每加一条就变、以脚本首行为准"的提示。
  - 整理期间曾有一刻 `check_commands` / `check_ai_meta` 各报 1 项未通过（并行会话在加
    `bmk.add.path`/`bmk.move`/`bmk.list`/`TabLock`/`TabCopyTitle`，参数与处理器尚未补齐）。
    当时在文档里**如实标为"在途状态"而不是假装全绿**；随后并行会话补齐处理器，
    10:05 复核三者均通过，标注已撤回并更新为最新数字。
    这条记录留在这里，是因为"检查红着但在修"与"检查绿了"必须能被区分 —— 下次遇到同样情形照此处理。

- **导航栏所有下拉"点一下就一闪而过"**。点面包屑分隔符 / 折叠 ‹ / 地址框 ⌄ /
  后退前进箭头，弹层出现后立刻消失，肉眼只看到闪一下。

  **真因是松手那一下把自己关掉了。** 导航栏各下拉在 `WM_LBUTTONDOWN` 里弹出
  （与资源管理器一致：按下即出，可以按住一路拖到想要的项上再松手）。弹层一创建
  就 `SetCapture(list)`，于是**同一次点击的松开**也送到列表，而光标还停在锚点上
  —— 锚点在列表**之外**。旧逻辑把"UP 落在客户区外"一律当成"点了外面该收起"，
  菜单便在松手瞬间自己关掉。

  这一次 UP 根本不是新点击，它是**开菜单那次点击的后半截**。`TrackPopupMenu`
  也是先等这一下松开，之后才把点击当作"关闭"。

  修法：判据抽成纯函数 `PopupUpIsOpeningRelease(downInList, downOutside, upInside)` ——
  只有"开菜单之后再没按下过 + 松开在客户区外"才认定为收尾并忽略；一旦用户重新
  按过（无论内外），后续 UP 都是真实点击，照常判定。

  顺带补上右键（`WM_RBUTTON(DOWN|UP)`）：标签/书签/状态栏的右键菜单开着时，
  用户的下一手可能是右键点别处，原先这条消息漏给 ListBox 默认处理、行为不可控。
  现在右键落在菜单内**只关菜单不选行**（`TrackPopupMenu` 同语义）。

  验证（结构化，未使用鼠标模拟）：`tests/popup_layout_test.cpp` 新增 9 项 ——
  5 项逐条覆盖判据，4+1 项用状态机推演完整消息序列（开菜单松手 / 点菜单内 /
  点外面 / 拖出去取消 / 右键点外），断言"开菜单那下松手菜单不关"且
  "点别处仍能关"。全量构建 60 TU 通过，`check_components` 29 条全绿。

- **`tools/probe_toggle.py` 改为纯观察式（原为 R21 违规）**。旧版从 x=150 到 700 每隔 6px
  向主窗口投 `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP`，命中就建模态弹层、再点一次 ——
  这就是"模拟鼠标循环点击抢占焦点"：弹层内部 `SetForegroundWindow` 会把前台从用户手上拿走。
  现在脚本**一次也不投点击**：只轮询弹层窗口数量，点击由人做。判据反而更强了
  （原版只能测"脚本点开的那条路径"，现在测的是用户真实操作的那条），并且不需要坐标、
  不需要扫描、不依赖布局。
- **`tools/probe_reopen.py` 删除**（未入库的历史探针，同样在循环投合成点击）。
  它要断言的"关掉之后必须能立刻重开"已并入 `probe_toggle.py` 的最后一步（收起后再盯 1.5s
  不得重弹），不必单独存在。

**标签栏手感收尾 + 缺口按优先级补齐。**
围绕"标签一多就天天撞"的几处：双击标签关闭、`+` 回到标签右侧并固定打开此电脑、
拖拽边缘滚动、Esc 取消拖拽、关闭后焦点显式化。

### 新增

- **双击标签正文关闭**：双击标签标题区关闭该标签（Chrome/Edge/VS Code 惯例）。
  - 双击**关闭钮**仍什么都不做 —— 此前这里写 `NotifyClose` 造成"双击一次关两个"：
    UP 已关掉标签 A，DBLCLK 按新布局又命中接管它的标签 B 的关闭钮。
  - 锁定标签跳过（与中键关闭同口径），鼠标规则（`sink_`）= MT_TAB 双击优先，
    规则吞掉就不再走默认关闭。DBLCLK 后的尾随 UP 由 `suppressDblUp_` 吞掉默认选择：
    它若再 `NotifySelect(旧下标)`，会在 CLOSE 消息之后选中"顶上来"的标签。
- **新建按钮 `+` 紧跟最后一个标签**，不再钉在标签栏最右端（空栏贴左、铺满贴右）。
  - 溢出时 `+` 固定在可视右缘、`»` 贴栏最右，两者不再重叠；
    标签裁切线抽成成员 `tabViewRight_` 显式表达 —— 过去从 `ovfRc_.left-S(2)` 反推，
    `+` 一移到标签右侧该反推就把 `+` 区域误算成标签区，导致 `+` 点不到。
    `HitTab` / `Paint` / `EnsureVisible` / 溢出菜单可见性四处统一读它。
  - 纯函数 `TabNewButtonX(tabsRight,minX,maxX)` 收口夹逼规则，
    `tests/tab_strip_rules_test.cpp` 覆盖空栏/未铺满/溢出/滚出栏外/窄栏五种边界。
- **`+` 按钮固定打开此电脑（`nav::kThisPc`）**，不再是复制当前目录。
  - 复制当前目录是"复制标签"（`tab.clone` / 右键"复制标签"）的职责，两条语义分开。
  - 右键菜单"新建标签页"、`tab.new` 无参默认值同步为此电脑（R20 人机等价）。
    此前"新建标签页"和"复制标签"两项实现完全相同（都复制当前路径），名不副实。
- **拖拽到可视区边缘自动滚动**（溢出时）：拖到左/右边缘带内触发 30ms 定时器持续滚动，
  让标签能拖到视野外；离开边缘即停。落点计算 `DropTargetAt` 抽成一份，
  MOUSEMOVE 与定时器共用同一口径，避免"滚一下插入线就跳到别处"。
  定时器在松手/Esc/捕获丢失三条路径上都停（`CancelDrag` 统一收口），不会残留。
- **Esc 取消拖拽**：拖到一半反悔，Esc 复位拖拽态并释放捕获，松手不会真的移位。
  `CancelDrag` 同时服务 WM_CAPTURECHANGED 与 Esc，两处状态清理不再各写一份。

### 修复

- **单击 `+` 没反应、双击才开此电脑**：根因是标签栏 DOWN 只报 `empty/tab/tab_close`，
  UP 才报 `new_tab/overflow`。点在 `+` 上时 DOWN 命中内置"双击空白 -> 上一级"规则并被
  `InputRouter` claim，UP 却被当成另一个目标然后被吞；双击时第一次 UP 没生效，
  最后一次 UP 又真的执行了默认新建。现在三处消息共用 `MouseTargetAt`/`TabMouseTarget`
  同一口径（关闭钮 > 正文 > `+` > `»` > 空白），并给 `InputRouter::OnMouseUp` 加了
  跨目标隔离：已知目标不一致时不再拿 DOWN 的 claim 吞掉 UP 所在控件的默认行为
  （Shell 的 `MT_UNKNOWN` 仍按沿用 pending 处理）。新增
  `tests/input_router_claim_test.cpp` 钉住这四个分支。
- **双击标签后不再误选"顶上来"的标签**：双击消息序列是 DOWN → UP → DBLCLK → UP。
  DBLCLK 已 `PostMessage(WM_APP_TAB_CLOSE)`，尾随 UP 再 `NotifySelect(旧下标)` 时，
  旧下标在 CLOSE 处理完后已经指向别的标签。现在 DBLCLK 置 `suppressDblUp_`，
  尾随 UP 只交给规则引擎清状态、不再跑默认选择；下一次 DOWN 会清掉该标记，
  即使 UP 落到窗口外也不会影响下一次点击。
- **移除"双击标签栏空白 = 新建标签"**：该隐式默认与 `+` 按钮语义重复、无提示，
  正是"双击变成打开我的电脑"的另一来源。空白双击现在只走鼠标规则
  （默认配置是"上一级"），不再偷偷建标签。
- **关闭活动标签后焦点显式落回新视图**：过去依赖"销毁视图后 `GetFocus()` 返回 null、
  `Activate` 恰好走 `!focus` 分支搬回焦点"的侥幸。现在关闭**前**就记录焦点是否在被关
  视图里，关掉后显式 `FocusActiveView()` —— 不依赖销毁次序，换关闭入口也不落空。

**命名与标识收编 + 已建入口接线。**
这一轮不改行为，只做两件事：① 独立后遗留的旧命名（品牌串三套、`eb_` 来源前缀、
`QuitExplorer` 语义错位）收口；② 上一轮建好却没人调用的入口（`MeasureText` /
`CreateChild` / `CreatePopup` / `ThemeControl`）接线，并删掉 4 个零调用死入口。

### 新增

- **标签右键菜单补齐 7 项**：重命名标签页、设置标签页颜色、向左/向右移动、
  移动到开头/末尾、关闭重复标签页。分三组插进原有菜单（关闭族 / 编辑族 / 移动族），
  边界项与"无重复可关"一律**灰显**而不是点了没反应（P4）。
  - 改名存 `TabRef::alias`，**不覆盖** `title`。`OnTabNav` 每次导航都会 `SetTitle(Shell 名)`，
    直接改 `title` 的表现是"改完进一次目录就变回去了"。新增 `core::DisplayTitle`
    作为唯一口径（改名 > Shell 名 > 路径末级），标签栏与窗口标题都读它 ——
    此前两处各写一遍 `display.empty() ? LeafOf(p) : display`，
    改名的标签会出现"标签栏新名、标题栏旧名"。
  - 颜色存**下标**（`TabRef::color`，0 = 无颜色）而不是 `COLORREF`：`core/` 零 Win32，
    且颜色必须与深浅主题配套换算。色板 7 档 + "无颜色"固定在 `Theme::TabColorFill`
    （深色 26% 混底、浅色 18%，两套色值 —— 同一份 RGB 在浅底上过重、深底上过暗）。
    激活标签画通栏色条（无颜色时退回主题 accent，保持原观感），
    非激活但着色的标签画内缩细色条：26% 的底色在深色主题下太弱，只靠底色分不出组。
  - 改名/颜色**按路径持久化**（`Settings::tabMeta`，与 `lockedPaths` 同一取舍）。
    不按标签序号（每次启动重排必然错位）、不按标签 id（进程内自增，跨会话无意义）。
    标签导航到别的目录时同步换成新路径的标记 —— 名字说的是那个目录，不是这个标签。
  - 关闭重复按路径**大小写不敏感**（`core::TabSet::DuplicatesOf`），锁定标签跳过并计数；
    一个也没关掉时 `tab.close.duplicates` 返回 false（否则审计记成成功、AI 以为清理过了）。
  - 三项同时登记为命令（`tab.rename` / `tab.color` / `tab.close.duplicates`，均 `aiExposed`）：
    右键菜单里的每个动作都必须有键盘/面板可达路径（R20 人机等价），
    否则只有鼠标能做。命令与菜单共用同一份实现，避免"菜单改名能存、面板改名存不住"。
- **`tests/tab_set_test.cpp`**：改名不覆盖 Shell 名、导航后改名仍生效、
  颜色越界夹回 0、`DuplicatesOf` 大小写不敏感且不含自己。
- **书签左侧图标 → 子目录下拉**。点书签项的图标格不再是"打开书签"，而是列出该路径的
  **直接子目录**，可就地逐级展开 —— 与"点地址栏分隔符（面包屑 chevron）"完全同一效果。
  两者此前是两套代码：枚举逻辑藏在 `nav_bar_actions.cpp` 的匿名命名空间里（`ChildTreeNodes`），
  书签栏要用只能复制一份。现在抽出 `src/folder_tree.{h,cpp}` 作为唯一入口
  （`Children` 建树 / `Show` 弹层，懒加载回调就是 `Children` 自己），
  `NavModel::ChildFolders` 仍是唯一的 shell 枚举实现，
  地址栏的两处调用点（面包屑 chevron、`‹` 折叠钮）一并改调它。
  - `BookmarkBar::Item` 新增 `iconRc`，与 `Paint` 里画图标的槽严格重合；`Handle` 的
    `WM_LBUTTONUP` 在调用默认行为前先判 `HitIcon`。
  - 图标格按下不预备拖动重排（`dragArm_` 排除图标区），否则在图标上稍一移动就变成重排书签。
  - 悬停图标格时该格单独画 `pressed` 圆角底（`hoverIcon_`），让"这一格能点开子目录"可见。
  - 纯分类节点（只装子书签、无路径）点图标退回"列分类内容"；空目录则直接进入该目录
    （与 mod 的 `ShowFsMenuForPath` 同口径 —— 弹空菜单等于点了没反应）。
- **`src/identity.h`**：品牌串、11 个窗口类名、单实例互斥体、命名管道、
  日志文件名、AI 服务名、环境变量、`WM_COPYDATA` 魔数的唯一来源。
  此前这些字面量散在 9 个文件里，改名要动 9 处。
- **`docs/NAMING.md`**：命名与标识规范。含"什么必须改、什么必须留"的处置表
  （跨软件契约 `EXBAR_*` 书签属性、数据文件名永不改）、命名空间按领域的规则、
  命令点分名与 legacy 名三条硬规则（稳定 id / 旧名永不删 / 消费方全覆盖），
  以及 9 个业界项目的比对依据（Blender 强制点分、VS Code legacy 数组、
  Qt 框架前缀归属、Opus 粗粒度命令、TC 前缀、Far 路由前缀、Chromium 三分载体、
  Tauri 窗口类名教训、Emacs Lisp 双连字符）。
- `tools/check_components.py` 从 15 条扩到 **24 条**（21 唯一性 + 2 禁令 + 1 接线）：
  新增品牌串（正式名/显示名/窗口类名）三条唯一性（`docs/NAMING.md` B1）、`MeasureText`、
  `SetWindowTheme`、`CurrentMods` 各一条，另新增 **`STALE` 段**——"入口已建但零调用"的检查
  （这类问题查重抓不到、编译不报错）。`STALE` 分三态，只有"`home` 内也无人调用"才判失败；
  "只有 `home` 内自用"降级为**提示**（该私有化，但代码是活的，不该让 CI 红）。
  据此把 `GetUiFont` / `GlyphFace` 收进 `ui::detail`（公开等于开了"绕过 DrawGlyph 自己建字体"的口子），
  `IconIdFromName` 登记为**预留**（数据驱动处按名字取图标，当前 keymap 不含图标名）。

### 变更

- **标签栏跨项目对标修正**（Chromium 标签栏设计文档 / VS Code Tabs / Mozilla 键盘无障碍 /
  Windows Terminal 主题设置）。28 项功能清单已补齐，这批修的是交互质量：
  - **快速连续关闭不再点空**。Chromium 把"关闭后下一个关闭钮留在鼠标下"列为明确需求；
    原实现关掉一个标签后按内容宽/均分/溢出重新分配，右侧标签整体左移，连点第二下必空。
    `TabStrip` 现在保存关闭前的实际宽度表，关闭后让接管位置的标签临时用**被关标签的宽度**，
    鼠标离开标签栏（`WM_MOUSELEAVE` → `CancelRapidClose`）才恢复。三种宽度模式下均实测对齐。
  - **DPI 切换重建图标缓存**。`ico::Resolve` 按 `SM_CXSMICON` 取图（实测 96dpi=16px、
    120dpi=20px），而 `ReloadTheme` 以前只重建 tooltip 和重算颜色，标签图标停留在旧 DPI
    尺寸 —— 与刚统一的图标尺寸令牌再次脱节。现在整批丢弃按新 DPI 重取。
  - **标签栏键盘焦点可见**。`SetFocus(h)` 后没有任何焦点绘制，Shift+←/→、Home/End 这些
    "标签栏作用域"的裸键用户看不到焦点在哪。`WM_SETFOCUS/KILLFOCUS` 现在触发重绘，
    `Paint` 在最后用 `DrawFocusRect` 画激活标签的点线框。
  - **锁定标签批量关闭有反馈**。Close Others/Left/Right 以前逐个调 `CloseTab`，每留一个
    锁定标签就响一次铃，命令仍返回 ok=true。`App::CloseOthers/CloseSide` 改为先收集再关
    （避免边遍历边 erase 导致迭代器失效）、过滤锁定项、只响一次铃并记日志，返回**实际关闭数**；
    全被锁定时命令如实报失败。对应菜单项在不可执行时灰掉（"重新打开"在历史为空时也灰掉）。
  - **Shift+F10 / VK_APPS 可打开标签右键菜单**（`WM_CONTEXTMENU`），落在激活标签上。
    此前菜单只有 `WM_RBUTTONUP` 一条鼠标路径，键盘用户够不到那 22 项。
  - **tooltip 不再随滚动全量重建**。新增 `ui::TooltipHost::MoveTools`，滚动时只发
    `TTM_NEWTOOLRECT` 更新热区矩形；全量 `SetTools` 只在标签集/DPI/窗口宽度变化时调用。
- **标签栏的关闭 ✕ 与新建 ＋ 改为字形，尺寸对齐其余栏图标**。
  这两个图标此前是全仓仅存的手绘折线（`tab_strip_paint.cpp` 的 `DrawCross` / `DrawPlus`），
  尺寸由两套互不相干的手算公式给出：✕ 跨度 = 框宽 − `thickness*2*2` = **12dp**，
  ＋ 跨度 = `(plusW/2 − 4) * 2` = **21dp**。21dp 落在 26dp 高的栏里几乎顶满上下边缘，
  同为"小图标"的锁（9×11）与溢出箭头（9×5）则小一圈 —— 三个图标挤在同一行，
  比例却来自三个不同的算式。
  改成 `ui::DrawGlyph` + 字形表：新增 `ico::IconId::Add`（`0xE710`），复用已登记但零调用的
  `ico::IconId::Close`（`0xE711`），两者统一 `kGlyphDp = 13`（实测墨迹 ✕ 10×10 / ＋ 11×11，
  与锁 `S(11)` 的 9×11 同一量级）。原注释"10px 字形在小尺寸下发虚"不成立 ——
  紧邻的锁与溢出箭头就是同尺寸字形。
  `tools/check_components.py` 同步收紧：`MoveToEx` / `CreatePen` / "绕过 DrawChevron 自绘箭头"
  三条规则的 `tab_strip_paint.cpp` 白名单全部移除，折线从此没有豁免位。

- **图标尺寸收成 `Theme` 令牌：4 档，取代散在 6 个文件里的 7 个裸数字**。
  上一轮把 ✕/＋ 改走字形后暴露了更根本的问题：`icon_glyph` 管"哪个码位"、`DrawGlyph`
  管"怎么画"，**唯独"画多大"没有归属** —— 于是 `S(9)`（书签栏分类箭头）、`S(10)`
  （面包屑/弹层 chevron）、`S(11)`（标签栏锁/溢出/书签栏 More）、`S(12)`（状态栏段图标）、
  `S(14)`（导航栏 `kIcon96`）、`S(16)`（标签栏/书签栏/弹层条目图标）各自为政，
  同一行里的三个图标可能来自三个不同的数字。`Theme` 此前有 `radiusBtn96` /
  `radiusPopup96`，却没有一个图标尺寸令牌。
  现在按**角色**分四档（不再按"哪个栏想画多大"）：

  | 令牌 | 96 DPI | 用途 |
  |---|---|---|
  | `iconChevron96` | 10 | 面包屑每级 / 书签栏分类 / 弹层展开器 / 导航栏分裂箭头与字段下拉箭头 |
  | `iconGlyph96` | 12 | 标签栏 ✕ ＋ 溢出 锁定 / 书签栏溢出 / 状态栏段图标 |
  | `iconCompact96` | 14 | 导航按钮、面包屑前导文件夹 |
  | `iconSmall96` | 16 | 标签栏 / 书签栏 / 弹层行的条目图标 |

  下限（8 物理像素）仍只在 `ui::DrawGlyph` 内部夹一次 —— 令牌 accessor 不再重复夹，
  两个下限就是两套口径，正是本工程删掉 `FontPx`/`Scale`/`IconSize` 的理由。
- **图标绘制原语从三个收成一个规则**。此前 `DrawIconCentered` / `DrawIconVCentered`
  按调用方给的 `size` **拉伸**，`DrawIconFit` 却按 `IconPx(icon)` **真实像素**，于是
  同一枚图标画出来的尺寸取决于走了哪个函数：120 DPI 下源图标 20px，导航栏 14dp 槽
  （18px）被压成 18px，弹层 16dp 槽保持 20px。现在 `DrawIconFit` 并入
  `DrawIconCentered` / `DrawIconVCentered`，统一规则是**落笔尺寸 = 图标真实像素，
  槽宽只作上限不放大**，槽比图标小（真放不下）时才缩到槽宽。
  顺带纠正一条错论据：实测硬边合成图标 20px→18px 时中间 alpha 为 **0** 个 ——
  `DrawIconEx` 缩放图标不做插值，"发虚/糊边"不成立；真问题是**尺寸不一致**。
- **导航栏分裂箭头在 96%~168% DPI 下完全不随 DPI 变化**（真 bug）。
  它调的是 `ui::DrawChevron(..., theme_->S(4), ...)`，而 `DrawGlyph` 有 `if (px < 8) px = 8`
  的下限 —— 于是按钮从 24px 长到 42px，箭头恒为 8px。实测：DPI 96/120/144 下
  `S(4)` 分别算出 4/5/6px，全部被夹成 8px。改走 `iconChevron()`（10px > 下限），
  "传进去的值"与"画出来的值"不再有落差。
- **面包屑分隔符的第二套 DPI 公式**。`nav_bar_draw.cpp` 的 `DrawSep` 用
  `MulDiv(12, 区高, 24)` 按区域高度反推字号，而它的注释写着"按 DPI 字号取" —— 
  代码与注释各说一套，结果是同一套 chevron 在面包屑里随栏高漂移、在标签栏/弹层里固定。
  改为 `th.iconChevron()`，三处同源。
- `src/nav_metrics.h` 删除 `kIcon96 = 14`：零引用，且是"图标尺寸有两个来源"的实证。
  该文件现在只放导航栏**几何**（按钮框 24、分裂箭头宽 12、chevron 区 20…），
  图标大小一律归 `Theme`。
- `src/nav_bar.h` 文件头注释修正：原文写"按钮 28px、图标 16px"，而代码取的是
  `kBtn96=24` / `kIcon96=14`。注释与代码各说一套，是"图标大小莫名其妙变了"时最难查的
  那种不一致。
- `tools/check_components.py` 增加两类机械校验（现共 23 唯一性 + 3 禁令 + 1 接线）：
  ① 禁令「图标尺寸绕过尺寸令牌」—— 直接把 `S(数字)` 交给图标/字形原语即失败；
  ② 尺寸令牌三段检查 —— `icon*96` 字段必须被 accessor 读、accessor 必须在业务代码里
  有调用点。两条都用注入违规的方式验证过确实会红（不是白名单式的形同虚设）。


  | 原 | 现 | 说明 |
  |---|---|---|
  | `eb_ui.{h,cpp}` + `namespace eb` | `ui_draw.{h,cpp}` + `namespace ui` | 16 个绘制/量字/图标原语 |
  | `eb_theme.h` | `ui_palette.h`（`UiPalette`） | 色值令牌是数据，绘制原语是动作，两者分开 |
  | `eb::TooltipHost` | `ui::TooltipHost`（`tooltip_host.h`） | 同属 UI 原语 |
  | `eb::CurrentMods` | `mouse_rules.h` 的全局 `CurrentMods()` | 读 `GetKeyState`，属输入端（R15） |

- **`WM_COPYDATA` 魔数改回 `0xEBB1`**（撤销本轮早先的换值）。原打算换成 `0x45584242`，
  理由只是"`EB`+`B1` 像 mod 期的玩笑"。但窗口类名 `ExBarBrowser_Main` 没变 —— 升级后若旧
  进程还在跑，新进程会找到它、用新魔数发消息、被静默拒绝，用户看到"已有一个 ExBar 窗口
  未响应"，而那个进程其实好好的。独立免除了"与 mod 兼容"的义务，但**同一程序的前后版本
  之间仍要兼容**，这是当时漏掉的一层。换值只买到审美收益，代价是一次真实断裂，故撤销。

  没有按原计划拆成 `Painter` / `Assets` / `Metrics` 三个类：本工程 16 个函数、约 200 行，
  拆三个命名空间只是多一层跳转。**判断依据是规模，不是教条** —— 等真长到需要分组再拆。
  全仓已无 `namespace eb` / `eb::` 残留，由 `check_components.py` 的 `N1/N2` 口径继续守着。
- **`CurrentMods` 从 UI 层移到输入端**（`eb::CurrentMods` → `mouse_rules.h` 的全局函数）。
  它读 `GetKeyState`，是**输入状态**而不是绘制资源；以前放在 `eb_ui`（绘制原语命名空间）里，
  于是每个栏在自己的 `WM_MOUSEMOVE` 里调 UI 层的函数去问输入状态，分层方向是反的
  （`ARCHITECTURE-RULES.md` R15）。顺带补上 **AltGr 特判**：右 Alt + 左 Ctrl 在德语/波兰语等
  布局下是字符输入修饰（`AltGr+E` = 「ę」），不能被当成 `Ctrl+Alt+E` 吃掉 —— 以前没有这个判断，
  非美式键盘上 `Ctrl+Alt+E`（切换扩展名）会在输入字符时误触发。
  `input_router` 的成员版本因需按"当前聚焦控件"决定是否抑制，保留自身一份。
- **`QuitExplorer` 主名改为 `win.quit`**。它的实际行为是"退出本程序"，而 mod 期
  这个名字是"重启 explorer.exe"。枚举名保留（`CmdId` 值一经分配不改），
  旧名作为 legacy 名仍可解析；`keymap.json` / `keymap.gen.h` / 校验器白名单同步。
- **`FindCommand` 改为三路解析**：精确名 → 旧名转点分名 → 点分名转旧名。
  此前是"先精确、再试点分"两段式，要求调用方知道自己手里是哪种名字；
  迁移期间三种组合都会出现，必须对任何输入都试三条路。
- `win::ThemeControl` 签名改为 `subApp` 后缀（`"Explorer"` / `"CFD"`），
  深浅两套 SubAppName 由它统一拼装，不再让调用方写三元表达式。
- `win::CreateChild` 参数 `h96` 改名 `heightPx`（它接收的是已缩放像素），
  并开放 `style` 参数（`StatusBar` / `BookmarkBar` 不需要 `WS_CLIPCHILDREN`）。

### 修复

- **标签栏：溢出时 » 按钮点不动**。判据是 `TabHit` 只跳过"起点在裁切区右侧"的标签
  （`left >= clipRight`），于是**跨过**裁切线的那一个标签在 » 按钮下面仍然命中 ——
  点 » 的结果是切到被遮住的标签，溢出菜单永远打不开。标签刚好多到溢出时，
  可见区的最后一个标签几乎必然跨界，不是理论边界。
  改为按**点**判（`TabHitInView`：横坐标落在裁切线上即不属于任何标签）。

- **标签栏：拖拽排序实际失效**。`WM_LBUTTONUP` 里先 `ReleaseCapture()` 再读
  `dragIdx_/dropIdx_`，而 `ReleaseCapture` 会**同步**派发 `WM_CAPTURECHANGED`；
  新加的复位分支把两个下标清成 -1，于是读到的永远是无拖拽 —— 拖了没反应、也不报错。
  `BookmarkBar` 早就在同一处写了"先提交再释放"的注释，标签栏一直漏着。
  改为先取快照、清状态，再释放捕获、投递移动。

- **标签栏：捕获被抢走后拖拽状态残留**。标签栏缺 `WM_CAPTURECHANGED` 处理：
  拖到一半右键弹菜单 / Alt+Tab 切走 → 收不到 `LBUTTONUP` → `dragging_` 一直是真，
  下一次鼠标移进来（哪怕没按键）标签就跟着光标乱跳。补上复位分支
  （`closeDownIdx_` 一并复位，否则残留索引会让下一次松开误关别的标签）。

- **标签栏：双击关闭钮关掉两个标签**。一次双击的消息序列是 DOWN → UP → DBLCLK，
  UP 已经关掉了标签 A，标签左移；DBLCLK 按**新布局**命中，而快速关闭模式刻意
  保住了关闭钮的位置——于是第二下又关掉标签 B，用户只双击了一次却少了两个标签。
  改为双击落在关闭钮上**不做任何事**（关闭钮不是可双击的控件）。

- **弹层高度未夹进工作区**。`RunModal` 先按原始高度决定向上/向下展开，再把 `y`
  推回 `rcWork.top` —— 于是 `top + height` 越过工作区底边，最后几行连同滚动条一起
  画到屏幕外，既点不到也滚不到。标签右键菜单 22 行 × 24dp，125% DPI 下 660px，
  768 高的屏去掉任务栏就顶到边。改为**先夹高度**再算位置。

- **标签栏：颜色档数有三份真相**。`Theme::kTabColorCount` 与 `tab_set.cpp` /
  `config.cpp` 两处硬编码的 `> 7` 并存，一旦色板扩容就会漏改 ——
  表现是"选了第 8 档，重启后变回无颜色"，且没有任何报错。
  档数收成 `core::kTabColorCount`（唯一定义，校验层也读它），`Theme` 转发。

### 变更

- **标签栏拖拽改为画插入竖线，不再整格刷悬停底色**。整格高亮读起来是"这个标签被
  选中了"，而拖拽时用户真正要看的是**插到哪一侧**。改由 `TabDropLineX`
  （纯函数，已单测）按拖动方向决定落点：往右拖画目标格右边，往左拖画左边，
  原地不画。Chrome / Edge / VS Code / Opus 一律用竖线，就是这个原因。

- **标签溢出菜单只列当前看不见的标签**。原来列全部 —— 点 » 的动机是"我看不到的
  那个在哪"，把看得见的也塞进来等于每次让用户在 15 行里找 3 个。部分可见
  （被 » 切掉一半）也算隐藏；激活标签打 ✓（溢出时最常问的第二件事是"我在第几个"）。

- **补 `Ctrl+1..8` / `Ctrl+9` 与 `Ctrl+PageUp/PageDown`、`Ctrl+Shift+PageUp/PageDown`**。
  `Ctrl+1..9` 在 ROADMAP 里列了很久但一直没绑（`g 1..9` 序列不是同一件事：
  序列要先想起前缀键）。移动标签用资源管理器/浏览器的惯例键，且带 Ctrl
  不受"标签栏"作用域限制——原来只有裸 `Shift+←/→`，必须先把焦点移到标签栏才能用。

- **下拉菜单"再点一次收不掉，反而重新弹出"**。地址栏分隔符最明显：点一下箭头变
  「⌄」并弹出子目录列表，再点一下箭头变回「›」——但列表又弹出来了，用户根本关不掉。

  **真因是关闭时机，不是"连点判据"。** 系统在派发点击之前**同步**给弹层发
  `WM_ACTIVATE(WA_INACTIVE)`；旧代码在此立刻 `PostMessage(WM_CLOSE)`，这条消息
  排在 `LBUTTONDOWN/UP` 之前被模态循环取走 → 弹层先销毁、捕获释放 → 剩下的
  `LBUTTONUP` 落到锚点控件，而各栏都在 **UP** 里执行动作 → 又调一次 `Show*`
  → 菜单"关了又开"。

  **关键事实：鼠标捕获不阻止激活切换。** 弹层是 `WS_POPUP` 顶层窗口，点主窗任一
  子控件都会让弹层失活，哪怕捕获仍在手里。此前两轮修法（400 ms 时间窗、关闭后留
  记录）都建立在"持捕获就安全"这个不成立的前提上，因此必然失效 —— 何况用户是
  "看一眼再点"，间隔 1~2 秒，任何毫秒级窗口都过期。

  修法对齐 `TrackPopupMenu` / 组合框下拉：失活时按 `popup::ShouldCloseOnDeactivate` 分流 ——
  **持捕获 + 前景窗同线程**（点的是本程序另一个窗口）→ 不关，这组 DOWN/UP 由
  `ListSubclass` 吞着，等 UP 判到"点在列表外"再关，锚点收不到点击、重开在消息层
  不可能发生；**其余**（点别的程序 / Alt+Tab / 捕获被抢走）→ 关，否则就是关不掉的菜单。
  配套：`WM_CAPTURECHANGED` 区分"LISTBOX 自己 SetCapture"（忽略）与"被抢走"（关）；
  外点的 DOWN 只记 `downOutside`，等 **UP** 才关并释放捕获 —— 少吞 UP 会顺带打开
  那个书签、切走那个标签。

  **所有弹层统一遵循**（`PopupProc` 是唯一实现），不只面包屑：后退/前进历史、
  地址框 ⌄、视图菜单、面包屑折叠 ‹、书签（图标树 / 分类 / 溢出 / 右键 / 空白右键）、
  标签右键与颜色二级、标签溢出、状态栏菜单、命令面板。锚点键带索引
  （`nav.crumb.3`、`nav.drop`、`bmk-icon:<id>`、`tab.menu.1`…），书签与标签用稳定
  id 而非下标（重排后下标会漂）。

  `popup_toggle` 现在只回答"某锚点的弹层是否开着"，供 `Show*` 兜底（键盘/命令重入）
  与面包屑箭头形态（› / ⌄）使用，不再有时间窗。

  验证：`tests/popup_toggle_test.cpp` 21 项（含一条枚举四种组合、断言"只有
  持捕获+同线程 不关"，防条件写反）；`popup_layout_test` 回归全绿；`check_components`
  29 条全过。进程外实测（真实鼠标点击）：点开 → 等 1.5 s（远超旧 400 ms 窗口）
  → 点同一处 → 弹层数归零（收起且不重弹）；点外部关掉后再点锚点 → 能重新打开
  （没有过度抑制）。

- **`win::CreatePopup` 的 this 指针设置时机晚于 `WM_NCCREATE`**。原实现在
  `CreateWindowExW` 返回后才 `SetWindowLongPtrW(GWLP_USERDATA)`，而弹层/对话框的
  窗口过程都在 `WM_NCCREATE` 里读 `CREATESTRUCTW::lpCreateParams`。改为直接用
  `lpCreateParams` 传递，并保留零值兜底。
- `log.cpp` 靠 `common.h` 间接取到 `identity.h`，单测独立编译时报
  "use of undeclared identifier 'id'"。改为显式包含。
- `tools/check_commands.py` 的三个盲点：
  ① 主名正则 `L"(\w+)"` 不认点分名，`win.quit` 那行被静默跳过（137 条变 136）；
  ② `implemented` 用主名比对处理器（处理器按 `CmdId` 枚举注册），改名后误报
  "已实现但没处理器"；③ 不认 `cmd_naming` 表里的旧名，用户已有 keymap 会被判失效。
- `tools/check_ai_meta.py` 第 11 项的两个缺陷（与 `check_commands.py` 同源）：
  ① 命令名正则 `L"(\w+)"` 不认点分名，`win.quit` 被整行跳过，于是误报
  "别名 QuitExplorer -> win.quit：源命令不存在"——**那恰恰是迁移完成的标志**；
  ② 覆盖率只按别名表左列（旧名）统计，迁移后的命令会被算成"尚无显式别名"
  （显示 1/137）。改为按两个方向取并集。修完输出 `137 条别名，全部有显式别名`。
- `tests/naming_test.cpp` 同类误判：点分主名被当成"缺别名"。改为
  "点分主名 + 旧名可解析 = 通过"。

### 移除

- `eb::FontPx` / `eb::Scale` / `eb::IconSize`（删除时的名字）：三个零调用的 DPI 换算入口。
  真实缩放一律走 `Theme::S()`；一个概念两个公式是"两套观感"的成因。
  其中 `FontPx` 还把枚举值当数字直接乘（`enum FontRole { FR_UI = 12 }`），
  枚举名义上是"字号角色"，实际只承载一个 int 槽位，而建字体的
  `GetUiFont` 又只收尺寸+字符串——角色在传递链上丢失，只能靠字体名反推。
- `eb::IconGlyph`（删除时的名字）：字形表已由 `ico::Glyph` 统一，这个按名字取的中间层零调用
  （`DrawChevron` 改为直接查 `ico::IconId`，不再经名字字符串绕一圈）。
- `eb::CurrentMods`：不是删除，是**搬家**（移到 `mouse_rules.h` 的 `ui` 之外），见"变更"首条。

### 文档

- 全部文档基线重核到 **2026-11-20 19:40**，并按代码实测修正数字：
  `src/` 18,464 行 / 57 `.cpp` + 48 `.h`；全量构建 10.5s（上一版误记为 29.8s，
  那是冷 PCH 一次性开销，PCH 命中后是 2.2s）；`check_components` 规则数 23。
- `docs/ARCHITECTURE.md` §2.1 重写窗口类注册策略（不再有"注册 `DefWindowProcW` 再替换"的
  过时描述，只剩 `EDIT` 子类化一处例外）；§3 模块表补 `identity.h` / `eb_ui` / `platform_win`；
  §12 不变量从 10 条扩到 13 条（新增 3 条纯机械约束）；§13 补 `ebop` 与 `ebai` 的对应关系。
- `docs/DECISIONS.md` 补 **D8**（从主仓独立为自包含仓库）与 **D9**（品牌串收编到 `identity.h`）；
  D4 补魔数换值；D7 标注"复检条件已触发但决策维持"及原因（不是引 CMake，而是把清单改成自动扫描）。
- `docs/NAMING.md` 实施状态表按实际补全（`MeasureText`/`ThemeControl`/`CreateChild`/`CreatePopup`
  接线、`QuitExplorer` → `win.quit`、三个校验器改动），并如实记录 1/137 的迁移进度。
- `docs/DEVELOPMENT.md` 补"新增头文件必须自足"（PCH 会掩盖缺失的 `#include`，
  单测单独编译才暴露 —— `log.cpp` 就是这样被发现的）；发布前检查补静态检查与基线更新两项。

### 接线（建好但此前无人调用）

- `eb::MeasureText` → 8 处裸调 `GetTextExtentPoint32W` 全部改用，
  顺带删掉 6 处冗余的 `SelectObject` / 还原样板（每处都各自维护一个 `old`）。
- `win::CreateChild` → 4 个栏的 `CreateWindowExW` + `SetThis` 两行变一行。
- `win::CreatePopup` → 弹层 / 对话框 / 序列提示共 4 处。
- `win::ThemeControl` → `nav_bar`（`CFD`）、`popup_list`（`Explorer`）共 3 处。

### 第二轮：命名收尾 + 校验器补硬约束

**① 命令主名全部迁到点分名（137/137）。**
`commands.cpp` 的 `d.name` 从旧驼峰名改成点分名（`TabCloseOther` → `tab.close.others`），
`Cmd` 前缀随之自然消失（零残留）。`legacyName` 改为 `cmdnaming::LegacyOf(name)` 反查 ——
此前是直接取 `d.name`，主名迁移后会让 `name` 与 `legacyName` 变成同一个值，
client 的"旧名对照"静默失效。

**`file.*` 与 `fs.*` 不合并。** 原计划"`Cmd*` 15 条并入 `fs.*`"作废：`file.*` 是交互式
（弹输入框/作用于当前选中），`fs.*` 是确定式（目标由参数点名）。`commands.h:39-47`
早已论证两套语义应当共存，合并会丢掉这个区分，并改写已暴露给 AI 的 `fs.*` 名。
`Cmd` 前缀去掉之后，两者的差异由第二段表达（`file.copy` vs `fs.copy.toclip`），足够清晰。

**② `eb` 命名空间拆解。**
`eb_ui.h/.cpp` → `ui_draw.h/.cpp`（`namespace eb` → `ui`）、`eb_theme.h` → `ui_palette.h`
（`EbColors`/`EbPalette` → `UiPalette`/`PaletteFor`）。21 个文件、约 77 处调用点。
`shell_view.cpp` 里同名的局部变量 `HWND eb`（ExplorerBrowser 缩写）一并改名为 `ebw` ——
它和旧命名空间同名，读代码时容易误认。
`ui::GetUiFont` / `ui::GlyphFace` 收进 `ui::detail`：它们本来零外部调用，
公开等于给了"绕过 `DrawGlyph` 自己建字体"的口子。

**③ `CurrentMods` 移到输入端并去重。**
`eb::CurrentMods` → `mouse_rules.h` 的 `CurrentMods()`（它读 `GetKeyState`，属输入状态，
放在 UI 层是分层反向）。同时发现 `input_router::CurrentMods` 与它**一字不差**（含 AltGr
特判）—— 校验器此前以"需按焦点抑制"为由白名单放行，读代码后确认成员函数没有任何额外
逻辑，改为 `return ::CurrentMods();`，白名单从 2 个文件收紧到 1 个。

**④ 校验器补三条硬约束。**
- **C1 命令 id 稳定**：`check_commands.py` 与 `data/commands-baseline.json` 比对枚举顺序。
  `CmdId` 没有显式赋值，值 = 声明顺序；中间插一条会让后面所有 id 位移，外部脚本/AI
  按 id 调用就会"调 A 执行 B"。现在只允许在末尾追加。
- **C2 旧名只增不减**：基线的 `legacyNames` 集合只许扩大。用户已有的 `keymap.json`
  写的是旧名，删一条就是静默丢键。
- **`eb` 废弃前缀**：3 条 `FORBID`（`namespace eb` / `eb::` / `#include "eb_`），防回流。

**⑤ `STALE` 检查从两态改三态。**
原来"外部有调用就算通过"太宽：`GetUiFont` 只在 `ui_draw.cpp` 内自用也会被判通过。
现在区分——外部有调用（通过）/ 仅内部自用（提示"应私有化"）/ 连内部都没调用（失败）。
顺带修掉"同名 `.h` 的声明行被算作外部调用"的漏检。

### 第二轮审查：发现并修正 7 个问题

以"假设自己改错了"为前提重审，以下都是**第一轮没发现**的：

- **`WM_COPYDATA` 魔数换值撤销**（恢复 `0xEBB1`）。见上方"变更"段的说明。
- **C1 规则漏报中间插入**：原判据比"已有序项的相对顺序"，而 `[A,B,C]` vs `[A,X,B,C]`
  的相对顺序**没变** —— 恰好漏掉最危险的情况（后续 id 全部位移）。改为前缀判据。
- **C1 读错数据源**：基线存的是表行顺序，但 `CmdId` 数值来自枚举声明顺序，两者当前
  确实不同（枚举 @69 = `CopyToFolder`，表 @69 = `FileFocusTop`）。新增 `EnumOrder()`
  从 `commands.h` 读，重建基线。
- **`FindCommand` 第三路已是死路径**：注释还写着"迁移期间三种组合"，但迁移已完成。
  删掉该分支；三路线性扫描改为 `unordered_map` 索引。
- **`UiPalette` 6 个令牌零消费**：`editFocus`/`editPlaceholder`/`listText`/
  `listInertText`/`chevronHot`/`panelBorder` 定义了但从不被取用。删掉，并新增校验规则
  "令牌未被消费"；位置式初始化改指定初始化器（23 个字段逐位核对，颜色值零变化）。
- **`CreateChild` 与 `CreatePopup` 的指针约定不一致**：前者传 `nullptr` 再事后设置。
  栏目前不用 `WM_NCCREATE` 所以侥幸无事，但下一个加该处理的人会拿到空指针。统一。
- **R12（窗口类 ≤3）被错列为独立待办**：R12 原文即"B5 之后"，它依赖载体合并。
  标注前置条件，当前 11 个类暂不启用。

### 第三轮审查：生产路径上的诊断代码（性能）

前两轮查的是"名字"和"规则"，这一轮查**运行时到底做了什么**，发现的问题比命名问题严重：

- **`ReapplyPreviewPane` 里有 ~160 行诊断代码，而它由 120ms 定时器反复调用。**
  诊断内容包括：在最多 40 列里逐个 `GetDetailsOf` 找 Size 列、对前 4 项取
  `PKEY_Size`（网络路径 = 真实 I/O）、用 `IColumnManager` 把 Size 列**移除再加回**
  （视图会闪）、以及一段 **UIA 全后代遍历**（跨进程调用）。用户每次把鼠标移过
  文件列表都要付一遍这个代价。→ 抽出 `DiagnosePreviewPaneState()`，由
  `-DEXBAR_DIAG_PREVIEW` 控制；函数本身从 168 行回到 13 行。
- `SetViewMode` 里 93 行同类诊断 → `-DEXBAR_DIAG_COLUMNS` 控制。
- 这些诊断虽在 `#if` 里但从**没有任何开关**（等于永久开启）。加开关后实测：
  产物里 `[cols]`/`[uia]`/`[late]` 三个特征串**均 0 次**；运行 5 次视图切换
  **新增日志 0 行**。
- **`LogLine` 与 `LogDbg` 是两个名字、一份实现。** `shell_view.cpp` 用前者 31 处、
  其余用后者 54 处；`log.h` 称 LogLine 是"兼容名"，但它实际已是并列主名。
  → 统一到 `LogDbg` 并删除 `LogLine`，新增校验规则"日志入口唯一"。
- 删掉 `shell_view.cpp` 里一行注释掉的 `SetPropertyBag` 诊断；修正 `common.h`
  里"两份轮转策略还不一致"的过时注释。

**方法上的教训**：前两轮只查"是否违反既有规则"，规则之外的问题（性能、过时注释、
注释掉的代码）全部漏掉。这类问题静态检查抓不到，只能逐行读生产路径在运行时做了什么。

### 第四轮审查：静态检查器本身失效（比报错更危险）

前三轮查的是代码与名字。这一轮查**检查器有没有真在工作**，结果发现一个静默失效：

- **`check_ai_meta.py` 的条目正则只命中 21 / 140 条。**
  `116241c` 把 119 条无参 schema 提成 `kNoArgs` 常量后，正则在 `argsSchema` 位置
  要求字符串字面量，一条也匹配不上。脚本照旧打印「全部通过」，第 7 项还写着
  「18/21 条有明确特性」—— 分母从 140 掉到 21，输出里没有任何异常信号。
  检查面静默缩水 85%。
  → 改为**按结构解析**：定位 `kTable`，深度感知地按顶层花括号切条目、按顶层逗号切
  字段（跳过 `Effects{0, 0, ...}` / `Requirements{...}` 内的分隔符）；字段值允许是常量名，
  去 `constexpr const char* NAME = ...` 处取值。再硬断言
  **「解析条目数 == 源码中 `CmdId::` 出现次数」**，任何解析退化当场失败。
  第 2/5/13 项现在对全部 140 条生效（此前只对 21 条）。
- **R20（人机等价）从未被实现。** `ARCHITECTURE-RULES.md` 写了这条规则，
  `check_ai_meta.py` 的 docstring 也声称对账「R18 / R20」，但没有任何检查项对应它。
  实际上只保证了"AI 能调的都已实现"，没保证"AI 能调的人也能调"。
  → 新增第 15 项，按两条路判：
  - **无必填参数** → 命令面板（`Ctrl+Shift+P`）列出全部已实现命令并以 `Args::Empty()`
    调用，机制保证可达。检查器断言 `ShowCommandPalette` 仍保持这个行为，改了立即报错。
  - **有必填参数** → 面板点了必然失败，必须另有能把参数递进去的路。17 条中 14 条是靠
    「确定式双胞胎」（`fs.delete` ↔ `Delete` 键等），3 条是结构化查询（`tool.list` /
    `tool.topn` / `tool.diskusage`），单独登记为 `AI_ONLY_QUERIES`。
- **低估过的坑**：第一版 R20 按"命令 id 在人类侧有没有调用点"扫，把
  `nav.home` / `session.save` / `tab.close.duplicates` 全报成不可达 —— 其实人在面板里
  点得到。面板是**一条**入口覆盖全部无参命令，不该按命令逐个找。规则写成机制判定后
  误报清零。
- `HUMAN_PATH` 里 `fs.properties` 原写「`Alt+Enter` → `CmdProps`」，核实后**并不存在**
  这个绑定（`keymap.json` 与 `input_router.cpp` 的 `kExtra` 都没有）。真实入口是状态栏
  「选中容量」段的点击（`app.cpp` 的 `SetClickFn`）。登记表写错入口比不写更糟 ——
  会让下一个人以为已经验证过。
- 登记表加两道自检：`HUMAN_PATH` / `AI_ONLY_QUERIES` 的键必须仍在 `aiExposed` 集合内，
  否则报「登记失效」。名字写错时不会报错、只会静默不生效，这是登记表最典型的腐烂方式。

**破坏性验证**（确认检查器真能抓，不是空转）：把未实现的 `DupFinder` 标成 `aiExposed`
→ 第 1 项报出（旧版会放过）；把 `NavHome` 的 schema 改成 `"{}"` → 第 5 项报出；
给 `bmk.add` 加必填 `path` 但不改 handler → 第 3 项报出；删掉 `tab.rename` 的
`HUMAN_PATH` 登记 → 第 15 项报出。

**文档数字全线失准**，一并按实测更正（README / AI-INTERFACE / OPERATION-MODEL /
NAMING / PARITY / ROADMAP / ARCHITECTURE）：

| 项 | 旧 | 实测 |
|---|---|---|
| 命令目录 | 137 | **140** |
| 已实现 | 108 | **111** |
| 对 AI 暴露 | 86 | **89** |
| 单测 / 断言 | 20 / 354 | **24 / 464** |
| `check_components` 唯一性规则 | 21 | **28** |
| `check_ai_meta` 检查组数 | 14 | **15** |
| `src/` 规模 | 18,486 行 | **19,940 行** |

### 修复（第二轮）

- `cmd_naming` 两个查表方向大小写不对称：`AliasOf` 用 `find`（区分大小写）、`LegacyOf`
  先 `Lower` —— 表现为"`nav.back` 认、`NavBack` 认、`navback` 不认"。`naming_test.cpp`
  的断言一直失败。两个方向统一改 `_wcsicmp`；局部 `Lower()`（第三个大小写归一实现）随之
  删除，同类工具统一在 `str::Lower`。
- `check_ai_meta.py` 四张硬编码旧名的表（`INDIRECT_ARGS` / `KNOWN_NO_TARGET` / `COVERS` /
  `UI_ONLY`）在主名迁移后全部失配，一次误报 19 条。改为点分名（`c['name']` 现在就是这个值）。

---

## [0.2.1] - 2026-11-20

**操作契约结构化 + AI 操作接口 + 便携存储 + 仓库独立。**
这一版不是加功能，而是把"同一件事有多份实现"逐处收口：命令参数、效果、前置条件、返回值、
命名、导航语义、历史栈、窗口类注册、日志、剪贴板、字形码位、tooltip 都各自收敛到一个实现，
再用 3 个静态检查脚本 + 20 个单测（354 项断言）把回退挡住。

### 变更（仓库独立：从 Windhawk 主仓脱钩）

- **`E:\ExBarBrowser` 成为独立 git 仓库**（分支 `main`）。此前它靠 `D:\Windhawk` 根
  `.gitignore` §8 的白名单放行，寄生在主仓里：改一个分片要在别人的仓库里 `git add ExBarBrowser/`，
  提交历史与 ExBar mod 的 7.7 万行混在一条时间线上，两边互不相关的改动互相干扰。
  迁到 E 盘后工作区已不在主仓路径下（主仓 `git status` 因此变成 95 条 `D`），继续寄生只会留下
  一堆"已删除"的幻影，因此改为独立仓库。主仓里那 95 条删除由操作者自行决定何时清理 ——
  本仓库不引用主仓，主仓也不引用本仓库，先清后清都不影响这边。
- **入库范围不变，但语义变了**：以前"不入库"是为了不给主仓添产物噪音；现在"不入库"意味着
  **克隆即可构建、构建不产生提交**。`data/` 整目录仍不入库（书签/键位/窗口几何是个人数据），
  新克隆首次启动会走内置默认表 + 空书签树，`settings.json` 首次保存时自动创建。
- **`src/keymap.gen.h` 明确入库**：它是 `data/keymap.json` 的生成物，但也是"配置文件缺失时"
  的兜底默认表 —— 不入库会让新克隆在无 `data/` 时没有任何热键。`check_commands.py` 负责
  保证两者一致，所以"生成物入库"在这里不是坏味道。
- **搬目录踩到 PCH 的绝对路径**：`obj/common.h.gch` 是 clang AST 二进制，里面记着
  `D:/Windhawk/ExBarBrowser/src/common.h`；搬完直接 `bash build.sh` 会得到 57 个 TU 全报
  `malformed or corrupted AST file`。`build_fast.py` 不会自动察觉路径变更，需要一次
  `--clean`（冷 PCH 实测 29.8s，其中 PCH 14.1s；PCH 命中后的全量重建是 10.5s）。已写进 README §3.1 与 §8 坑表。
- **README / CHANGELOG 全量重核对**：现状表从"10,200 行 / 127 条命令 / 94 已实现 / exe 12 MB"
  更新为实测值（当时 18,388 行、137 条、108 已实现、86 对 AI 暴露、18.9 MB / strip 1.3 MB），
  §3.4 版本控制整节重写，§5 架构图补 `ai_api`/`ai_server`/`shell_input`/`nav::History`/
  基础设施收编，§6 目录树补全 20 个新模块，§8 坑表补两条（PCH 路径、生成头不同步）。

### 变更（导航收口为组件，全局复用）

导航以前是"同一件事写多遍"：路径语义有两套实现（`common.h` 无长度限制、
`NavModel` 基于 `PathRemoveFileSpecW` + MAX_PATH），历史栈的压栈动作在
`NavigateActive` / `OnTabNav` / `OnTabNavFailed` / `GotoHistory` 四个函数里
各手写一遍（`hist.resize(idx+1)` + `push_back` + `idx = size()-1` 共 4 份），
"此电脑"的 CLSID 硬编码在 3 个文件里。这类重复的代价不是啰嗦，而是**症状远离
现场**：用户看到"后退要按两次"，根因在别处的 `push_back`。

- **新增 `src/nav_path.h` —— 路径语义唯一实现**（纯字符串、无 MAX_PATH 限制）：
  `IsNamespace` / `IsDriveRoot` / `Normalize` / `ParentOf` / `Leaf` / `UpTarget` /
  `CanNavigateUp` / `kThisPc`。`common.h` 的旧名字保留为**转发**，不再有第二份实现。
- **新增 `src/nav_history.{h,cpp}` —— 历史栈事务组件**（每个标签一份）：
  `Request` / `Goto` / `Complete` / `Fail` / `ClearKeepingHere` / `Restore`，
  压栈动作只有 `History::Push` 一处。`App::Tab` 里原来的
  `hist/idx/pending/pendingPushed/pendingPrevIdx` 五个字段收敛成一个 `nav::History`。
- **删除 `NavModel::ParentOf` / `LeafOfPath` 的实现**，改为转发 `nav::`。
  顺带修掉一个静默缺陷：旧实现超过 MAX_PATH 直接返回空，导致**超长路径的地址栏
  Tab 补全失效**；现在走无长度限制的实现（已有单测覆盖）。
- **删除 `common.h::CollapseHistoryToCurrent`**，规则并入 `History::ClearKeepingHere`。
- **导航按钮改走 `CommandBus`**：工具栏的 ←、→、↑、刷新以前直接调 `GoBack()` /
  `GoUp()` 等，绕过命令总线（因而绕过了前置检查与审计）—— 这正是上一版
  "按钮能点、双击没反应"的分裂来源。现在按钮与双击/热键/序列走同一条命令。
- **按钮灰显读同一个谓词**：`NavBar::SetPath` 用 `nav::CanNavigateUp`，
  与命令前置检查 `Req::CanGoUp` 同源，"灰着"必然等于"点不动"。
- **命令拒绝原因对外可见**：`CommandBus` 新增 `lastBlockReason()`。
  以前 `Dispatch` 算出了原因却只写日志与审计，AI/脚本侧只能看到笼统的
  "命令执行失败"；现在返回真实原因（如"已经是顶层（此电脑或网络根）"）。

### 变更（存储改为便携模式：数据放 exe\data）

- **配置目录从 `%APPDATA%\ExBar` 迁到 `<exe>\data`**：整个程序目录可整体拷走，
  不写注册表、不写用户目录，便于随身/绿色部署。迁过去的是**真实配置**：
  `settings.json`、`bookmarks.html`、`keymap.json`、`mouse.json`、`audit.log`。
  书签同时不再与 ExBar mod 共用同一份 `bookmarks.html` —— 两边各写各的，
  避免互相覆盖。旧目录原样保留不删（可回退、可对照）。
  代价：exe 放在 `Program Files` 这类只读目录时配置写不进去，需要把整个目录放到
  用户可写位置；`Config::Writable()` 提供只读检测，写入失败只记一次日志不刷屏。
- **`browser.json` 改名 `settings.json`**：与前两项一致 —— `browser-` 前缀原本是为
  避开 ExBar mod 的同名文件，现在数据目录独立，前缀没有存在意义。
  代码里 `Config::SettingsPath()` 与相关日志一并改为 `settings.json`；
  首次启动若只有旧的 `browser.json`，重命名即可（两者 Schema 完全相同）。
- **去掉 `browser-` 前缀，键位/鼠标规则合并为单文件**：`browser-keymap.json` →
  `keymap.json`、`browser-mouse.json` → `mouse.json`。这个前缀是历史遗留：数据目录
  曾是 `%APPDATA%\ExBar`，与 ExBar mod 共用，而 `keymap.json` / `mouse.json` 正是
  mod 的文件名，不加前缀会互相覆盖。现在数据目录已与 mod 分离，撞名风险消失；
  同时取消"种子 + 用户覆盖"的双文件机制，只保留一套真实配置，用户直接编辑。
- **不再有任何种子/默认值副本**：`Config::SeedAssets()` 整个删除，仓库里也不再带
  示例 `bookmarks.html`。书签文件缺失时走空书签分支（`BookmarkStore::Load`
  返回 false 即空树），不需要种子兜底。
- **`data/` 全部不入库**：这些文件因人而异（书签、最近访问、窗口几何、键位习惯），
  已在 `.gitignore` 中排除，并用 `git rm --cached` 从索引中解除跟踪（磁盘文件保留）。

### 修复（持久化缺口）

- **`closedTabs` 只写不读**：退出时保存了关闭历史，但启动时从未调用
  `TabSet::SetClosed`，导致 `Ctrl+Shift+T` 重开标签在重启后永远是空的。
  现在 `App::Run` 启动即恢复。实测：注入两条关闭历史 → 启动 → 退出，条目完整保留。
- **视图模式不持久化**：`viewMode_` 只活在内存里，每次重启都回到"小图标"。
  现在 `Settings::viewMode` 随 `ApplyViewMode` 落盘，新标签启动时套用。
- **排序/分组不持久化**：补 `Settings::sortMode` / `sortAscending` / `groupBy`，
  通过 `ViewSort` / `ViewGroup` 命令写回。
- **导航窗格/预览窗格显隐不持久化**：补 `Settings::showNavPane` / `showPreviewPane`。
  两个窗格的偏好必须在**首次导航前**就位（`EBO_SHOWFRAMES` 与
  `IExplorerPaneVisibility` 只在建窗格时被消费），因此 `ShellView::Create`
  一开始就把配置读进 `navPaneWanted_` / `g_previewPaneWanted`，避免"第一次打开时
  窗格还在，切一次目录才生效"。
- **最近访问写盘时机**：`AddRecent` 只改内存、全程序仅"清除历史"处 `Save()`，
  正常浏览产生的记录要等用户改别的设置或正常退出才落盘，强杀即丢。
  现在 `AddRecent` 置脏位（`Config::dirty()`），退出时统一写盘，
  浏览过程中不写文件；原地导航到同一路径不再无谓置脏。

### 修复（"上一级"在盘符根失效）

- **盘符根双击文件列表空白无反应**：`C:\`、`D:\` 这类盘符根上，双击空白本该回到
  "此电脑"（与资源管理器一致），实际毫无反应。根因不是漏写分支，而是**同一件事有
  三套判定**：`App::GoUp()` 里写了"盘根 → 此电脑"的分支，但命令总线在
  `Req::CanGoUp` 前置检查处就把它拒了 —— 状态探测（`app.cpp`）把盘根算成
  "不能向上"。于是同一个动作两条路径行为分裂：工具栏的"上一级"按钮能跳（它绕过
  命令总线直调 `GoUp()`），双击空白跳不动（走命令总线被拦）；
  `NavModel::IsRoot()` 又是第三套口径（盘根不算 root），按钮因此还是亮的。
  现在判定收敛成 `common.h` 的 `CanNavigateUp()`，前置检查、`GoUp()` 执行、
  导航栏灰显三处共用。审计日志里原来是一串
  `NavUp (blocked: 已经是顶层（盘根或网络根）) ok=0`，现为 `ok=1`。
- **顺带修掉 `ParentOf()` 的 UNC 判定**：`\\server\share\dir` 的父级是
  `\\server\share`（有效上一级），原代码 `cut <= second` 把它一起判成"没有上级"，
  网络共享下任意子目录的"上一级"都静默失效。改为 `cut < second`。
- 拒绝原因文案同步为"已经是顶层（此电脑或网络根）"—— 原来写"盘根"，
  而现在盘根是**能**向上的，旧文案会误导。

### 移除

- **两个死配置**：`Settings::layout` 与 `Settings::mouseRules` 会读写但全项目无
  使用点（鼠标规则实际只从 `mouse.json` 读）。字段与 `Config::Load/Save`
  中对应行一并删除；旧配置文件里的这两个键在下次保存时自然消失。

### 安全

- **AI 令牌只在运行时有效**：`ai::StopServer()` 此前不清理发现文件，
  `aiEnabled` 关掉后 `ai-token.txt` / `ai-pipe.txt` 仍留在磁盘上，
  看起来像通道还开着。现在停服即删除这两个文件。

### 变更（命令契约结构化，第 1–10 步）

- **第 10 步：偿清"只能靠选中"的债务。** 债务表原本只登记 4 条，实际
  "依赖选中且已实现"的有 15 条 —— 大部分从没被登记。补齐后新增 5 条确定式命令：

   `FsOpenInNewTab {path}`、`FsOpenInNewWindow {path}`、
   `FsCopy {targets[]}`、`FsCut {targets[]}`、`FsProperties {target}`。

  三个值得记的点：
  ① **`FsCopy` 不产生副本** —— 它与 `Ctrl+C` 同义，只是"准备好等粘贴"，
  想真复制到某处要用 `FsCopyTo`。这两件事极易混淆，所以在注释、文档、
  别名（`fs.copy.toclip` vs `fs.copy`）三处都标了。
  ② 造 `IDataObject` 用 `CIDLData_CreateFromIDArray` 而非 `SHCreateDataObject`
  （后者被 `NTDDI_VERSION` 包着，某些配置下不可见）。两者都比自己拼 `CF_HDROP`
  好 —— 后者丢掉"来自哪个命名空间"，粘贴到某些目标会失败。
  ③ **一个路径解析不了就整批失败**。漏一个会造成"我复制了 5 个实际只复制了 3 个"
  而调用方以为全成了。

  另外：写这 5 个 handler 时我先把公共逻辑抽成 lambda，结果静态检查立刻报
  "参数没取用"——第 9 步踩过同一个坑。这次改成不抽公共函数、三个 handler 各写
  三行重复代码：**例外表越长检查能挡的问题越少，重复三行换覆盖值得**。这条写进
  注释了，免得后来人"优化"回去。

  顺带修掉检查器的一个 bug：`COVERS` 遍历外套了 `if old in KNOWN_NO_TARGET`，
  导致 `CmdRename`/`CmdDelete` 在覆盖表里却从不被打印 —— 覆盖情况看起来比实际更差。

  指标：命令 132 → 137，已实现 103 → 108，AI 可调用 81 → **86**，
  债务表未覆盖 3 → **2** 条。

### 变更（命令契约结构化，第 1–9 步）

- **第 9 步：补齐三处能力缺口。** 按"机器最缺 → 失败最不可解释 → 最实用查询"的
  优先级依次做，每项都先对照成熟项目的做法。

  **`FsOpen {path}`（新增）。** `CmdOpen` 依赖选中，机器无法表达"打开第 3 个文件"。
  语义与 `CmdOpen` 一致：文件夹在当前标签进入，文件交 shell。不加 `mode` 参数是
  参照 VS Code —— 它把打开拆成 `open`/`openWith`/`revealInExplorer` 三条独立命令，
  因为那是三种意图，各值得一个名字。路径不存在时直接失败而不弹系统框
  （确定式调用不该卡在没人看的对话框上）。

  **前置条件 16% → 30%。** 参照 VS Code 的 `when` 子句 + `enablement`：把"边界处
  会静默失败"的条件声明出来。新增 4 个条件 —— `CanMoveTabLeft`/`CanMoveTabRight`
  （`TabMoveLeft` 有 `toIndex<0→0` 钳制，已在最左时按了没反应）、`HasClosedTabs`、
  `HasMultipleTabs`、`HasSelectionSingle`。共给 18 条命令补了条件，其中 `CmdRename`
  原先完全没声明（无选中时会弹框）。

  **`OpTopN` / `OpDiskUsage`（新增 `src/disk_scan.{h,cpp}`）。** 三条硬约束：
  ① 必须能中途停（给 deadline，超时带已有结果返回 —— 几万文件的递归卡在 UI 线程
  就是假死）；② 必须报告完整性（`truncated`/`timedOut`/`elapsedMs`/`scannedFiles`，
  只给数字而不说"没算完"会被当成真值）；③ 不跟随重解析点（跟 junction 无限递归，
  且把链接目标大小算到链接处，数字是错的）。

  两条命令而不是一条带 mode：`OpTopN` 答"什么最占空间"（看 `entries`），
  `OpDiskUsage` 答"这目录占多少"（看 `totalBytes`）—— Unix 上 `du` 与 `ncdu`
  也是两个工具。`totalBytes` 统计截断前的全部，否则"总量 < 各子项之和"会让人困惑。

  新增 `tests/disk_scan_test.cpp` 25 项；`precondition_test` 从 26 → 42 项。
  合计 257 项断言全通过。

### 变更（命令契约结构化，第 1–8 步）

按 `docs/OPERATION-MODEL.md` §7 的顺序推进"让每个操作同时对人可用、对机器可调"，
七步机制 + 第八步内容补全已落地，共 219 项断言 + 14 组静态检查守住。

- **第 8 步：内容补全（AI 可暴露面 45 → 78）。** 七步做完的是**机制**，
  不等于每条命令都填了内容。这一步按三条判据逐条判断该不该暴露：
  ① 已实现（否则出现在 `tools/list` 里却调不动）；② 不依赖当前选中
  （机器无法指定"选哪个"，暴露出去就是"AI 以为删了 A，实际删了 B"）；
  ③ 不是纯人机 UI（弹菜单、开关面板、退出程序）。

  新暴露 33 条：`TabMove*`(4)、`GoTab1..9`、`CloseLeftOne`/`CloseRightOne`、
  `PathCopyDir`/`PathCopyTab`、`CmdSelectAll`、`FileFocusTop/Bottom`、
  `CmdNewFolder`/`CmdNewTextFile`/`CmdPaste`、`CycleHiddenSeg`、`FocusAddress`、
  `BmkAddCurrent`、`SessionSave/Restore`、`NewWindow`、`OpenTerminal(Admin)`。

  仍不暴露 22 条，每条有理由，已由静态检查逐条标注：
  17 条依赖选中（`CmdDelete`/`CmdRename`/`PathCopy*` 等 —— 其中 4 条有 `Fs*` 确定式版本
  可替代，其余是真实缺口）；5 条纯人机 UI（`QuitExplorer`/`FolderOptions`/`MapDrive`/
  `UnmapDrive`/`CommandPalette`/`HotkeyManager`）。

  **顺带修掉一个标注错误**：`PathCopyDir` / `PathCopyTab` 早先和 `PathCopy` 一起标了
  `needsSelection=true`，但它们的实现用 `ActivePath()` / `tabSet_`，**根本不看选中**。
  标注与行为不符，把这两条错误地排除在机器能力之外。这提醒 `needsSelection` 是人写的
  标注，必须对照实现核实。

  新增静态检查第 14 项：暴露的命令必须"已实现 + 不依赖选中 + 非纯 UI"，
  违反即报错。

- **第 7 步：对外暴露（MCP 兼容）。** `ai_server` 加 JSON-RPC 2.0 层：
  `initialize` / `tools/list` / `tools/call` / `exbar/state`。
  上一轮的自定义格式（`{"token":..,"method":..}`）对 MCP 客户端不可用 ——
  每个客户端都得自己写转换层就失去了"开箱可连"的意义。现在靠**有无 `jsonrpc`
  字段**区分两种协议，既有本地脚本零改动。

  三处关键决定：
  - **错误分两类**（MCP 硬要求）：请求不合法 → JSON-RPC `error` 带 `code`；
    工具执行失败 → `result.isError=true`。混在一起客户端分不清"我调错了"
    还是"环境不满足"，只能盲重试或盲放弃。
  - **令牌检查在方法分派之前**：否则能靠"返回 method-not-found 还是 bad-token"
    枚举有哪些方法 —— 能力面泄露。单测断言"未知方法与已知方法的未授权响应完全一致"。
  - **`description` 从结构化字段生成，不手写**：手写的会与 `annotations` 脱节
    （改了 `destructive` 忘了改描述），两者不一致时模型会做错判断。

  验证用了**真实管道往返**（`tests/pipe_e2e_test.cpp` 35 项）：进程内起服务、
  自己当客户端连接、走完 initialize → tools/list → 成功调用 → 缺参数 →
  未知工具 → 错误令牌 → 停止清理。47 个工具全部带 name/description/
  inputSchema/annotations。

- **第 6 步：命名空间化。** 新增 `src/cmd_naming.{h,cpp}`：131 条命令的旧名 ↔ 点分名
  双向映射（`NavBack` ↔ `nav.back`、`CmdDelete` ↔ `file.delete`、`FsDelete` ↔ `fs.delete`）。
  `FindCommand` 两个方向都认，所以既有 keymap / 序列 / 鼠标规则**零改动**，
  外部调用方可以开始用稳定名字。

  命名规则：全小写、点分、两段。域取自 `CommandDef::group` 的小写 —— 不另造分类，
  否则两套"分组"迟早不一致。单测里有一条断言"每个点分名的域前缀必须与该命令的
  group 对应"，这条在写测试时立刻抓出了 `AiTogglePanel` 被错标成 `tool.*`
  （它是 `group=Window`）。

  两处有意为之：**大小写不敏感**（与旧名的 `_wcsicmp` 行为一致，避免"`NavBack`
  能用但 `NAV.BACK` 不行"的半吊子行为）；**确定式命令用 `fs.*` 而非 `file.*`**
  （"对指定路径操作"与"对当前选中操作"不是同一件事，同名域会让人以为只是参数不同）。

  `tools/list` 现在同时返回规范名 `name` 与 `legacyName`：新客户端只学一种，
  既有脚本/审计日志用另一个对照。

  判据 `tests/naming_test.cpp` 31 项。

- **第 5 步：返回值。** `CommandBus` 新增一条**独立的**"带结果"注册通道 ——
  `RegisterWithResult` / `InvokeResult` / `HasResult`，`CommandDef` 加 `outputSchema`。
  为什么要独立通道而不是让 131 条 handler 全返回 `json::Value`：绝大多数命令是
  "做了/没做成"，让它们都构造 JSON 是纯负担，还容易让写操作假装有返回值。

  头两条落地的是 `OpListDir`（列出当前目录）和 `OpStat`（查单项属性），
  底层新增 `shellops::ListView` / `StatItem`。它们查的是**当前视图**而不是按路径
  扫盘 —— 与用户看到的完全一致（含虚拟命名空间、搜索结果、过滤状态），
  也不需要重新判断访问权限。

  一个容易忽略的细节：无返回值的命令必须给**空对象**而不是留旧数据。
  调用方复用同一个 `json::Value` 时若不清空会读到上一次的结果 —— 那比"没有结果"
  更危险，因为它看起来像真的。单测里专门断言了这件事。

  判据 `tests/result_test.cpp` 20 项。

- **第 4 步：交互与确定分离。** 新增四条确定式命令 —— `FsRename`（`{target,newName}`）、
  `FsDelete`（`{targets[]}`）、`FsCopyTo` / `FsMoveTo`（`{targets[],to}`）：
  目标由参数点名，绝不弹框、绝不读"用户碰巧选中了什么"。
  旧的 `CmdRename` / `CmdDelete` / `CopyToFolder` / `MoveToFolder` **保持原样**
  —— 它们的语义里"弹框问用户"是不可分割的一部分（`F2` 的体验就是弹输入框），
  加参数会让"传了参数就不弹框"变成隐式分叉。拆开后人机各走入口，逻辑共用。

  实现上**不是**另写一份按路径的文件操作，而是 `shell_ops::SelectByPath` 先选中
  目标、再复用既有的 Selection 实现 —— 否则回收站/冲突/权限/撤销这些语义会有
  第二份实现，迟早与既有行为分叉（架构规则 R6）。

  `shell_ops` 新增：`SelectByPath` / `SelectNone` / `SelectedSinglePath` / `RenameTo`。
  其中 `RenameTo` 用 `IFileOperation` + `FOF_SILENT` + `FOF_NOERRORUI`：
  确定式调用遇到冲突时**返回失败**而不是弹框 —— 明确的失败比没人看的对话框更可预期。

  判据 `tests/fs_ops_test.cpp` 44 项。债务消化 4/7，剩余 `CmdCut`（剪切改的是剪贴板，
  语义特殊）、`BatchRename`（等 PowerRename）、`PathCopyLink`（低优先级）已登记。

- **第 1 步：参数对象化。** `CommandBus::Handler` 从
  `function<bool(const std::wstring&)>` 改为 `function<bool(const Args&)>`
  —— 参数从"一个需要自己拼的字符串"变成具名对象，一次性解决多参数、可选参数、
  类型校验、自动生成 UI、向后兼容加参数五件事。
  旧的单值规格（keymap/序列/鼠标里的 `"ViewMode:3"`）经 `ArgsFromLegacyArg`
  **从 `argsSchema` 推导键名**，因此删掉了 `ai_api` 里那张 `kArgKeys` 手工映射表
  —— 那张表就是"参数是字符串"这个缺陷的症状。
  判据 `tests/args_test.cpp` 22 项：新旧两条路径对同一操作结果一致。

- **第 2 步：效果四维化。** `Risk{Read,Write,Danger}` 拆成
  `Effects{readOnly, destructive, idempotent, openWorld, undoable, scope}`，
  127 条命令逐条按真实行为重标（只读 14 / 破坏性 21 / 跨进程 22 / 幂等 41）。
  单轴表达不了"幂等的写"与"不可逆的写"的区别 —— `NavRefresh` 和 `CmdPaste`
  原来同归 `Write`，但前者可安全重试、后者不可撤销。
  `tools/list` 现在输出 MCP 风格的 `annotations`（四个 hint + scope）。

- **第 3 步：前置条件。** 新增声明式 `Requirements` + `CanExecute()` + `ReqReason()`，
  21 条常用命令已标注。`Invoke` 现在先过前置检查，拒绝时审计记
  `(blocked: 原因)`。这修掉了"无历史时点后退毫无反应"这类哑失败 ——
  以前 handler 静默 `return false`，界面和日志都没有线索。
  判据 `tests/precondition_test.cpp` 26 项。

顺带修掉两个静默缺陷：

- **参数被丢弃**：`TabNew` 声明了 `path` 参数但 handler 从没读过。新增的静态检查
  （`check_ai_meta.py` 第 3 项）会比较"schema 声明的键"与"handler 实际取用的键"。
- **空 schema 的不安全默认**：122 条原为 `"{}"`，按 MCP 规范语义是"接受任意参数"，
  已全部改为 `{"type":"object","additionalProperties":false}`。

已登记的债务（`check_ai_meta.py` 的 `KNOWN_NO_TARGET`）：这 6 条破坏性命令仍只能作用于
"当前选中项"，本身没有目标参数。第 4 步已为其中 4 条提供确定式版本（机器改用新入口），
剩余 `CmdCut` / `BatchRename` / `PathCopyLink` 待做 —— 它们的共同点是"改动的不只是文件"：
剪切改剪贴板、批量重命名要等 PowerRename、快捷方式写入走另一套 shell 接口。

### 新增

- **AI 操作接口（人机同源）**：给命令表补上 `risk` / `argsSchema` / `aiExposed` /
  `needsSelection` 元数据（架构规则 R18），并新增 `ai_api`（工具投影、参数校验、派发）
  与 `ai_server`（命名管道 + 令牌 + 请求泵）。AI 的所有动作都走同一条 `CommandBus`，
  审计记 `actor=ai`，不存在"AI 专用"的第二份实现。对外 41 条工具，集中在导航、
  标签切换、视图调整、书签栏显隐。

  三条设计取舍：
  - **默认关闭**：需 `settings.json` 里 `aiEnabled=true` 才监听管道；
  - **依赖选中项的命令不暴露**（`CmdDelete`/`CmdRename`/`CmdPaste`…）：AI 无法用参数
    指定目标，暴露出去会作用在"用户碰巧选中的"对象上。要支持这类操作需新增能用参数
    指定目标的语义命令，而不是把 UI 命令硬暴露；
  - **危险命令双门**：`aiAllowDanger`（设置）+ `approve=true`（调用方）同时满足才执行。

  配套 `tools/check_ai_meta.py` 静态对账（暴露/参数/选中项三方一致），
  `tests/ai_api_test.cpp`（34 项）+ `tests/ai_server_test.cpp`（8 项）单测，
  说明见 [docs/AI-INTERFACE.md](docs/AI-INTERFACE.md)。

### 变更

- **分隔符行高 24px → 2px**：地址栏下拉的分组分隔线原来占满一整行（120 DPI 下 30px），
  看起来像个空条目。现在分隔线只占 2px：`PopupList` 从 `LBS_OWNERDRAWFIXED` 改为
  `LBS_OWNERDRAWVARIABLE`，逐行设高（`LB_SETITEMHEIGHT`）+ `WM_MEASUREITEM`。
- **地址栏下拉新增"清除历史记录"**：位于列表底部（前面加分隔线），清空当前标签的
  后退/前进栈与"最近访问"列表，保留当前目录作为唯一历史条且不触发导航。
  同时修掉方向键会停在分隔行上导致"按回车没反应"的问题。

### 修复

- **双击文件列表空白区没有返回上一级**：规则引擎、序列键和命令总线早已具备，但 Shell
  文件视图从未把命中的目标喂给 `InputRouter`——`mouse.json` 里也没有 `empty` 规则。现在
  新增 `src/shell_input.{h,cpp}`：Win10 走 `SysListView32` hit-test，Win11 走 UIA
  `ElementFromPoint` + 有限祖先上溯，把 `empty/file/folder/group/column/tree` 翻译成
  `MouseTarget`，经 `MouseSink` 汇入现有规则引擎；`SHELLDLL_DefView` / `SysListView32`
  / 导航树在每次视图重建后自动重装子类。默认规则补上 `empty × left × double → NavUp`。
  实测从 `docs` 双击空白回到父目录，审计为 `mouse NavUp ok=1`。
- **冷启动第一次双击空白仍会偶发失效**：Win11 DirectUI 的点击只经 `WM_PARENTNOTIFY`
  上抛，之前这条路径把目标直接写成 `unknown`，两次点击都进不了规则；同时 UIA 惰性启动
  的单次失败会立刻进入冷却，首击目标拿不到。现在 `WM_PARENTNOTIFY` 的 `DOWN` 也做
  UIA/ListView 命中测试（`UP` 沿用 pending 目标），并通过 `WM_APP_MOUSE_*_NOUP` 接上
  `InputRouter` 的 `expectUp=false` 契约，派发后立刻释放 claim；UIA 失败累计 3 次探测才冷却，
  `InputRouter` 为首击 `unknown` 保留匿名 pending、由第二击补上目标。冷启动冷进程复测：
  `before=docs → after=ExBarBrowser`，审计 `mouse NavUp ok=1`。
- **面包屑分隔符列成了"同级目录"**：点 `D:\Windhawk` 右侧的 `›` 弹出的是 `D:\` 下的同级
  文件夹（Downloads、Program Files…），而不是 `D:\Windhawk` 自己的内容。原因是
  `NavBar::ShowSiblingMenu` 先对分段路径取 `ParentOf()` 再枚举，得到的是"当前级的兄弟"。
  分隔符语义上归属于它**左边**那一级，改为直接枚举 `parts[partIndex].path` 的子文件夹，
  与 mod `ShowCrumbMenu` 的 `EnumChildFoldersAt(crumbPidls[partIndex])` 对齐；函数随之
  更名为 `ShowChildMenu`。副作用是盘根分段（`D:\`）的 `›` 现在也能列出该盘顶层文件夹。
- **隐藏预览窗格后一换目录又自动打开**：以前读 `FOLDERFLAGS` 的 `0x400` 当"预览窗格"
  位——那是 `FWF_NOSCROLL`，几乎所有视图都置位，于是每次都判成"已打开"，导航时又把窗格
  打开。`FOLDERFLAGS` 里根本没有"显示预览窗格"这一位；真正决定窗格是否创建的是宿主提供的
  `IExplorerPaneVisibility::GetPaneState`。现在 `ShellView` 通过 `IServiceProvider`
  进入 ExplorerBrowser 站点链并实现 `IExplorerPaneVisibility`：关闭态返回
  `EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE`；"是否已显示"改以窗格宿主窗口
  （`Shell Preview Extension Host`）的真实可见宽度为准采样，并在 `OnNavigationPending`
  里采样，覆盖双击文件夹等 shell 内部导航。`TogglePreviewPane`（`Ctrl+Alt+P`）与
  shell 自带关闭按钮都形成持续意图。
- **选中文件不显示高亮**：根因是**我们对 shell 内部窗口调了 `SetWindowTheme`**。
  `ApplyTheme`/`OnViewCreated` 会遍历视图子树，把 `ExplorerBrowserControl`、
  `SHELLDLL_DefView`、`DirectUIHWND` 等一律设主题；DirectUI（Win11 文件列表）
  因此改用另一套 `SubAppName` 样式字典重新解析 UIFILE，之后选中项不再绘制高亮底色
  ——但选中集本身是完好的（状态栏"已选 N"正常），所以看起来像"焦点问题"，实际是绘制被改坏。
  现在只对 `ExplorerBrowserControl` 这一层设主题（与同类宿主项目做法一致），交给 shell
  自己把深色下传给内部 DirectUI；内部窗口一概不碰，只单独深色化 `ScrollBar` 小控件。
  另移除了对 DirectUI 树反复广播 `WM_THEMECHANGED` 的 `PokeTheme`。
  依据 `tools/probe_highlight.cpp`（离屏、不抢焦点、不模拟鼠标，选中前后像素差分判据）：

  | 动作 | changed | peakCount | 结论 |
  |---|---|---|---|
  | 基线（不改主题） | 4784 | 174 | 有高亮 |
  | 只对顶层 `ExplorerBrowserControl` | 4784 | 174 | 有高亮 |
  | 对全子树 `SetWindowTheme` | 1224 | 14 | **无高亮** |
  | 只给 `ScrollBar` 设主题 | 4784 | 174 | 有高亮 |
  | 只调 `AllowDarkForWindow` | 4784 | 174 | 有高亮 |
  | 只发 `WM_THEMECHANGED` | 4784 | 174 | 有高亮 |
  | 先画坏再 `SetWindowTheme(NULL,NULL)` | 4784 | 174 | 可恢复 |
- **选中项不高亮时键盘也失灵（伴随问题）**：焦点此前只给到视图容器
  `ExBarBrowser_TabView`，从未交给 `ExplorerBrowserControl`，框架因此不知道本视图被激活，
  type-ahead/方向键/右键上下文都受影响。现在容器收到 `WM_SETFOCUS` 时先把焦点交给
  `ExplorerBrowserControl`（IExplorerBrowser 自己的宿主窗口，它再走
  `OnViewWindowActive → UIActivate`），随后补一次 `UIActivate(SVUIA_ACTIVATE_FOCUS)`。
- **历史栈每步压两条 → 后退要按两次**：以前 `NavigateActive` 先压"请求路径"，Shell 完成导航
  后再压"真实路径"（重定向 `文档 → OneDrive\文档`、尾斜杠、正斜杠规范化都会触发）。
  现在 `Tab::pending` 记录本次请求，完成时**就地校正** `hist[idx]`（多级重定向逐跳收敛），
  视图内部双击才压新条；后退/前进统一走 `GotoHistory`，导航失败会把游标退回。
- **复制/剪切文件、拖放全部失效**：`WinMain` 只调了 `CoInitializeEx`，而 `OleSetClipboard` /
  `OleFlushClipboard` 和 shell 视图的 `RegisterDragDrop` 都要 OLE。改为 `OleInitialize` /
  `OleUninitialize`。
- **Ctrl+Shift+1..8 全部变成"超大图标"**：8 个键绑的是同一个 `ViewMode` 且没有参数，
  `_wtoi("")==0`。命令规格现在支持 `Name:arg`（`ViewMode:3`），`CommandBus::InvokeByName`
  统一拆分，键盘/序列/鼠标/面板同一条路径。
- **9 条序列 `g 1`…`g 9` 是空动作**：keymap 绑了 `GoTab1..9`，命令目录里却没有这些 id，
  绑定阶段被静默跳过。补齐 9 条命令 + 处理器；目录同时对账 mod 的 127 条能力（新增
  `CopyToFolder` / `MoveToFolder` 实现，`TabMergeAll` / `BatchRename` / `SelectByPattern` 标注未实现）。
- **书签栏气泡悬停读到已释放内存**：`TTM_ADDTOOLW` 只保存 `lpszText` 指针，原代码传的是
  循环内的局部 `std::wstring::c_str()`。标签栏/状态栏同样问题一并改为持有副本。
- **新建文件夹第二次必失败**：`IFileOperation::NewItem` 少了 `FOF_RENAMEONCOLLISION`
  （explorer 默认自动改名 "新建文件夹 (2)"）。
- **状态栏 800ms 全量枚举**：文件夹/文件分类按 `(path,total)` 缓存（`SplitCache`），
  大目录不再每 800ms 做一遍 O(n) 的 COM 遍历。
- **地址栏/书签栏/标签栏图标不识别特殊命名空间**：三处此前各自裸调 `SHGetFileInfoW`
  （且对 `::{GUID}` 还加了 `SHGFI_USEFILEATTRIBUTES`），此电脑、回收站、网络、云存储/NSE
  扩展一律退成通用文件夹图标。新增 `src/icon_resolve.{h,cpp}`（移植 exbar.cpp 的
  `GetPathIcon` 全链路）：友好名→解析串映射、虚拟命名空间转 PIDL 取真实图标、真实路径先
  PIDL 解析（云盘等 NSE）、注册表 `CLSID\DefaultIcon` 兜底、stock 文件夹保底。四处调用点
  统一为 `ico::Resolve`：`nav_bar.cpp`(lead 图标)、`bookmark_bar.cpp`(IconFor)、
  `nav_bar_actions.cpp`(IconFor→弹层)、`app.cpp`(标签栏)。
- **鼠标滚轮规则从未生效**：`WM_MOUSEWHEEL` 没接到 `InputRouter`，且标签栏传的是客户区坐标
  （契约是屏幕坐标）、水平滚轮方向反了。三处一起修。
- **复制路径在剪贴板失败时漏 HGLOBAL**：`CopyText` / 书签菜单改为失败即 `GlobalFree`。
- **链接失败**：`src/icon_resolve.cpp` 未列入 `build_fast.py` 的 `SOURCES`。

### 新增

- **输入端脚手架补齐 Shell 文件视图**：`src/shell_input.{h,cpp}` 把参考 `Exbar.cpp` 的目标
  模型落到独立进程：目标枚举扩出 `unknown/group/column/tree/search/toolbar/preview`，
  鼠标规则新增存在性早退（无对应按键规则不做 UIA 跨进程调用）、50ms 重复 DOWN 去重、
  系统双击矩形 + 防三击、左键第一击放行。UIA 单次失败即时重试，连续 3 次探测失败才
  500ms 冷却，避免第一次点击就丢规则。键盘组合键、序列键、快捷键管理器、命令总线仍保持单一入口。
- **纳入版本控制**：ExBarBrowser 全部源码、脚本、文档与构建配置纳入 `D:\Windhawk` 主仓 git
  （根 `.gitignore` §8 放行、本目录 `.gitignore` 逐项排除产物），首次提交 95 个文本文件。
  `*.exe` / `*.obj` / `*.d` / `*.gch` / `*.log` / `shot*.png` / `out*.png` 与 `*.bak` 类
  编辑备份一律不入库；提交沿用主仓约定式提交，见 [README](README.md) §3.4 与
  [CONTRIBUTING](CONTRIBUTING.md) §2.3。

  *（同日晚工作区迁到 `E:\ExBarBrowser` 后已改为独立仓库，见本版首节；此条保留为当时的事实记录。）*
- **复制到 / 移动到文件夹**（`CopyToFolder` / `MoveToFolder`）：`IFileOpenDialog(FOS_PICKFOLDERS)`
  + `IFileOperation::CopyItems/MoveItems`，撤销与冲突交给系统。
- **命令目录审计**：`tools/check_commands.py`（静态：表 / keymap / 处理器三方一致）与
  `tests/commands_test.cpp`（运行期：keymap 引用的命令必须存在于目录）。
- **应用图标**：自绘「蓝色圆角底板 + 白色文件夹 + 双标签页」图标（品牌蓝 `#4C9FFF`），
  经 `windres` 编进 exe 的 `.rsrc`（`RT_GROUP_ICON` + 8 档 `RT_ICON`：16–256px）。
  标题栏/任务栏/Alt-Tab/资源管理器统一生效；`tools/gen_icon.py` 可再生成，`resources/app.rc`
  声明资源，`src/app.cpp` 的 `LoadAppIcon()` 从资源加载并 `WM_SETICON` 设置大小两档。
  `build_fast.py` 在 PCH 后增量编译资源对象（`app.res.obj`）并参与链接。

### 变更

- **默认键位表改成"数据 → 代码"生成**（消除第二副本）：`src/keymap.cpp` 里 70 条
  `kDefaultHotkeys[]` + 38 条 `kDefaultSeqs[]` 是从 `data/keymap.json` 手抄的兜底表，
  改默认键要同时改两处、漏一处就出现"配置文件读到新键、兜底还是旧键"的静默不一致。
  现在由新增的 `tools/gen_keymap.py` 生成 `src/keymap.gen.h`，`BuildDefault()` 只遍历生成表。
  `tools/check_commands.py` 增加一项对账：生成头与 json 不一致即 FAIL（已验证负向用例）。
  `data/keymap.json` 与 `../data/defaults/keymap.json` 内容同步（70 热键 + 38 序列）；
  `src/keymap.h` 的注释此前称两文件是"同一份资产"，实际是两份独立副本，已改正。
- **构建清单改自动扫描**：`build_fast.py` 的 `SOURCES` 从 45 行手写列表改为 `src/*.cpp`
  自动扫描 + `EXCLUDE_PATTERNS` 排除表。多人/多会话并行时"新增 .cpp 就要在同一段字符串里
  插一行"是必冲突改动，且冲突解决不当时会静默漏文件（直到链接期报未定义符号）。
  已验证：并发会话新增 `src/shell_input.cpp` 后被自动收录，构建脚本零改动。
- **构建产物可隔离**：新增 `--objdir <dir>`，obj/pch/资源对象与该会话的暂存 exe 一起进
  独立目录（暂存名 `ExBarBrowser.<objdir>.exe`）。此前两个会话共用 `obj/` 和
  `ExBarBrowser.new.exe`：一方重建 PCH、另一方正拿旧 PCH 编译；或一方刚链接完就被另一方覆盖。
- **地址栏分隔符间距收紧**：每级 chevron 区宽 `kSep96` 32→20px、绘制内边距 `S(8)`→`S(3)`，
  前导/面包屑图标绘制槽 `kIcon96` 16→14px（对齐 `Exbar.cpp`），`›` 与前后面包屑文字的留白
  由约 16px 降到约 6px；点击热区仍保留 20px，箭头不缩小。
- **弹窗列表深色滚动条**：自绘弹层里的原生 `LISTBOX` 以前没套主题，深色背景上是一条亮灰宽条；
  现在深色用 `DarkMode_Explorer`、浅色用 `Explorer`，并给列表窗口 `AllowDarkForWindow`，
  滚动条颜色与 hover 展开宽度与 ExBar mod 一致。
- **标签栏高度 32 → 26**（逻辑像素，`theme_.S()` 后随 DPI 缩放）：新增常量
  `kTabBarHeightDp`（`tab_strip.h`），`App::Layout` 与 `TabStrip::Create` 共用一处数字，
  不再各写一个。四栏高度现状：标签 26 / 导航 30 / 书签 26 / 状态 24。
- **`tab_strip` 拆成五片**：`tab_strip.cpp`(建模/图标/生命周期) + `_input`(消息/悬停/命中) +
  `_layout`(宽度/溢出/tooltip) + `_paint`(绘制) + `_actions`(右键与 » 菜单)。纯移动。
- **`app_commands` 拆成四片**：核心(调度 + `WithView` + 菜单) / `_tabs`(标签·跳转·导航·路径) /
  `_files`(文件操作·视图) / `_misc`(窗口系统·外部·书签·会话)。`withView` 从函数内 lambda
  提升为 `App::WithView`（两个分片共用），`tools/check_commands.py` 改为审计
  `app_commands*.cpp` 全片，处理器计数仍为 94。
- **`app.cpp` 按职责拆成六片**（`ARCHITECTURE-RULES` R3：单文件 ≤300 行）：
  `app.cpp`(装配/生命周期 286 行) + `app_wnd`(消息路由) + `app_tabs`(标签生命周期) +
  `app_nav`(导航与历史栈) + `app_layout`(布局与主题) + `app_stats`(状态栏取数)。
  纯移动，无逻辑改动；`HomePath()` 改为 `App` 的静态成员，供各分片共用。
- **标签图标所有权下沉到 `TabStrip`**：`SetTabs` 现在直接吃 `core::TabRef` 列表，标题兜底、
  `ico::Resolve` 与 HICON 缓存（上限 256）都在组件内完成，`App` 不再持有 `tabIcons_`。
  缓存满时只回收"本次不展示"的 path —— 原实现在 `App` 里是整表 `clear()`，会把已经在
  `items_` 里、正要绘制的句柄一起销毁。

---

## [0.2.0] - 2026-11-19

**功能迁移**：把 ExBar mod 的七类自定义能力迁到独立进程（M0–M7）。宿主接线（DUI/XAML 子类化、
低级钩子、跨进程 RPC）不迁移，改为自有窗口 + 组件 + 命令管线。

### 新增

- **命令管线**（`commands.cpp`）：127 条命令目录（与 `data/capabilities.json` 同 id，90 条已实现 / 36 条占位），
  统一 `Invoke(id, arg, actor)`；`audit.log` 记录 actor（key/seq/mouse/ui/palette/ai）。
- **键盘快捷键**（`keymap.cpp` / `chord.cpp`）：直接读 mod 的 `keymap.json`（**70 条**），
  支持用户覆盖、冲突与系统保留键标记。
- **序列键**（`sequence.cpp` / `sequence_hint.cpp`）：**38 条**（`g g` / `g t` / `g u` …），
  前缀状态机 + 提示弹层 + 超时 + `Esc` 取消。
- **鼠标规则**（`mouse_rules.cpp` / `input_router.cpp`）：目标区 × 按钮 × 触发（单击/双击/长按/四向滚轮）
  × 修饰键；内置"双击书签栏空白 → 切换书签栏"。
- **标签栏**（`tab_strip.cpp` / `tab_set.cpp`）：拖拽排序、锁定、关闭历史、MRU、溢出滚动与 `»` 菜单、
  右键菜单、克隆、中键关闭。
- **导航栏**：地址自动补全、最近访问、视图排序/分组、隐藏文件三态、导航窗格开关。
- **书签栏**（`bookmark_bar.cpp` / `bookmarks.cpp`）：与 mod 共用 `bookmarks.html`
  （Netscape + `EXBAR_*`），拖拽添加、右键增删改、显隐。
- **状态栏**（`status_bar.cpp` / `view_stats.cpp`）：9 段（文件夹/文件/已选、修改日期、路径、
  选中容量、磁盘/容器、隐藏三态、类型），tooltip、段点击、右键显隐。
- **命令面板**（`app_commands.cpp`）与**快捷键管理器**（`hotkey_manager.cpp`）、
  **文本输入对话框**（`prompt.cpp`，书签重命名/新建分类）。
- **配置与状态**（`config.cpp` / `json.cpp` / `text_file.cpp`）：`%APPDATA%\ExBar\browser.json`，
  首次运行把 keymap/mouse/bookmarks 复制到用户目录。
- **会话**：保存 / 恢复命名标签组。

### 修复（2026-11-19 代码审查）

- `build.bat` 与 `build.sh` 的文件清单/库列表同步，并补 `-lpropsys`（PKEY 需要）。
- 标签栏作用域的 `Shift+←/→`（`TabMoveLeft/Right`）不再抢文件视图的 Shift 选择：
  只在"无 Ctrl/Alt 且焦点位于标签栏"时生效；编辑框内的 `Ctrl+C/V/X/A/Z`、方向键、Delete 让给 EDIT。
- `tf::WriteText` 改为写 `.tmp` + `MoveFileEx(REPLACE_EXISTING)` 原子替换，避免写一半截断配置/书签。
- `ShellView::Sink` 补 `virtual ~Sink()`（`Release()` 里 `delete this` 的基类析构要求）。
- 退出顺序修正：`WM_CLOSE` 只保存配置并销毁视图；字体与图标在子窗口销毁后的 `WM_DESTROY` 释放，
  避免销毁过程中重绘用到已删字体。
- 键位/鼠标用户文件改用 `keymap.json` / `mouse.json`，不再与 mod 的
  `%APPDATA%\ExBar\keymap.json`（`bindings[]` 格式）互相覆盖。
- 删除未使用的 `app_commands.cpp::ArgInt` 与 `tab_strip.cpp` 的重复 `DrawChevron`。
- 标签栏溢出时不再每次刷新把横向滚动位置重置到最左；切换/新建标签会自动把激活标签滚入可视区。
- 状态栏取数节流：导航变化立即取数；后台窗口每 5 拍、>800 项每 2 拍、>3000 项每 4 拍，
  避免每 800ms 全量枚举大目录。
- `audit.log` 增加轮转（每 200 条检查一次，超过 2 MB 重命名为 `audit.log.1`，只保留一份）。
- `tabNewPos` 生效：`right`（默认，当前标签右侧）/ `end`（末尾），新建、克隆、右键新建都遵守。
- 修复：地址栏右侧 `⌄` 下拉被长路径撑得比地址框宽 → `ShowPopupList` 新增 `exactWidth`，
  该菜单严格等于地址框宽度（文字用省略号收）；超出屏幕时保持**左边缘对齐**（缩窄而不是整体左移）。
- 修复：保存的窗口几何可能落在屏幕外（地址框右端跑出屏幕，弹层只能被拉回屏内，看起来不对齐）
  → 启动时把窗口整体夹回当前显示器工作区。

### 已知问题

- 审计与调试日志都是同步 `CreateFile/WriteFile`（已加 2 MB / 4 MB 轮转，高频操作仍有可测开销）。
- 多个文件超过 300 行（`app.cpp` / `app_commands.cpp` / `tab_strip.cpp` / `bookmark_bar.cpp`…）。
- 代码为平铺结构，与 [docs/ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md) 的分层规则不一致（待定）；
  `src/contract|input|pipeline` 是并行架构批次的占位，**未接入构建**，与 `src/commands.*` 并存待合并。
- 未实现：搜索（Everything）、工具集、预览/详细信息窗格、复选框、AI 面板、批量选择/复制到/移动到。

---

## [0.1.0] - 2026-11-19

**P1 骨架**：第一个可构建、可日常使用的版本。目标不是功能对齐 ExBar mod，而是把"独立进程 + 托管真实
Shell 视图 + 自绘 chrome"这条路线跑通，并留下可继续生长的分层骨架。

### 新增

- **进程骨架**（`src/main.cpp`）
  - Per-Monitor V2 DPI 感知，逐级降级到 `SetProcessDpiAwareness(2)` / `SetProcessDPIAware()`。
  - STA COM 初始化；`ApplyProcessDarkMode` 在任何窗口创建前调用。
  - 单实例：`Local\ExBarBrowser.Instance` 命名互斥量；已有实例时用 `WM_COPYDATA`（magic `0xEBB1`）
    把路径转给已有窗口开新标签，并还原/前置该窗口。
  - 命令行：目录参数、`/select <文件>`、`/select,<文件>`（打开父目录）、无参数时打开用户主目录。
- **窗口与布局**（`src/app.cpp`）
  - 主窗口 `ExBarBrowser_Main`，客户区自上而下：标签栏 32 px / 导航栏 30 px / 视图区 / 状态栏 22 px
    （全部经 `Theme::S()` 按 DPI 缩放）。
  - 视图区 `ExBarBrowser_ViewArea` 作为所有标签容器的父窗口，切标签只做 `ShowWindow` 隐藏，不销毁视图。
  - 标签模型：每个标签持有 `ShellView` + 线性历史 `hist` + 当前索引 `idx` + 显示名 + 项目数。
  - 关闭最后一个标签即退出进程。
- **标签栏**（`src/tab_strip.cpp`）
  - 自绘、双缓冲；标签宽度随标题自适应并夹在 112–230 px；激活标签带 accent 下划线。
  - 悬停显示关闭按钮，中键点击关闭；右侧 `+` 新建标签。
  - 只负责绘制与命中，点击通过 `WM_APP_TAB_SELECT/CLOSE/NEW` 投递给 `App`，不持有标签状态。
- **导航栏**（`src/nav_bar.cpp` 窗口/布局/命中、`src/nav_bar_draw.cpp` 绘制、`src/nav_metrics.h` 尺寸常量、`src/nav_bar_actions.cpp` 交互）
  - 五个自绘按钮：后退 / 前进 / 向上 / 刷新 / 视图。后退与前进用系统真实图标（`SHGetStockIconInfo`
    225/226/227）并带 12 px 分裂箭头（点箭头出单向历史）；向上与视图用 Segoe MDL2 字形。
  - 面包屑画在地址框内：前导文件夹图标 + 常驻折叠钮 `‹` + 每级 32 px chevron 区；
    路径过长时从左侧折叠，折叠钮列出被折叠层级；未折叠时折叠钮列"此电脑"顶层（驱动器/用户目录）。
  - 地址框右侧内嵌 `⌄` 下拉：前进 + 后退合并历史（各最多 16 条，中间分隔线）。
  - 地址编辑：点空白或 `Ctrl+L` 进入，`Enter` 提交、`Esc` 还原、失焦退出；编辑框内 `Tab`
    按前缀补全同级文件夹并循环。
  - 视图菜单：8 档模式（超大/大/中/小图标、列表、详细信息、平铺、内容），经
    `IFolderView2::SetViewModeAndIconSize` 立即作用于当前标签。
- **自绘下拉弹层**（`src/popup_list.cpp`）
  - `ShowPopupList`：`WS_POPUP` + `CS_DROPSHADOW` 宿主 + owner-draw `LISTBOX`，24 px 行高、
    16 px 图标、悬停/选中底色、深色 1 px 边框、文件夹行右端 chevron、分隔行、禁用行。
  - 自带模态消息循环：`Enter` 选中、`Esc` 取消、失焦自动关闭；返回行索引，图标由调用方释放。
  - 替代 `TrackPopupMenu`：系统菜单带不了图标、配色跟不了主题、行高不可控。
- **导航模型**（`src/nav_model.cpp`）
  - 解析名（`C:\...`）与命名空间（`::{GUID}` / `shell:`）双口径；命名空间用友好名显示、不提供"向上"。
  - 同级枚举走 `SHParseDisplayName` → `SHBindToParent` → `BindToObject` → `EnumObjects`，
    跳过隐藏项，按 `StrCmpLogicalW` 自然排序。
- **Shell 视图**（`src/shell_view.cpp`）
  - 每个标签一个 `IExplorerBrowser`，`SetOptions(EBO_SHOWFRAMES)` 打开 shell 原生导航窗格，
    `SetPropertyBag(L"ExBarBrowser")` 隔离视图状态。
  - `IExplorerBrowserEvents` 接收器：`OnViewCreated` / `OnNavigationComplete` 回填路径、显示名与项目数。
  - 主题联动：`SetWindowTheme` + `AllowDarkModeForWindow`，并只对 `ExplorerBrowserControl` /
    `SHELLDLL_DefView` / `DirectUIHWND` 三类窗口补发 `WM_THEMECHANGED`，避免全树递归。
- **主题**（`src/theme.cpp`）
  - 颜色令牌单一来源：深色文件区 `#141414` / 工具栏 `#1e1e1e`，浅色 `#ffffff` / `#f3f3f3`（本机实测值）。
  - 令牌覆盖：`bg/bar/barAlt/hover/pressed/line/text/sub/disabled/chevron/accent/editBg/editBorder/listBg/listHot/listSel`
    + 画刷 `brBg/brBar/brBarAlt/brHover/brEdit` + 字体 `font/fontBold/glyphFont`（Segoe MDL2 Assets）。
  - 跟随系统 `SystemUsesLightTheme`（回退 `AppsUseLightTheme`）；`WM_SETTINGCHANGE(ImmersiveColorSet)`
    与 `WM_THEMECHANGED` 时整体重建令牌、字体、画刷与 shell 视图主题。
  - 进程级深色走 uxtheme 未文档化导出（序号 135 `SetPreferredAppMode`、133 `AllowDarkModeForWindow`），
    取不到就静默降级为"仅工具栏深色"。
  - 深色标题栏：`DwmSetWindowAttribute` 属性 20，失败退 19（1809）。
- **状态栏**（`src/status_bar.cpp`）：左侧"N 个项目"，右侧当前路径，超长省略。
- **诊断**（`src/common.h` 的 `LogDbg`）
  - `%TEMP%\ExBarBrowser.log`（UTF-16 LE，追加写）记录 `Initialize / parse / item / browse / complete`
    五步 HRESULT 与路径，以及 `[click]`（导航栏命中分解）与 `[popup]`（弹层创建/失焦）两类调试行。
- **注册脚本**（`scripts/install.ps1`、`scripts/uninstall.ps1`）
  - `HKCU\Software\Classes\Folder\shell\ExBarBrowser`（"Open with ExBar"）与
    `Directory\Background\shell\ExBarBrowser`（"Open in ExBar"）两个动词，全部写 HKCU、不需要管理员。
  - `-Takeover` 可把"双击文件夹"重定向到本程序，改动前先导出
    `backup\Folder-shell.reg` 并留 `.takeover` 标记；卸载脚本据此还原系统默认。
- **开发工具**（`scripts/shot.py`）：`PrintWindow(PW_RENDERFULLCONTENT)` 抓主窗口整图，按类名
  `ExBarBrowser_Main` 定位，纯 ctypes + zlib 手写 PNG，不依赖 Pillow。
- **构建**（`build.sh` / `build.bat`）：真多 TU，clang++ 20.1.3 `-target x86_64-w64-mingw32`
  `-std=c++20 -O1 -g -mwindows`，静态链接 libstdc++/libgcc，单 exe ≈3.2 MB。

### 修复

开发期在代码注释中留档的四个坑（均为实测，不是推测）：

- **`WM_COPYDATA` 解错一层**：`lParam` 是 `COPYDATASTRUCT*` 而不是 payload 本身，之前直接把
  `lParam` 当 `CopyDataPayload*` 用，magic 校验永远失败、转发静默不生效。现在先解 `COPYDATASTRUCT`
  再校验 `dwData` / `cbData` / `magic` 三重条件。
- **同级下拉列成了同级**：`SHBindToParent` 给出的是"父文件夹 + 本项子 PIDL"，直接 `EnumObjects`
  枚举到的是兄弟项；必须再 `BindToObject` 拿到目标文件夹自己的 `IShellFolder` 才是子文件夹。
- **正斜杠导航失败**：`SHCreateItemFromParsingName` 对 `C:/x/y` 返回 `E_INVALIDARG`（实测），
  命令行与用户输入都常带正斜杠。`ShellView::DoNavigate` 统一转反斜杠并去掉尾部多余分隔符，
  命名空间路径不转换。
- **首屏路径不规范化**：`ShellView::Create` 里的首次导航可能同步完成，`App::NewTab` 原来用调用方
  传入的原始字符串当标题与历史首项，会出现 `C:/x` 与 `C:\x` 两份。现在优先取 shell 回填的
  `view->path()`。

### 已知问题

- 标签栏不处理溢出：标签总宽超过窗口后，右侧标签与 `+` 按钮被裁到窗口外，无法滚动或压缩。
- 盘根分段仍绘制 chevron 区，点击无菜单（无父级可枚举同级）。
- 弹层里 `folder` 行画了 chevron 但没有二级弹出，点击等于直接选中该行。
- 视图菜单不标当前档位（弹层暂无"选中态"）。
- `app.cpp` 438 行，仍同时承担标签生命周期、导航、布局与命令分发，待按架构规则拆分（B2/B3）。
- 日志有两份实现（`common.h` 的 `LogDbg` 与 `shell_view.cpp` 的 `LogLine`），待合并。
- `/select,<文件>` 只打开父目录，不选中目标项。
- 无任何配置与状态持久化：窗口几何、标签组、视图设置都不保存。
- 日志无轮转；`%TEMP%\ExBarBrowser.log` 会一直追加。
- 每个标签一个 `IExplorerBrowser`，内存随标签数线性增长，未做懒加载或回收。

### 安全

- 注册脚本只写 `HKCU`，不请求提权；`-Takeover` 前先导出备份，卸载时只删自己写的键值。
- 不注入 explorer、不装全局钩子、不注册 shell 扩展 DLL，卸载后系统行为完全恢复。

---

## 版本号约定

`0.x` 阶段：`0.1.0` 为 P1 骨架；`0.2.x` 起按 P2 功能递增（`0.2.1` = P2 收口 + 操作契约 + AI 接口），
`0.3.x` 对应 P3，`1.0.0` 留给"可作为系统默认文件夹处理器日常使用"的里程碑
（P2 全绿 + 连续一周无崩溃）。

一个版本号对应一次**可回退的交付**，不对应一次提交。版本内允许多次提交，
但 `[Unreleased]` 只能在版本发布时整段下移，不要边做边改已发布的段落。
