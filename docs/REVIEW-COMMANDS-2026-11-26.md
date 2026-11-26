---
status: current
baseline: 2026-11-26
scope: 命令总线（commands*.cpp/h、cmd_req.h）与 AI 通道（ai_api*、ai_server*、ai_queue*、ai_events*、ai_http*、ai_sse*、mcp_stdio*）
method: 全文通读源码 + 交叉验证调用点（app.cpp / app_wnd.cpp / app_commands*.cpp / config.h / json.cpp）
---

# 命令总线与 AI 通道域审查报告（2026-11-26）

> 审查对象：`src/commands*.cpp/.h`（约 1.3k 行）+ `src/ai_*`（约 2.2k 行）+ `src/mcp_stdio.cpp`。
> 每条问题给出：级别 / 文件:行 / 符号 / 问题 / 机理 / 修法。行号以当日源码为准。
> 结论先行：架构方向正确（唯一入口、元数据驱动、fail-closed、人机同源），本次发现
> **2 条 P1（其中一条可让热插拔后通道整体失效）**、3 条 P2、5 条 P3。

---

## 一、问题清单

### P1-1〔ai_queue〕`interrupted_` 粘滞：通道停用后再次启用，所有请求立即失败

- 位置：`src/ai_queue.cpp:65-73`（`Queue::Interrupt`）、`src/ai_server.cpp:366-399`（`StopServer`）、`src/ai_server.cpp:346-360`（`StartServer`）
- 符号：`aiqueue::Queue::interrupted_` / `ai::StartServer` / `ai::StopServer`
- 问题：`StopServer` 调 `g_queue.Interrupt()` 把 `interrupted_` 置 true，但 `Queue` 没有任何复位它的接口；`StartServer` 也不复位。通道热插拔关闭再打开后，`Submit` 的等待谓词 `[&] { return job->done || interrupted_; }` 恒真，每个请求立即返回"超时"。
- 机理：`config.set aiEnabled=false` → `App::SyncAiChannel`（`app.cpp:809-842`）→ `StopServer` → `Interrupt()`。用户再设回 `true` → `StartServer`：`g_running` 已是 false，会完整重建管道与监听线程，但 `g_queue` 是文件级静态对象（`ai_server.cpp:47`），`interrupted_` 仍是 true。监听线程 `RunOnUiThread → Submit` 直接短路返回空串 → 客户端全部收到 timeout。表现为"关一次 AI 开关再打开，通道永久死掉，直到重启进程"。单测没抓到是因为测试从不走"停了再启"的路径。
- 修法（二选一）：
  1. `Queue` 增加 `Reset()`（锁内置 `interrupted_ = false`），`StartServer` 里在启动线程前调用；
  2. 或 `Interrupt` 语义改为一次性唤醒（唤醒等待者后自动复位）。建议方案 1，语义显式。
- 代码佐证：

```cpp
// ai_queue.cpp:65
void Queue::Interrupt() {
    { std::lock_guard<std::mutex> lk(mu_);
      interrupted_ = true;          // 只置位，无复位路径
      queue_.clear(); }
    cv_.notify_all();
}
// ai_queue.cpp:21 —— Submit 的等待谓词
const bool got = cv_.wait_for(lk, std::chrono::milliseconds(waitMs),
                              [&] { return job->done || interrupted_; });
```

### P1-2〔ai_api〕`timeoutMs=90000` 的契约与 `aiTimeoutMs=10000` 的现实差 9 倍

- 位置：`src/ai_api.cpp:183、219`（`ListTools`/`FindTool` 硬编码 `t.timeoutMs = 90000`）、`src/ai_api_json.cpp:221`（tools/list 原样广播）、`src/config.h:281`（`aiTimeoutMs = 10000`）、`src/ai_server.cpp:51-54`（`UiTimeoutMs()` 读配置）
- 符号：`ToolSpec::timeoutMs` / `Settings::aiTimeoutMs`
- 问题：tools/list 向客户端声明"单次调用软超时 90 秒，客户端据此设定时器"；而管道侧实际等待上限是 `aiTimeoutMs`（默认 10 秒，可被改得更小）。两者无任何联动。
- 机理：客户端信了 90s 契约 → 10s 时收到 `error.code=-32000 / errorCode=timeout` → 客户端的"未到期收到失败"分支与"到期"分支语义完全不同（前者通常是环境错误，后者是放弃等待）。契约错误直接导致调用方对"该不该重试"做出错误决策。命令域反复强调 errorCode 是给机器分支的，但给机器分支的时间基准本身是错的。`ToolSpec::timeoutMs` 注释里写的"长命令可逐条覆盖"从未落地——命令表里没有任何一条提供覆盖值。
- 修法：`ListTools()/FindTool()` 从 `Config::Get().s().aiTimeoutMs` 取值（传输层配置就是唯一真源），并顺手把逐条覆盖的能力接到 `CommandDef`（加一个 `timeoutMs` 字段，默认 0 = 用全局值）。若担心 ai_api 层引配置依赖，至少在 tools/list 的 description 或 capabilities 里注明"实际等待以 settings.aiTimeoutMs 为准"。

### P2-1〔mcp_stdio〕桥接独占单实例管道，与本地脚本通道互斥且未披露

- 位置：`src/ai_server.cpp:228-230`（`CreateNamedPipeW(..., 1, ...)` 单实例）、`src/mcp_stdio.cpp:196-235`（`BridgeBody` 常驻连接）
- 符号：`ServeLoop` / `BridgeBody`
- 问题：管道是 `FILE_FLAG_FIRST_PIPE_INSTANCE` 语义的单实例（nMaxInstances=1）。`--mcp-stdio` 模式下桥接线程常驻占用唯一实例；此时任何第三方本地脚本直连 `ai-pipe.txt` 都会得到 `ERROR_PIPE_BUSY`。
- 机理：桥接是"stdio 客户端 ⇄ 管道"的常驻字节搬运工，连接从不断开。单实例意味着桥接在 = 管道满。用户在 MCP 客户端里配了 `--mcp-stdio` 后，AI-INTERFACE.md §3 的"PowerShell 手工验证通道"等所有直连脚本全部失效，且 `ERROR_PIPE_BUSY` 的文档解释只覆盖了"订阅断开"这一种历史成因（`ai_server.cpp:310-315` 注释）。`ConnectPipe`（`mcp_stdio.cpp:123-135`）对 `ERROR_PIPE_BUSY` 的处理是重试 2 秒后放弃——桥接自己占着时，重试注定失败。
- 修法：短期在 AI-INTERFACE.md 明确披露互斥；中期把管道改成多实例（nMaxInstances=2+ + `PIPE_UNLIMITED_INSTANCES` 循环 `CreateNamedPipeW`，每连接一线程或重叠 I/O）。注意多实例化要连带审查 `g_pipe` 全局句柄与 `StopServer` 的唤醒逻辑（见 P3-4）。

### P2-2〔ai_api〕`AuditRejected` 把机器码塞进人可读 reason 位

- 位置：`src/ai_api.cpp:256-259`（`reject` lambda）、`src/ai_api.cpp:274、290-297、311、318`（各拒绝点 `reject(r.errorCode, r.errorCode)`）、`src/commands_audit.cpp:62-78`（`AuditRejected`）
- 符号：`CallImpl` 内 `reject` / `CommandBus::AuditRejected`
- 问题：`reject(reason, errorCode)` 两个实参都传 `r.errorCode`——审计行的 `reason` 字段落的是 `unknown_tool` / `bad_args` 这类机器码，而非中文原因（`r.error` 明明就在旁边）。
- 机理：`commands_audit.cpp` 与 `commands.h:303-307` 的约定是"errorCode 机器可判读、reason 人可读，机器码不能塞中文，中文也不该被机器码挤掉"。拒绝路径把这个双轨约定打穿：事后人工翻 audit.jsonl 追"AI 为什么被拒"，reason 列只有机器码，得再去对照错误码表。执行路径（`Dispatch → Audit`）是守约的（reason 传 `pendingReason_`），只有拒绝路径违约。
- 修法：`reject` 改为捕获时同时拿两个字段：调用点已有 `r.error`（中文）与 `r.errorCode`，把 lambda 改成 `reject(r.error, r.errorCode)` 即可；`AuditRejected(commandName, reason, errorCode)` 的形参序不用动。注意 `bad_args` 的 `r.error` 在 reject 时已赋好（`ai_api.cpp:317`），逐点核对 early-return 前 `r.error` 均已填充。

### P2-3〔commands〕`Register`/`RegisterWithResult` 互斥检查只做了一半

- 位置：`src/commands.cpp:109-128`（`Register` 检查 resultHandlers_ 并 LogDbg 警告）、`src/commands.cpp:134-142`（`RegisterWithResult` 不检查 handlers_）、`src/commands.cpp:242`（Dispatch 先查 resultHandlers_）
- 符号：`CommandBus::Register` / `CommandBus::RegisterWithResult`
- 问题：`Register` 会在同 id 已有 ResultHandler 时报警"这次的 Register 会被忽略"；反方向（先 `Register` 后 `RegisterWithResult`）没有任何检查——先注册的普通 handler 被静默遮蔽，连日志都没有。
- 机理：Dispatch 的查找顺序是 resultHandlers_ 优先（`commands.cpp:242-267`），所以"带结果的胜出"无论注册顺序如何。`Register` 的警告注释说"注册时就报警，而不是等到运行时表现为'命令没反应'"——但只覆盖了两个方向之一。两个通道分居不同 `app_commands_*.cpp` 文件时，"我明明注册了却没跑"的排查成本与作者写这条警告时想消灭的完全一样。
- 修法：`RegisterWithResult` 开头加对称检查：遍历 `handlers_`，同 id 时 `LogDbg(L"[cmd] 冲突：%s 已用 Register 注册，它的 handler 会被 RegisterWithResult 遮蔽", ...)`。也可以更进一步把两条通道合一（`Register(id, fn, wantsResult)`），但最小修复是补对称警告。

### P3-1〔ai_server〕令牌兜底路径用时间+计数器，可预测

- 位置：`src/ai_server.cpp:63-84`（`MakeToken`）
- 符号：`MakeToken`
- 问题：`RtlGenRandom` 不可用时的兜底是 `GetTickCount64() + (++c)` 填充 8 字节、其余 8 字节全零。
- 机理：兜底令牌的熵实际只有"开机以来的毫秒数 + 进程内计数"。同机低权限进程若能读到启动时刻（进程创建时间可查），可以枚举毫秒值暴力对表——虽然 AI-INTERFACE.md §4 已声明"不防同用户恶意进程"，但令牌防的正是"别的程序随手乱调"，兜底把这条护栏降到接近于无。16 字节里 8 字节恒零还让令牌在 hex 上呈现明显规律。
- 修法：兜底改用进程随机种子的哈希链（`filehash::HashBytes` 已在 ai_events.cpp 用于同类用途），或干脆兜底失败时拒绝启动通道（`StartServer` 返回 false 并记日志）——fail-closed 与项目口径一致。

### P3-2〔ai_server〕`StopServer` 与监听线程对 `g_pipe` 的无锁竞态

- 位置：`src/ai_server.cpp:39`（全局 `HANDLE g_pipe`）、`366-373`（UI 线程读它并 `CancelIoEx/DisconnectNamedPipe`）、`228-241、332-335`（监听线程写它）
- 符号：`g_pipe` / `StopServer` / `ServeLoop`
- 问题：同一个裸 `HANDLE` 被 UI 线程（Stop）与监听线程（ServeLoop 循环体内赋值/置 INVALID）并发读写，无原子性、无锁。
- 机理：竞态窗口：监听线程刚 `CloseHandle(g_pipe)` 但还没执行 `g_pipe = INVALID_HANDLE_VALUE;` 时，UI 线程读到旧句柄并对其 `CancelIoEx` —— 对已关闭句柄操作通常返回错误（无害），但句柄值可能已被系统回收复用给其他对象，此时 `CancelIoEx/DisconnectNamedPipe` 会打到无关对象上。概率低、后果不定，属于典型"测试里永远复现不了"的 UB 面。
- 修法：`g_pipe` 换成读侧快照 + 监听线程内置换：Stop 里不直接碰句柄，而是置 `g_running=false` 后靠既有的自连唤醒（`StopServer` 已实现自连唤醒 `ConnectNamedPipe`）让监听线程自己走清理路径；或最小改动用 `std::atomic<HANDLE>`（x64 上 HANDLE 8 字节、atomic 无锁），Stop 读一次快照再做 Cancel。

### P3-3〔commands〕`InvokeByNameLegacy` 热路径每次重新解析 schema JSON

- 位置：`src/commands.cpp:320-332`（`InvokeByNameLegacy`）、`src/commands_args.cpp:49-72`（`ArgsFromLegacyArg` 每次 `json::Parse(argsSchema)`）、`src/commands_table.cpp:364-390`（`FindCommand` 最多 3 次全表线性扫描）
- 符号：`InvokeByNameLegacy` / `ArgsFromLegacyArg` / `FindCommand`
- 问题：键位/序列/鼠标每次触发都走 `FindCommand`（241 条 × 最多 3 轮宽字符比较）+ `ArgsFromLegacyArg`（把该命令的 argsSchema 字符串完整 Parse 一遍构造 json::Value）。
- 机理：单次开销是微秒级，键位触发频率低，本身不构成可感卡顿；但它是命令域唯一的"每键必付"成本，且 `FindCommand` 的三段式匹配（精确 → AliasOf 反查 → LooksDotted 正查）在 d.name 已全面点分化的今天只剩前两段有意义。命令表是编译期常量（`CommandTable()` 已 static 缓存），键名→CommandDef 与 schema 解析结果同样可以缓存。
- 修法：在 `CommandTable()` 构建时顺手建 `std::unordered_map<std::wstring, const CommandDef*>`（点分名 + legacy 名两个键都登记），`FindCommand` 退化为一次哈希查表；`ArgsFromLegacyArg` 的解析结果按 `CmdId` 缓存（schema 是静态字符串，结果可复用）。属于纯性能重构，行为零变化，放欠账即可。

### P3-4〔ai_server〕订阅分支的 `after` 游标对丢行不设防

- 位置：`src/ai_server.cpp:302-307`（`WaitFor` 循环里 `if (seq > after) after = seq;`）、`src/ai_events.cpp:331-348`（`WaitFor` 只返回一条）
- 符号：`ServeLoop` 订阅分支 / `events::WaitFor`
- 问题：`WaitFor` 每次只交出**一条**事件，循环里以 `after = seq` 推进游标。若 `send()` 失败前已连续产出多条事件，剩下的条目下轮还会拿到（游标没推进）——这是对的；但反过来，`Since`（`ai_events.cpp:318`）里定义的 `gap`（缓冲淘汰导致 seq 断档）在订阅推送路径上没有对应处理：订阅者长时间阻塞后缓冲淘汰了旧事件，它会在无感知的情况下从"现在的最老事件"继续收，跳过中间一段且没有任何 gap 标记。
- 机理：`Since` 对补历史场景明确给了 `gap:true / firstAvailable`，订阅推送路径只推进游标不检测断档。长时间挂着的订阅者（AI 宿主的常驻订阅）在事件风暴（如批量命令 + state.changed 每 100ms 一条）后缓冲（kMaxEvents=512）滚动，会静默漏事件——对靠事件做"命令完成检测"的客户端表现为偶发"等不到 done"。
- 修法：订阅分支在 `WaitFor` 返回后核对 `seq` 与本地 `after` 的连续性（`seq != after+1` 即为断档），断档时合成一条 `{"event":true,"type":"events.gap","firstAvailable":N}` 推给客户端，客户端据此自行调 `events/since` 补账。

### P3-5〔ai_server〕简格式与 JSON-RPC 对同一请求的超时口径不同

- 位置：`src/ai_server.cpp:198-216`（`TimeoutResponse`）
- 符号：`TimeoutResponse`
- 问题：超时响应只区分"有没有 jsonrpc 字段"两种形状；旧格式脚本拿到 `{"ok":false,"error":"timeout"}` 时**没有** errorCode 字段（JSON-RPC 分支有 `errorCode:"timeout"`）。
- 机理：项目口径是"errorCode 才是给机器分支的稳定标识"（AI-INTERFACE.md §3 错误码表第一行就把 timeout 列为errorCode）。JSON-RPC 客户端拿得到 `errorCode:"timeout"`，旧格式客户端拿不到——同一个事实在两条协议路径上的机器可读性不同。旧脚本想区分"超时（可查 job）"与"其他失败（别重试）"只能匹配字符串。
- 修法：旧格式超时响应补一个 `"errorCode":"timeout"` 字段（加字段是向后兼容的，`ok:false/error` 原样保留）。

---

## 二、总结表

| # | 级别 | 文件:行 | 符号 | 一句话 | 修法量 |
|---|---|---|---|---|---|
| P1-1 | P1 | ai_queue.cpp:65-73 / ai_server.cpp:346+ | `Queue::interrupted_` | 停用再启用后队列永久短路，通道死到重启 | 小（加 Reset + StartServer 调用） |
| P1-2 | P1 | ai_api.cpp:183 / config.h:281 | `timeoutMs` vs `aiTimeoutMs` | tools/list 契约 90s、实际 10s，机器分支时间基准错误 | 小（取配置值 + CommandDef 可选覆盖） |
| P2-1 | P2 | ai_server.cpp:228 / mcp_stdio.cpp:196+ | 单实例管道 | 桥接独占后本地脚本全部 PIPE_BUSY，未披露 | 中（文档先披露，后多实例化） |
| P2-2 | P2 | ai_api.cpp:256+274 等 | `reject` lambda | 拒绝审计 reason 位落机器码，破坏双轨约定 | 极小（改传 `r.error`） |
| P2-3 | P2 | commands.cpp:109-142 | `Register*` | 互斥检查只做一个方向，反向静默遮蔽 | 极小（补对称警告） |
| P3-1 | P3 | ai_server.cpp:63-84 | `MakeToken` 兜底 | 时间+计数器兜底令牌近乎可预测 | 小（哈希链或拒绝启动） |
| P3-2 | P3 | ai_server.cpp:39+366 | `g_pipe` | Stop 与监听线程无锁竞态，句柄复用面 UB | 小（atomic 或自连唤醒收口） |
| P3-3 | P3 | commands.cpp:320 / commands_args.cpp:49 | legacy 热路径 | 每键触发重新解析 schema + 三轮线性查表 | 中（哈希索引 + 解析缓存） |
| P3-4 | P3 | ai_server.cpp:302 / ai_events.cpp:318 | 订阅游标 | 订阅推送无 gap 标记，风暴后静默漏事件 | 小（断档合成 gap 事件） |
| P3-5 | P3 | ai_server.cpp:198-216 | `TimeoutResponse` | 旧格式超时无 errorCode，两条协议机器可读性不一致 | 极小（补字段） |

## 三、建议处理顺序

1. **立即**：P1-1（三行修复 + 一条"停启"回归用例）、P2-2 与 P3-5（各一行）。
2. **本周**：P1-2（契约对齐，防客户端误判重试策略）、P2-3。
3. **立项**：P2-1 多实例化（牵动 ServeLoop 重构，与 P3-2 一起做）；P3-3 哈希索引；P3-4 gap 事件；P3-1 令牌兜底。

> 复核建议：P1-1 可写一条 e2e——`config.set aiEnabled=false → true` 后立刻 `tools/list`，
> 修复前必现 timeout，修复后正常返回；P1-2 用 `python tools/probe_mcp_stdio.py` 对比
> tools/list 的 timeoutMs 与 `config.get aiTimeoutMs` 的值。
