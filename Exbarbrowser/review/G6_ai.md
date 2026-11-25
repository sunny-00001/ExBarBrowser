# G6_ai 组代码审查报告

审查范围：`src/ai_*` / `src/aichat_*` / `src/mcp_stdio.*`，共 **21 个文件 / 5439 行**（与 `review/G6_ai.txt` 清单一致，`wc -l` 逐项核对）。
纪律：每个文件 Read 到底（最长 775 行的 `ai_server.cpp` 一次读完），逐函数逐分支；无抽样。
方式：只读 `src/`（辅以 `identity.h` / `theme.h` / `config*.cpp` / `app.cpp` 核对约定与调用点），未 build、未运行任何 AI 命令、未改源码。

---

## 一、文件覆盖清单

| # | 文件 | 行数 | 结论 |
|---|------|------|------|
| 1 | src/ai_api.cpp | 428 | 基本合格。schema 校验有类型盲区（union type 静默跳过、double→int 转换越界 UB）；`reject` 路径与审计覆盖到位。 |
| 2 | src/ai_api.h | 352 | 合格（纯声明+设计说明）。一处字段名与线协议不一致（`selectionPaths` vs JSON `selection`）。 |
| 3 | src/ai_api_json.cpp | 279 | 基本合格。`content` 为字符串而 MCP 路径为数组（同名字段两种类型）；1 处排版破损。 |
| 4 | src/ai_events.cpp | 396 | 基本合格。锁内同步落盘（UI 线程付 I/O 代价）；`Since()` 的 limit/gap 语义有小缺陷；`EventLine` 平摊键可覆盖信封。 |
| 5 | src/ai_events.h | 115 | 合格。契约说"没令牌不能订阅"，但订阅入口在 `ai_server` 里绕过了校验（见 A-3）。 |
| 6 | src/ai_http.cpp | 260 | 有问题。取消不灵敏（最长 180s）、流未校验 `[DONE]` 即报 ok、UA 硬编码品牌串。 |
| 7 | src/ai_http.h | 75 | 合格。超时/取消/线程约束写得很清楚，与实现基本对得上。 |
| 8 | src/ai_queue.cpp | 85 | 有问题。超时即从队列摘除，与头文件及 `ai_server` 的注释契约相反；`Interrupt` 无复位入口（A-1 的根因）。 |
| 9 | src/ai_queue.h | 74 | 有问题。`interrupted_` 是粘性状态且没有 `Reset()`，导致通道"关→开"后永久不可用。 |
| 10 | src/ai_server.cpp | 775 | 问题最多。StopServer 顺序（join 先于 Interrupt）、`g_pipe` 跨线程裸读、未鉴权订阅、64KB 超长分支不可达、`CancelIoEx` 用错对象。行数超 500 但已记账（775），无棘轮回归。 |
| 11 | src/ai_server.h | 78 | 合格（文档略陈旧）：仍写"每个连接先发 HELLO"、"PostMessage 到主窗口"，实现已改为队列，属文档漂移。 |
| 12 | src/ai_sse.cpp | 259 | 基本合格。解析健壮（残行/多字节/整段重发都处理了）；`calls_.resize(idx+1)` 无上限，可被服务端打成 OOM。 |
| 13 | src/ai_sse.h | 97 | 合格。四个坑的说明与实现一一对应。 |
| 14 | src/aichat_panel.cpp | 634 | 有问题。**DPI 硬编码像素 0 处 `theme_->S()`**（B-1）；`Retry()` 同步调 EnsureWebView 违背本文件自述规则；每次 WM_PAINT 写 3 条日志。行数 634 已记账。 |
| 15 | src/aichat_panel.h | 128 | 合格。生命周期与"收起不销毁"的理由清楚。 |
| 16 | src/aichat_service.cpp | 282 | 有问题。`AssignProcessToJobObject` 返回值未检查（第一重保证可静默失效）；`Enabled()` 不清 `g_started`，node 崩了仍报 true；无重启。 |
| 17 | src/aichat_service.h | 59 | 合格。三重保证的设计写得很清楚（正因如此 B-5 更值得修）。 |
| 18 | src/aichat_webview.cpp | 622 | 有问题。`Start()` 复用 `startIssued_` 导致 `Retry()` 永久失效（A-4）；挂起回调持 `owner_` 裸指针，面板销毁后 UAF 风险。 |
| 19 | src/aichat_webview.h | 79 | 合格。vtable/引用计数纪律说明到位。 |
| 20 | src/mcp_stdio.cpp | 326 | 基本合格。`g_thread_id` 跨线程非原子；"取消读"与"stdin EOF"未区分（误取消=退出程序）；`LineReader` 无限长。 |
| 21 | src/mcp_stdio.h | 42 | 合格。"只搬字节"的边界守住实测（未发现它自己做协议判定）。 |

工程约束核对：品牌串 `ExBarBrowser` 拼写一致；命名空间 `ai` / `ai::events` / `ai::http` / `ai::sse` / `aiui` / `aisvc` / `mcpstdio` 无越界污染；单文件 ≤500 行——`ai_server.cpp`(775)、`aichat_panel.cpp`(634)、`aichat_webview.cpp`(622) 三个超限但均在 `data/file-length-baseline.json` 记账内（其中 webview 实际 622 < 记账 672，棘轮会自收紧），**无回归**。

---

## 二、严重级分组

### A 级：会造成功能不可用 / 挂死 / 越权

**A-1｜ai_queue.h:70 + ai_server.cpp:387 —— `interrupted_` 粘性，通道热插拔"关→开"后永久不可用（严重）**
- 问题：`Queue::Interrupt()` 把 `interrupted_ = true` 且**没有任何复位入口**（全仓 grep 只有 `ai_server.cpp:387` 一处调用 `Interrupt`，无 `Reset`）。`Submit()` 的谓词 `job->done || interrupted_`（ai_queue.cpp:22）一旦置真就立即返回 false，于是 `RunOnUiThread` 恒返回空串，每条请求都走 `TimeoutResponse`。
- 触发：`app.cpp:767-784 SyncAiChannel()` 的热插拔路径——设置里把 `aiEnabled` 关掉（`StopServer()` → `Interrupt()`），再打开（`StartServer()` 重新起线程，但队列仍是被打断的那个）。之后**整条 AI 通道只回 timeout**，管道/令牌/事件全都正常，极难归因。`ai_server.h:12` 明写"可随时热插拔启停"。
- 修复：给 `Queue` 加 `void Reset()`（`interrupted_=false; queue_.clear();`），在 `StartServer()` 里 `g_queue.Reset();` 早于 `g_thread = std::thread(...)`；顺手让 `Submit()` 在 `interrupted_` 为真时**不入队**直接返回 false（现在会先 push 再摘，白跑一趟）。

**A-2｜ai_server.cpp:364-397 —— `StopServer` 先 `join` 再 `Interrupt`，关闭/退出可挂到 10 分钟（严重）**
- 问题：顺序是 `g_running=false` → `CancelIoEx/DisconnectNamedPipe` → `g_thread.join()` (372) → **最后**才 `g_queue.Interrupt()` (387)。监听线程此刻可能正阻塞在 `RunOnUiThread`→`Submit` 的 `cv_.wait_for`（最长 `aiTimeoutMs`）上；而 `StopServer` 跑在 UI 线程，`join` 期间 `PumpRequests` 不可能被执行 → 那个 job 永远拿不到 `done`。等待上限是 `aiTimeoutMs`，`config_io.cpp:227` 允许到 **600000ms = 10 分钟**。
- 触发：一次 AI 调用正在执行（或刚排队）时，用户关窗口，或把 `aiEnabled` 改掉。两条路径都先 `KillTimer(kAiTimer)`（app.cpp:743/780）再 `StopServer`，所以连"顺手泵一下"的机会都没有。
- 修复：把 `g_queue.Interrupt()` 移到 `join()` **之前**（顺序：`g_running=false` → `Interrupt()` → 唤醒管道 → `join()`）；配合 A-1 的 `Reset()` 保证下次启动可用。

**A-3｜ai_server.cpp:252-270（判定在 171-174）—— 未鉴权的 `events/subscribe` 也能进推送态，事件流越权（严重）**
- 问题：`processLine` 里订阅判定只调 `IsSubscribeRequest(line)`，它**只解析 `method == "events/subscribe"`，完全不看令牌**。请求本身会被 `HandleRequestLine` 判 `bad_token` 并写回错误，但紧接着 `subscribed=true; ++g_subscribers;`，连接立刻转入推送循环，`events::WaitFor` 之后的**所有事件**（命令名、完整 args、错误原因、`state.changed` 的整份状态快照含路径/选中项）都推给这个未授权客户端。JSON-RPC 分支同理（`HandleRpcLine` 回 `-32600 bad_token`，但订阅态照切）。
- 触发：同用户会话内任意进程连上管道名（`ai-pipe.txt` 就在数据目录，明文可读）→ 发一行 `{"method":"events/subscribe"}` → 拿到全部事件，令牌形同虚设。这直接违反 `ai_events.h:11`"事件线也是同一个令牌保护的，没令牌不能订阅"。
- 修复：订阅判定必须复用鉴权结果。最小改法：`HandleRequestLine` 已经知道结果，让它回传一个"这条是否成功订阅"的标志（或让 `processLine` 自己再走一遍 `CheckToken`/`RpcHasToken` 同源实现），只有鉴权通过才 `subscribed=true; ++g_subscribers`。

**A-4｜aichat_webview.cpp:415（配合 560-564、aichat_panel.cpp:275-285）—— WebView2 失败后"重试"永久失效（严重）**
- 问题：`WebViewImpl::Start()` 第一行 `if (startIssued_) return Ready() || !reason_.empty();`。`Stop()` 只释放 `core_/ctrl_/env_`，**既不复位 `startIssued_` 也不清 `reason_`**。于是任何一次失败（`reason_` 非空）之后，第二次 `Start()` 恒返回 **true**（`Ready()||!reason_.empty()` = `false||true`），调用方以为重新创建成功。
- 触发：WebView2 创建失败（运行时缺失、环境回调失败、控制器失败）→ 面板显示占位页与"重试"按钮 → 用户点重试 → `Panel::Retry()` 置 `webStartAttempted_=false` 后再 `EnsureWebView()` → `web_.Start()` 假成功 → `showingPlaceholder_=false; error_.clear()`，面板停在"AI 助手正在创建浏览器视图…"且**永远不会再真的重试**（`webStartAttempted_` 又被置 true，按钮点了也没用）。这是面板白屏时唯一的自救入口。
- 修复：`WebViewImpl::Stop()` 里补 `startIssued_ = false; reason_.clear(); parent_=nullptr; url_.clear();`；并把 `Start()` 的幂等判定改成显式状态机（`kIdle/kCreating/kReady/kFailed`），失败态允许重新进入 `kCreating`。

**A-5｜ai_server.cpp:37 / 226 / 236 / 333 / 368-371 —— `g_pipe` 跨线程裸读写（严重，UB）**
- 问题：`HANDLE g_pipe` 是普通全局变量。监听线程写它（226 创建、236/333 置 INVALID），UI 线程在 `StopServer()` 里读（368）并 `CancelIoEx`/`DisconnectNamedPipe`（369-370），`join` 之后又写（383）。无锁无原子 → 数据竞争；极端时序下 UI 线程拿到的句柄已被监听线程 `CloseHandle` 并可能被系统复用，`CancelIoEx` 会作用到**别的对象**上。
- 修复：改 `std::atomic<HANDLE> g_pipe`；关闭句柄只由监听线程做，UI 线程只做 `load()` + `CancelIoEx`（幂等，句柄失效时返回 FALSE 无害）。或干脆用 `HANDLE` + 一把 `std::mutex` 保护创建/关闭/取用三处。

**A-6｜ai_server.cpp:368-371 —— 用 `CancelIoEx` 打断同步 `ReadFile`（严重，且与工程自述矛盾）**
- 问题：注释写"打断阻塞中的 ConnectNamedPipe / ReadFile"，但 `CancelIoEx` 只对 **overlapped** 操作有效——`mcp_stdio.cpp:140-142` 的注释自己就写着"`CancelIoEx` 对**同步**读无效（它只管 overlapped 操作），所以要用 `CancelSynchronousIo`"。这里管道是 `PIPE_WAIT` 同步句柄。实际起作用的只有 `DisconnectNamedPipe` 和自连唤醒；一旦某条路径两者都没兜住（例如客户端已连、正在 `ReadFile` 阻塞等下一行、`DisconnectNamedPipe` 未按预期让读返回），`join()` 会**无限等**（注意 A-2 的 `Submit` 那条只覆盖"正在等 UI"的情形，覆盖不到"正在等客户端字节"）。
- 修复：按 `mcp_stdio` 的既有做法，`StopServer` 里记录监听线程 ID 并 `CancelSynchronousIo(OpenThread(THREAD_TERMINATE,...))`，或在 `CreateNamedPipeW` 上加 `FILE_FLAG_OVERLAPPED` 改异步模型（改动大，优先前者）。

**A-7｜ai_server.cpp:226-228 + mcp_stdio.cpp:196 —— 单实例管道被长连接独占，第二个客户端必然 `ERROR_PIPE_BUSY`（严重，功能互斥）**
- 问题：`CreateNamedPipeW(..., nMaxInstances=1, ...)`，而 `mcp_stdio` 在 `BridgeBody()` 里**一次连接用到底**（`hPipe` 持有到桥接结束）。此期间 AI 面板的 node/powershell 桥、任何脚本、第二个 MCP 客户端都会拿到 `ERROR_PIPE_BUSY(231)`；`ConnectPipe` 只重试 2 秒（mcp_stdio.cpp:29/124-134）就放弃。
- 触发：以 `--mcp-stdio` 启动（客户端拉起），同时用户打开 AI 面板聊天 → 面板侧请求全部失败。
- 修复：`nMaxInstances` 至少放到 4~8（名字已按 hwnd+pid 唯一，实例数不涉及多实例串门）；或面板服务改为"加锁串行 + 多实例"的常规做法。

### B 级：明确的缺陷或与约定冲突（不影响整条链路生死）

**B-1｜aichat_panel.cpp:477-617 —— DPI 硬编码像素，全文件 0 处 `theme_->S()`（中）**
- 问题：占位页与"重试"按钮的几何全是裸像素：`barH = 30`（477）、`tr.left += 12`/`right -= 12`（486-487）、`area.left += 20`/`right -= 20`/`top += 30+16`（557-559）、`y += h + 6`（582）、空行 `y += 8`（584）、诊断行 `dr.bottom - 34`（602）、按钮 `w=96/h=28`（307-308）、`rc.left+20`、`rc.bottom-84`（309-310）、可用性阈值 `120/80`（305）。`grep -c "S(" src/aichat_panel.cpp` = **0**。
- 对比约定：`theme.h:199` 提供 `int S(int px) const { return MulDiv(px, dpi, 96); }`，`theme.h:204-207` 还专门把图标尺寸收成令牌并注明"调用方一律用这几个，不再各自 S(裸数字)"。同目录自绘文件普遍遵守：`bookmark_bar.cpp` 26 处、`nav_bar_draw.cpp` 18 处、`prompt.cpp` 16 处、`settings_dialog_draw.cpp` 31 处。
- 触发：150%/200% 缩放下——标题条只有约 15/22 逻辑像素高（文字被裁）、按钮 48×14 DIP 点不中、文案左边距过窄、诊断行与正文重叠。且 `aichat_panel` 没有 `WM_DPICHANGED` 处理（只有外部调 `NotifyParentMoved`），换 DPI 后不会重排。
- 修复：全部换成 `theme_ ? theme_->S(x) : x`（面板生命周期内 `theme_` 由 App 保证，可直接用），或按 `theme.h` 的做法先落成 `barH96 / btnW96 / btnH96 / pad96` 之类令牌再 `S()`；`RetryButtonRect` 与 `OnHostPaint` 必须**同源**（现在已经是同一函数，改一处即可）。

**B-2｜ai_http.cpp:211-218 —— 取消最长 180 秒才生效（中）**
- 问题：`cancel.IsSet()` 只在两次 `WinHttpQueryDataAvailable` 之间检查，而该调用会阻塞到接收超时（`WinHttpSetTimeouts(..., 180000)`，135 行）。
- 触发：用户点"停止生成"，服务端恰好在长思考/慢吐字 → 界面"停止"按钮看起来没反应，最长 3 分钟。
- 修复：置 cancel 时从另一线程 `WinHttpCloseHandle(request)`（会让阻塞读立刻返回错误）或用 `WinHttpSetStatusCallback`；至少把 receive 超时降到 20~30s 并在超时后用 `avail==0` 之外的分支重试（SSE 保活帧证明连接还活着）。

**B-3｜ai_http.cpp:228-251 —— 没收到 `[DONE]` 也报 `ok = true`（中）**
- 问题：循环只因 `avail == 0`（对端关闭）或 `parser.done()` 退出，退出后无条件 `result.ok = true`。若服务端返回 200 + 非 SSE 正文（很多网关在限流/参数错时如此），`SseParser` 一帧事件都不产出，结果仍是"成功且空回复"。
- 触发：被网关拦截、模型名写错返回 JSON 错误体、代理把流截断。
- 修复：`result.ok = parser.done();`，并在 `!ok && error.empty()` 时给一句"流未正常结束（未收到结束标记）"；同时对 200 响应校验 `Content-Type: text/event-stream`（现在完全没查）。

**B-4｜ai_sse.cpp:163-169 —— `calls_.resize(idx + 1)` 无上限（中，可被打成 OOM）**
- 问题：`idx` 直接来自服务端 JSON（`tg.GetInt(L"index", 0)`，只挡了负数）。`index: 100000000` → `std::vector<PartialCall>` 申请约 100M × sizeof(PartialCall)，抛 `bad_alloc` 未捕获 → `std::terminate`。
- 触发：恶意/故障网关；也算"服务端数据当可信"的边界问题。
- 修复：`if (idx < 0 || idx >= 32) continue;`（OpenAI 并发 tool_calls 极少超过个位数），或 `resize(std::min(idx+1, kMaxCalls))`。

**B-5｜aichat_service.cpp:192-197 —— `AssignProcessToJobObject` 返回值被丢弃（中）**
- 问题：只有 `MakeKillOnCloseJob()` 的返回值被检查，`AssignProcessToJobObject(g_job, g_pi.hProcess)` 的成败没人看。若失败（父进程已在不允许嵌套的 job 里、权限受限），`aichat_service.h:13-15` 声称的第一重保证（"本进程被强杀，系统连带结束子进程"）**静默失效**，只在"父进程还能正常退出"时靠第二重兜住。
- 修复：检查返回值并 `LogDbg`；失败时把 `g_job` 关掉（现在是泄漏一个 job 句柄），并降级说明"仅靠显式 Stop"。顺带 `MakeKillOnCloseJob` 失败时 `g_job` 非空但未 `CloseHandle`（88-96）。

**B-6｜ai_queue.cpp:27-32 —— 超时即从队列摘除，与文档契约相反（中，语义问题）**
- 问题：`ai_queue.h:18-19`、`ai_queue.h:41`、`ai_server.cpp:159-163` 三处都写"提交方超时走人，UI 线程**仍会把那个请求处理完**"。实现却在超时分支执行 `queue_.erase(it)`，于是**还没被取走的请求被直接丢弃，命令根本没执行**。同时 `Abandoned()` 只统计"已被取走并跑完"的（`Pump` 里 `if (job->abandoned) ++abandoned_`），被丢弃的根本不计入——`PumpRequests` 的"界面卡了多久"的账少算一大块。
- 语义风险：两份文档说 A 行为、代码做 B 行为。按文档语义（继续执行）会出现"客户端已收到 timeout，但命令仍然改了用户目录"；按代码语义（丢弃）则是"超时 = 静默取消"。必须选一个并统一。
- 修复：若采用"丢弃"，改三处注释并把丢弃计数单独暴露（`Discarded()`）；若采用"照跑"，则超时分支**不要** erase，只置 `abandoned`。

**B-7｜ai_server.cpp:254-256（配合 273-278）—— `line_too_large` 分支不可达，超长行表现为无解释断链（中）**
- 问题：`feed()` 在 `acc.size() + read > kMaxLineBytes` 时直接 `return false` → 外层 `break` → 断开连接。因此 `processLine` 里的 `line.size() > kMaxLineBytes` 永远为假（`line` 必然 ≤ `acc.size()` ≤ 64KB），那条 `{"ok":false,"error":"line_too_large"}` 是**死代码**。客户端看到的是"发大了就被踢掉"，没有任何结构化错误。
- 修复：把限长判定移进 `feed()` 的切行循环（对单行长度判定，超长时 `send(错误响应)` 后 `return false`），或把 `feed` 的累积上限放宽到 `2*kMaxLineBytes` 让 `processLine` 有机会回错误。

**B-8｜aichat_webview.cpp:427-447 + 560-564 —— 挂起的完成回调持 `owner_` 裸指针，面板销毁后可能 UAF（中）**
- 问题：`new CompletedHandler(this, ...)` 交给 WebView2（它持一份引用），回调里 `owner_->OnEnvironmentReady(...)`。`Stop()` 只是释放接口，**没有作废挂起的 handler**；`Panel::Destroy()`（aichat_panel.cpp:622-633）销毁 `WebViewImpl` 之后，若环境/控制器回调才送达，`Invoke` → 访问已析构的 `WebViewImpl`（`reason_` 是 `std::wstring`，会写到已释放内存）。
- 触发：用户快速展开后立即关闭面板/退出程序，而 WebView2 环境创建是异步的（这正是本文件反复强调"回调靠消息循环派发"的同一机制）。
- 修复：`WebViewImpl` 加 `std::shared_ptr<bool> alive_`（或用 `std::atomic<bool> cancelled_`），`Stop()` 置真，`Invoke` 里判后再调；更稳的做法是 `Stop()` 后 `PostQuitMessage` 之前不再有回调，但那需要同步销毁契约，代价大。

**B-9｜ai_api.cpp:46-152 —— schema 校验的两处盲区（中）**
- 问题 1：类型判定只认单值 `spec->GetStr(L"type")`。JSON Schema 允许 `"type": ["string","null"]`，此时 `GetStr` 拿到空串 → `want` 为空 → 整段类型检查**静默跳过**，任意类型都通过（与"fail-closed，不兜底"的设计口径相反）。数组 `items` 同理。
- 问题 2：整数判定 `val.num != static_cast<double>(static_cast<long long>(val.num))`（90 行、114-115 行）。C++ 里 double→整型的越界转换是 UB；`{"n": 1e30}` 这类输入走的是未定义路径（MSVC 上通常得到 `LLONG_MIN`，于是被误判为"不是整数"而拒绝——恰好偏安全，但仍是 UB）。
- 修复：类型判定支持数组形式（任一命中即通过，`null` 单独放行）；整数判定改 `std::isfinite(val.num) && std::fabs(val.num) <= 9.2e18 && val.num == std::floor(val.num)`。

**B-10｜aichat_panel.cpp:275-285 —— `Retry()` 同步调 `EnsureWebView()`，违背本文件自述规则（中）**
- 问题：`Show(true)` 与 `Create` 都刻意用 `PostMessageW(WM_APP_AI_PANEL_CREATE)` 延后创建，理由写在 109-111 行与 220-225 行："这一帧消息循环还在命令处理器里，WebView2 的异步回调送不到"。但 `Retry()` 在 `WM_LBUTTONDOWN`（363-377）这个"命令处理器内部"直接调 `EnsureWebView()`。同一份约束，两条路径两套做法。
- 修复：`Retry()` 改成 `ShutdownWebView(); webStartAttempted_=false; ... ; PostMessageW(host_, WM_APP_AI_PANEL_CREATE, 0, 0);`（与首次创建同一条路径），并重置 `webCreateRetries_`。

**B-11｜ai_server.cpp:600-617 —— 通知（无 id）也会收到错误响应（中，协议）**
- 问题：JSON-RPC 2.0 §4.1 要求通知**不得**有响应；这里对每条通知都回 `{"jsonrpc":"2.0","id":null,"error":{...}}`（注释 601-602 承认这是刻意偏离）。MCP 客户端在 `initialize` 后会发 `notifications/initialized`，严格实现的客户端会把它当成"服务端发了非法响应"记错甚至断开。
- 修复：`id == nullptr && method 为空`/通知类方法只记日志不回包（`HandleRpcLine` 返回空串，`processLine` 对空串不发）。

**B-12｜mcp_stdio.cpp:41 / 169 / 323 —— `g_thread_id` 跨线程非原子（中，UB）**
- 问题：桥线程写（169）、UI 线程读（`CancelBridgeIo` 147-153）、`Stop()` 里再清零（323）。非 `std::atomic`。另外 `Start()` 只等 `g_ready` 2 秒（289）就返回 true，万一超时，`g_thread_id` 仍是 0 → `Stop()` 的取消循环全是空打，只能靠 2 秒后 `detach` 兜底（313-319）。这正是"stdio 桥线程状态"是否需要 ai_server 例外待遇的问题——**结论：不应该例外，它和 `ai_server` 一样需要原子化**，因为它客观上被两个线程访问。
- 修复：`std::atomic<DWORD> g_thread_id{0}`；`Start()` 用 `WaitForSingleObject(g_ready, INFINITE)` 或返回 false 表示"没起来"，不要"超时了也报成功"。

**B-13｜mcp_stdio.cpp:237-245 —— "主动取消读"与"stdin EOF"混为一谈，误取消即整个程序退出（中）**
- 问题：`readLine` 返回 false 的三种原因（EOF、读错误、被 `CancelSynchronousIo` 取消）走同一条路：`CloseHandle` → `PostMessageW(g_main, WM_CLOSE)`。`Stop()` 的取消循环（305-309）会**故意**取消读——若此时桥接正阻塞在 stdin 上，就会在"应用正在关闭"的路径上再投一次 `WM_CLOSE`。现在因为顺序（`mcpstdio::Stop()` 在 `ai::StopServer()` 之前、窗口还在）暂时无害，但任何"想在运行中重连 stdio"的改动都会立刻变成"取消一次就把程序关了"。
- 修复：加 `std::atomic<bool> g_stopping`，`Stop()` 前 `g_stopping=true`；`BridgeBody` 末尾 `if (!g_stopping && eof) PostMessage(...)`（用 `ReadFile` 的 `ERROR_BROKEN_PIPE`/`n==0` 明确区分 EOF 与"被取消"）。

**B-14｜ai_events.cpp:203-210 —— 每条事件在全局锁内同步落盘（中，性能）**
- 问题：`PublishImpl` 持 `g_mu` 期间做 `auditlog::NowIso()` + `tf::AppendLine`（含旋转检查，每 200 条 `GetFileAttributesEx`）。`Publish` 的调用方是 UI 线程（每条命令一次，见 ai_api.cpp:417）与 `state.changed` 轮询（每 100ms 一次，含整份状态快照）。同时 `WaitFor`（订阅推送）也抢这把锁 → 磁盘慢时事件推送直接卡住。
- 修复：把落盘挪出锁（用独立的写线程或 `std::deque<std::string>` 待写队列 + 单写者），锁内只保留 seq/内存环形缓冲；顺序性由队列保证，注释里"seq 大的先写进文件"的担心由单写者自然解决。

**B-15｜ai_api_json.cpp:38-41 vs ai_server.cpp:688-706 —— 同名字段 `content` 两种类型（中）**
- 问题：旧格式 `ResultToJson` 里 `content` 是**字符串**（`Str(json::Stringify(...))`）；MCP 路径里 `content` 是**数组** `[{type:"text",text:...}]`。同名不同型，任何"按字段名统一解析两种响应"的客户端都会在类型断言处炸。
- 修复：旧格式改用 `contentText` / 直接只给 `structuredContent`；或在旧格式里也给数组形状（MCP 形状是超集，客户端只会更省事）。

### C 级：可维护性 / 轻微偏离（建议修，不阻塞）

**C-1｜ai_http.cpp:125 —— UA 品牌串硬编码**
`WinHttpOpen(L"ExBarBrowser/1.0 (AI panel)", ...)`。拼写与 `identity.h:20 kBrand = L"ExBarBrowser"` 一致，但没引用它（该文件也没 include `identity.h`）。工程口径是品牌串只在 `identity.h` 定义（`kBrand`/`kBrandShort`/`kLogoText`）。修复：`std::wstring ua = std::wstring(id::kBrand) + L"/1.0 (AI panel)";`。

**C-2｜ai_api.h:240 / ai_api_json.cpp:101 —— 字段名与线协议不一致**
`StateSnapshot::selectionPaths` 在 JSON 里叫 `selection`（`v.Set(L"selection", sel)`）。按头文件字段名找键的调用方（含自断言左值）会拿不到值。修复：JSON 键改 `selectionPaths`，或头文件字段改名 `selection`。

**C-3｜ai_sse.cpp:11-37 —— `carry_` 无上限**
对端持续发不含 `'\n'` 的字节流时无限增长（`Feed` 无任何长度检查）。`ai_server` 侧对"一行"有 64KB 纪律，SSE 侧没有，口径不一致。修复：`if (carry_.size() > 1u<<20) { carry_.clear(); reason_=...; }` 或直接标记流异常。

**C-4｜mcp_stdio.cpp:49-63 / 92-115 —— `LineReader::acc` 无上限；`params` 数组被静默重建**
`acc` 同样无上限；`InjectToken` 在 `params` 不是对象时（JSON-RPC 允许位置参数数组）把它替换成只含 `__token` 的空对象，**位置参数全部丢失**且不报错。MCP 实务都用对象，风险低，但静默丢参数违背"不兜底"的口径——至少该拒绝并回 `-32602`。

**C-5｜ai_events.cpp:109-118 —— `EventLine` 把 data 的键平摊到顶层**
`if (e.data.IsObject()) for (kv) v.Set(kv.first, kv.second)`：data 里若出现 `seq` / `event` / `type` / `ts` 就会**覆盖信封字段**，客户端看到的序号/类型是假的。修复：保留 `data` 子对象（客户端多拆一层），或平摊时跳过保留字。

**C-6｜ai_events.cpp:303-304 / 318-327 —— `Since()` 的 limit 与 gap 语义**
`limit > kMaxEvents` 时**静默改成 256**（`kDefaultLimit`），既不报错也不 clamp 到 512 —— 请求 `limit:1000` 的客户端会以为拿到了全部。`gap` 判定用 `first`，但 `first` 只在"有后续事件"时才被赋值，于是"nothing new + 历史已被淘汰"时 `firstAvailable` 输出 0（无意义）。修复：`limit = std::min(limit, kMaxEvents)`；`firstAvailable` 直接取 `g_events.front().seq`。

**C-7｜aichat_service.cpp:100 —— `PickFreePortForTest()` 是头里没声明的外部符号**
只在本 .cpp 定义，`grep` 全仓无调用者、`aichat_service.h` 也无声明 → 隐式外部链接的死代码/边界泄漏。修复：删除，或移进匿名命名空间并加 `#ifdef` 测试开关。

**C-8｜aichat_service.cpp:102-108 / 236-254 —— `Enabled()` 不看进程是否还活着，也没有重启**
`if (g_started) return true;` 与 `Enabled() { return g_started; }`：node 崩了之后 `Current()` 能算出 `running=false`（241-247），但**没人复位 `g_started`**。于是 `Panel::EnsureWebView` 的"服务未就绪"判定永远为假，面板一直往死端口导航；也没有任何重启尝试。修复：`Current()` 里发现退出就把 `g_started`/`g_port` 清掉（`Stop()` 复用），并给面板一个"重启服务"的动作。

**C-9｜ai_server.cpp:71-76 —— 令牌兜底熵不足**
`memcpy(raw, &t, sizeof(t))` 只填 `raw[0..7]`（16 字节缓冲里后 8 字节恒为 0），兜底令牌实际只有 64 位时间+计数器。修复：`memcpy(raw + 8, &t, sizeof(t))` 或填两处。

**C-10｜ai_server.cpp:611-614 —— `bad_token` 用 -32600**
`kInvalidRequest`（"请求本身不合法"）语义不符：令牌错是**鉴权失败**，不是格式错。同文件 194 行注释自己说 `-32xxx` 才是实现自定义区间。修复：`-32001 bad_token`（并在客户端侧同步文档）。

**C-11｜ai_server.cpp:48-52 —— `UiTimeoutMs()` 无上限防御**
600000 的上限只存在于 `config_io.cpp:227` 与 `settings_reg_table.cpp:430-432`；`ai_server` 自己 `return v > 0 ? v : Settings{}.aiTimeoutMs;`，任何直接写 `mutableSettings()` 的路径（测试、将来的导入）都能塞进 10^9。修复：`return std::clamp(v > 0 ? v : Settings{}.aiTimeoutMs, 1000, 600000);`。

**C-12｜ai_api.cpp:408-417 —— 事件里带完整 args 且长期落盘**
`data.Set(L"args", args)` → `events::Publish` → `events.jsonl` 长期保留（2MB 轮转）。路径、搜索词、可能的重命名内容都进去了。这是审计需要，但值得在 `docs/AI-INTERFACE.md` 里写清"事件文件含完整参数"，并考虑对 `destructive` 命令做参数摘要。

**C-13｜aichat_panel.cpp:426-430 / 464-466 —— 每次 WM_PAINT 写 3 条日志**
`OnHostPaint` 每次重绘都 `LogDbg` 尺寸/ready/err + 颜色/font/theme。拖动、折叠、WebView2 尚未就绪时的重绘风暴会把日志灌满（且 `MeasureTextHeight` 每段一次 `DT_CALCRECT`）。修复：日志降级为"状态变化时才打"（用成员记上一次的 `w×h`/`ready`）。

**C-14｜文档漂移（多处，低）**
- `ai_server.h:13`："每个连接先发 HELLO，校验令牌" —— 实现里**没有 HELLO**，是第一行请求里带 token。
- `ai_server.h:35-36`："只把请求 PostMessage 到主窗口" —— 已改为 `aiqueue` + 条件变量。
- `ai_server.cpp:707`："`json::Value::Set` 是 move 语义，先 Set 会把它搬空" —— `json.h:36` 是 `void Set(const std::wstring& key, Value v)`，按值传参，传左值 `content` 是**拷贝**，不会被搬空（代码顺序仍是对的，注释理由不成立）。
- `ai_server.cpp:24` `#include <cstdlib> // rand_s`：`rand_s` 已不再使用（`MakeToken` 改用 `SystemFunction036`），include 与注释都过期。
- `ai_api_json.cpp:175` `std::wstring ToolsToJson() {    json::Value root = ...`：函数体挤在签名同一行。
- `aichat_panel.cpp:185`（`// ...通知它。    // 没有暴露...` 两句注释粘一行）、`220-226`（注释掉到 0 列、`const aisvc::Status st` 也掉到 0 列）、`616-617` 括号缩进 —— 该文件多处缩进/注释断裂，建议 `clang-format` 过一遍。

---

## 三、评价

**整体**：这组代码的**设计文档密度**是我在这个仓库里见过最高的一档——几乎每个非平凡决定都写了"为什么不能那样"和实测证据（D14 白屏、`CompletedHandler` 槽位 3、`PeekNamedPipe` 断连导致 err=231、`tf::Join` 尾分隔符）。绝大多数注释是**可信的**（我逐条对着实现核过），这对审查帮助极大。命令层"元数据驱动 + fail-closed + 自断言 + verdict"的口径统一，`ai_events` 的三级轨迹也确实是设计出来的而不是事后补的。

**真正的问题集中在两类**：

1. **状态机的"退出/复位"路径没有闭环**。`ai_queue::interrupted_` 只置不清（A-1）、`WebViewImpl::startIssued_` 只置不清（A-4）、`aisvc::g_started` 只置不清（C-8）——三个独立的粘性标志，症状一致：**首次失败后永久失效，而且从外部看一切正常**（响应照样回、日志照样打）。这类 bug 在一个"可热插拔、可重试"的自述设计里是硬伤，且三条都指向同一个工程习惯：状态只写了进入态，没写离开态。建议给每个跨调用存活的状态机补一条"如何回到初始态"的注释与测试，`tools/` 下也值得加一个 `--self-check` 的幂等演练。

2. **线程契约的边角**。`g_pipe` 裸句柄跨线程（A-5）、`CancelIoEx` 用在同步句柄上（A-6，且与 `mcp_stdio` 自己的注释矛盾）、`Submit` 等 UI 与 `StopServer` join UI 构成互等（A-2）。这三条单看不致命，组合起来就是"关窗口偶发卡死/偶发闪退"的经典现场。这类问题不需要重构，只需要把 `ai_server` 里那 5 个 `g_` 全局按"谁写谁读"列一张表贴到文件头——现在的注释写的是"线程模型（关键，别改坏）"，但缺的正是这张表。

**最该先修的三个**：A-1（热插拔关→开必坏，用户可直达）、A-3（未鉴权订阅，安全口径被绕过）、A-4（面板唯一自救入口失效）。这三个都是"用户在界面上点两下就能撞到"的级别，且都是小改动（各 3~10 行）。

**行数**：`ai_server.cpp` 775 行已接近该文件逻辑复杂度的上限。若后续再往里加协议分支，建议按已记账的拆分惯例拆出 `ai_server_rpc.cpp`（JSON-RPC 2.0 那一半本来就是独立的一层，`HandleRpcLine` 到文件尾正好 170 行，边界天然）。

**结论**：架构与协议形状是对的，问题几乎全部落在"生命周期收尾"与"跨线程句柄"两个面上，属可以在不动骨架的前提下修完的一档。修完 A 组 7 条后可认为该组达到可发布质量；B 组建议排进同一次迭代（B-1 DPI 与 B-3 `ok` 误报值得优先，都是用户能看见的）。
