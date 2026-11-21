---
status: current
baseline: 2026-11-21 19:15
---

# AI 操作接口

> 目标：本程序由人与 AI 共同操作。人能通过鼠标/键盘做的事，AI 通过接口也能做；
> 反过来，接口能做的事人也能做（不存在只有 AI 能触发的动作）。
>
> **AI 客户端不要试图"模拟点击"**：本接口就是为此存在的。命名管道 `tools/call`
> 走的是同一条 `CommandBus`，比合成输入更可靠（不受布局/DPI/焦点影响）且可审计。
> 合成鼠标键盘（含向窗口投 `WM_LBUTTON*`/`WM_KEY*`）在架构规则 R21 里全仓禁止
> —— 见 [ARCHITECTURE-RULES.md §3.1](ARCHITECTURE-RULES.md)。

代码位置：

| 文件 | 职责 |
|---|---|
| `src/commands.h/.cpp` | 命令表 + AI 元数据（`risk` / `argsSchema` / `aiExposed` / `needsSelection`） |
| `src/ai_api.h/.cpp` | 工具投影、参数校验、派发、状态快照 |
| `src/ai_server.h/.cpp` | 命名管道通道、令牌校验、请求泵 |
| `tools/check_ai_meta.py` | 静态对账（暴露/参数/选中项三方一致） |

---

## 1. 三条铁律

1. **唯一执行入口**：AI 的所有动作都走 `CommandBus`，与工具栏、快捷键、命令面板
   是同一条路径。不存在"给 AI 单独写一份删除实现"——否则两边行为迟早分叉。
2. **元数据驱动**：命令表里的 `aiExposed` 是唯一真源。`ai_api` 只读表，不硬编码
   任何命令知识。新增命令时改表即可，不用改接口层。
3. **失败关闭**：未暴露的命令拒绝、参数不合规拒绝、危险命令未经双门拒绝。
   宁可少做，不做"尽力而为"的猜测。
4. **失败可机器分类**：每个拒绝都带 `errorCode`（见 §3 错误码表）。中文 `error` 只给人读，
   客户端按码决策 —— 否则"补命令 / 补暴露 / 改参数 / 要审批"这四件事只能靠解析文案区分，
   一次文案调整就会让所有客户端走错分支。
5. **能力面自述**：`exbar/capabilities` 直接由命令表算出总数、已实现、已暴露与两张缺口清单。
   "缺什么"由产品回答，不由客户端 grep 源码推断。

---

## 2. 命令能暴露给 AI 的条件

一条命令要出现在 `tools/list` 里，必须同时满足：

| 条件 | 原因 |
|---|---|
| `implemented == true` | 未实现的命令调用只会失败，暴露出去是噪音 |
| `aiExposed == true` | 显式登记，不是默认开放 |
| `argsSchema` 是合法 JSON | MCP `inputSchema` 直接复用；缺失会让客户端 schema 校验失败 |
| `needsSelection == false`，**或** `needsSelection == true` 且观测面能读选中 | 依赖当前选中项的命令，AI 必须先能"看见"选中才能用 |

**needsSelection 例外（2026-11-21）**：`bmk.add.selection` 是第一条带 `needsSelection=true` 暴露的命令。
前提是观测面已能读回选中（`state` 返回 `selectionCount` / `selection` / `selectionFolders` /
`selectionTruncated` / `focus`），AI 先读快照确认选中的是哪个文件夹，再调用收藏。
`check_ai_meta.py` 第 4/14 项守这条规则：观测面字段不在源码里时，needsSelection 命令一律收紧为不暴露。

**为什么不暴露其余依赖选中项的命令**（`CmdDelete` / `CmdRename` / `CmdCopy` / `CmdPaste`…）：
它们的语义是"对用户当前选中的东西做什么"。AI 只知道自己想删什么，不知道用户此刻
选了什么 —— 暴露出去的结果就是"AI 以为删了 A，实际删了用户选中的 B"。要支持这类
操作，正确做法是新增能**用参数指定目标**的语义命令（mod 的 `ebop` 层），而不是
把 UI 命令硬暴露出去。

同理不暴露的还有：弹系统对话框的（`FolderOptions` / `Preferences` / `MapDrive`
——阻塞且语义不可控）、改进程形态的（`win.quit`（原名 `QuitExplorer`） / `NewWindow`）、写剪贴板的
（`PathCopy*` ——修改外部状态却不产生可回退的记录）。

**暴露条数以 `python tools/check_ai_meta.py` 的第二行为准**（它会打印"其中 aiExposed：N 条"），
本文不写死数字 —— 加一条命令就变，写进来的值第二天就是错的。
覆盖导航、标签、视图、书签、文件夹操作（确定式 `Fs*`）、目录统计（`Op*`）与会话。
完整清单以 `tools/list` 为准，静态判据见 `tools/check_ai_meta.py` 第 14 项（暴露决策）
与第 15 项（R20 人机等价）。

---

## 3. 通道协议

本地命名管道，一行一个请求（UTF-8 JSON），一行一个响应。

**默认开启**（2026-11-21 起）。`data\settings.json` 的 `"aiEnabled": false` 才关闭。
**支持热插拔**：设置面板 / `config.set` 改 `aiEnabled` 立即启停监听，无需重启程序。

启动后写发现文件（同目录）：

| 文件 | 内容 |
|---|---|
| `ai-pipe.txt` | 完整管道名（`\\.\pipe\ExBarBrowser_ai_<窗柄>_<pid>`） |
| `ai-token.txt` | 本次运行的随机令牌（16 字节 hex） |
| `ai-pipe.<pid>.txt` / `ai-token.<pid>.txt` | 每实例一份的副本（多实例并存时枚举用） |

令牌每次启动重新生成，进程退出即失效。客户端必须先读这些文件再连接。
多实例并存时，规范名 `ai-pipe.txt` / `ai-token.txt` 指向**最后启动**的实例；
要枚举全部在监听的实例，遍历 `ai-token.*.txt`（pid 后缀）。实例退出时只删
属于自己的发现文件，不会误删其他实例的。

### 协议有两种，靠有无 `jsonrpc` 字段区分

**推荐：JSON-RPC 2.0（MCP 兼容）。** Claude Desktop / Cursor 这类 MCP 客户端
可以直接连，不需要中间层。

```jsonc
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"__token":"..."}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05",
     "serverInfo":{"name":"exbarbrowser","version":"1.0"},
     "capabilities":{"tools":{"listChanged":false}}}}

→ {"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"__token":"..."}}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[...]}}

→ {"jsonrpc":"2.0","id":3,"method":"tools/call",
   "params":{"name":"fs.rename","arguments":{"target":"D:\\a.txt","newName":"b.txt"},
             "__token":"...","approve":true}}
← {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"..."}],
     "isError":false}}
```

```jsonc
// 非 MCP 标准方法，用厂商前缀避免与将来标准冲突
→ {"jsonrpc":"2.0","id":4,"method":"exbar/state","params":{"__token":"..."}}
← {"jsonrpc":"2.0","id":4,"result":{"activePath":"D:\\","tabs":[...]}}

// 能力面自描述：命令总数 / 已实现 / 已暴露 + 两张缺口清单
// 为什么需要它：tools/list 只含"已暴露"，客户端看到"没有这条命令"时分不清
//   ① 表里没有（要新增）② 有实现但没开放（补元数据）③ 有表项没 handler（写实现）
// 三种修法完全不同，靠猜必然猜错。答案由产品直接给出，客户端不 grep 源码。
→ {"jsonrpc":"2.0","id":5,"method":"exbar/capabilities","params":{"__token":"..."}}
← {"jsonrpc":"2.0","id":5,"result":{"total":168,"implemented":160,"aiExposed":135,
     "notImplemented":["tab.merge","view.toggle.checkboxes"],
     "notExposed":["file.delete","file.rename"]}}
```

**令牌**：MCP 没有这个概念，所以放在 `params.__token`（也接受顶层 `token`）。
这是本地护栏 —— 不防同用户下的恶意进程（它能直接读令牌文件），
防的是"别的程序随手乱调"。

**错误分两类**（这是 MCP 规范的硬要求，也是客户端能否正确应对的关键）：

| 情形 | 返回 | 客户端该做什么 |
|---|---|---|
| 请求不合法（版本错/缺 method/令牌错） | JSON-RPC `error` + `code` | 改请求或修令牌 |
| 工具执行失败（缺参数/未知工具/命令返回 false） | `result.isError = true` | 换参数或换工具 |

混在一起的话，客户端分不清"我调错了"和"环境不满足"，只能盲重试或盲放弃。

**令牌检查在方法分派之前**：否则能靠"返回 method-not-found 还是 bad-token"
枚举出有哪些方法。未授权时，所有请求的响应完全一致。

### 旧格式（既有本地脚本在用，保留）

```jsonc
// 列出工具
→ {"token":"...","method":"tools/list"}
← {"ok":true,"tools":[{"name":"NavBack","title":"后退","group":"Nav",
                       "risk":"write","needsSelection":false,"inputSchema":{}}]}

// 查状态
→ {"token":"...","method":"state"}
← {"ok":true,"activePath":"D:\\Windhawk","activeTab":0,
   "tabs":["D:\\Windhawk"],"itemCount":15,"viewMode":3,
   "selectionCount":1,"selection":["D:\\Windhawk\\src"],"selectionFolders":["D:\\Windhawk\\src"],
   "selectionTruncated":false,"focus":"file_view"}

// 调用命令
→ {"token":"...","method":"tools/call","name":"ViewMode","args":{"preset":5}}
← {"ok":true,"command":"ViewMode","risk":"write"}

// 危险命令
→ {"token":"...","method":"tools/call","name":"TabCloseOther","approve":true}
← {"ok":false,"command":"TabCloseOther","risk":"danger","error":"danger_disabled"}
```

### 错误码

`error` 是给人看的中文原因，**`errorCode` 是给机器分支的稳定标识**。
两者同时返回：客户端只读 `errorCode` 就不会被文案改动打断，人仍能读懂发生了什么。

| `error`（中文，示例） | `errorCode`（机器） | 客户端该做什么 |
|---|---|---|
| `invalid_json` | —（JSON-RPC `error.code`） | 改请求 |
| `bad_token` | —（JSON-RPC `error.code`） | 修令牌；**不泄露工具清单** |
| `unknown_method` | — | 改 method |
| `missing_name` | — | 补 `name` |
| `未知或未暴露给 AI 的命令` | `unknown_tool` | 命令表里没有：新增命令 |
| `未知或未暴露给 AI 的命令` | `not_exposed` | 有实现但没开放：补 `aiExposed` |
| `未知或未暴露给 AI 的命令` | `not_implemented` | 有表项没 handler：写实现 |
| `缺少必填参数：xxx` / `该命令不接受参数` | `bad_args` | 改参数（读 `inputSchema`），别改代码 |
| `命令的 argsSchema 非法（实现缺陷）` | `schema_invalid` | 命令表写错：修表 + `check_ai_meta` |
| `danger_disabled` | `danger_disabled` | 用户在设置里关掉了危险命令；这是人的决定 |
| `need_approval` | `need_approval` | 需要人类批准这一次操作 |
| 前置条件原因（如"已经是顶层"） | `blocked` | 改目标或先做前置动作 |
| `命令执行失败` | `failed` | 查日志对应模块行 |
| （handler 成功但断言不符） | `effect_not_met` | 读 `assertions`：修实现或修判据 |
| `timeout` | —（`error: timeout`） | UI 卡在模态对话框：**不要重试**，请人关掉 |

---

## 4. 安全边界

| 层 | 机制 | 防的是什么 |
|---|---|---|
| 通道 | 会话内令牌 + 可热插拔的 aiEnabled | 同机其他进程随手乱调 |
| 连接 | 每进程随机令牌 | 同机其他进程随手乱调 |
| 命令 | `aiExposed` 白名单 | 未审计的动作被触发 |
| 目标 | 拒绝 `needsSelection` 命令 | AI 操作到"用户碰巧选中的"对象上 |
| 危险 | `aiAllowDanger` + `approve` 双门 | AI 单方面执行破坏性操作 |
| 参数 | 严格校验，不兜底 | 参数被静默丢弃导致误操作 |
| 审计 | `CommandBus::Audit` 记 `actor=ai` | 事后无法回溯是谁做的 |

**这不是沙箱**。令牌防的是"别的程序随手调用"，不防同用户下的恶意进程
（它能直接读 `ai-token.txt`）。真正的隔离要靠 Windows 的进程/用户边界。

---

## 5. 人机并发

命令总线是串行的：AI 的请求排队后由 UI 线程的定时器（100ms）取出执行，
人类点击按钮走的是同一条路径。因此：

- AI 不会与人的操作"同时"执行 —— 天然互斥。
- AI 的请求若撞上人类打开的模态对话框，会在 10 秒后超时返回，不会卡死客户端。
- `state` 快照是**现取**的（每次调用都重新读活动标签），不会返回陈旧路径。

---

## 6. 扩展：怎么加一条能被 AI 用的命令

1. 在 `src/commands.cpp` 的表里加一行，填 `Risk`、`argsSchema`、`aiExposed=true`。
2. 若带必填参数：在 `src/ai_api.cpp` 的 `kArgKeys` 加一条（键名必须与
   `argsSchema.required` 完全一致）。
3. 在 `app_commands*.cpp` 里用 `reg(CmdId::X, handler)` 注册实现。
4. 跑 `python tools/check_ai_meta.py` —— 它会检查暴露/参数/选中项是否自洽，
   第 22 项还会校验"每个拒绝都带 `errorCode`、码值不越界"。
5. 跑 `tests/ai_api_test.cpp`。
6. 新增任何拒绝分支时，`r.error` 与 `r.errorCode` 必须成对出现 —— 只给中文等于
   让客户端回到"解析文案"的老路。

> `kArgKeys` 存在的原因：`CommandBus` 的 handler 只收**一个字符串** arg，
> 而 `argsSchema` 是给人和 AI 看的 JSON Schema，两者之间没有机器可读的映射。
> 这张表把"哪个键对应命令的哪个参数"钉死，脚本会验证两边一致。

---

## 7. 验证

```bash
CC="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"

# 工具投影 + 参数校验 + 危险命令门（34 项）
"$CC" -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -D_WIN32_IE=0x0A00 -Isrc tests/ai_api_test.cpp src/ai_api.cpp src/commands.cpp \
  src/json.cpp src/config.cpp src/text_file.cpp -o /tmp/t.exe \
  -lole32 -luuid -lshell32 -ladvapi32 && /tmp/t.exe

# 协议层：非法输入 / 令牌 fail-closed / 不泄露能力面（9 项）
"$CC" -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -D_WIN32_IE=0x0A00 -Isrc tests/ai_server_test.cpp src/ai_server.cpp src/ai_api.cpp \
  src/commands.cpp src/json.cpp src/config.cpp src/text_file.cpp -o /tmp/s.exe \
  -lole32 -luuid -lshell32 -ladvapi32 -luser32 && /tmp/s.exe

# 静态对账
python tools/check_ai_meta.py
```

手工验证通道（PowerShell）：

```powershell
$pipe  = Get-Content "$exe\data\ai-pipe.txt"
$token = Get-Content "$exe\data\ai-token.txt"
$c = New-Object System.IO.Pipes.NamedPipeClientStream('.', ($pipe -replace '^\\\\\.\\pipe\\',''), 'InOut')
$c.Connect(2000)
$w = New-Object System.IO.StreamWriter($c); $w.AutoFlush = $true
$w.WriteLine("{`"token`":`"$token`",`"method`":`"tools/list`"}")
(New-Object System.IO.StreamReader($c)).ReadLine()
```


---

## 8. 导航栏历史的读面与搜索的已知缺口

### 8.1 历史 / 最近访问（2026-11-21 补齐）

地址框下拉里的历史过去只有鼠标能看能点，机器侧只能盲按 `nav.back` 再回读路径。
现在有三条只读/确定式命令：

| 命令 | 参数 | 作用 |
|---|---|---|
| `nav.history.list` | `{backward?: bool}` | 列出可后退/可前进的条目（最近的在前），带 `count` |
| `nav.history.jump` | `{depth: int, backward?: bool}` | 跳到第 N 条；返回 `{from,to}`，`to` 取自历史条目 |
| `nav.history.clear` | 无 | 破坏性，需 `aiAllowDanger` + `approve`；返回 `{path,recentCleared}` |
| `nav.recent.list` | 无 | 地址框下拉里「最近访问」的结构化读面 |

`nav.history.jump` 的 `to` **不能**在跳转后回读 `activePath`：导航是异步的（Shell 回调才写路径），
立刻读只会读到出发地 —— 这正是"回报成功但内容不对"的典型。它取的是历史条目本身。

`path.copy.dir` / `path.copy.tab` 现在把写入剪贴板的文本一并返回（`structuredContent.text`）。
剪贴板没有读回面，这是刻意不给 AI 读剪贴板（里面可能有密码）的取舍。

### 8.2 搜索：`search-ms:` 宿主不支持（未解决，已如实失败）

`search.find` 与导航栏搜索框共用 `App::RunSearch`，构造 `search-ms:query=…&crumb=location:…`
并交给 `IExplorerBrowser`。**这条链路在本工程里从来没有生效过**：

- `BrowseToObject` 对 `search-ms:` 项返回 `S_OK`，但视图留在原目录；
- 实测：零结果查询（`zzzz-no-such-token`）之后 `itemCount` 与 `tool.list` 条目数一个不少；
- `IExplorerBrowser` 没有 Explorer 的搜索带（search band），协议处理器 URL 不是可浏览 folder。

处理方式（失败关闭，不再假成功）：

1. `ShellView::DoNavigate` 明确拒绝 `search-ms:` 并记日志；
2. `RunSearch` 只在 `Navigate` 成功后写 `searchFrom_` / `searchQuery_`（先记账再导航会留下
   "state 说在搜、视图还在原目录"的假状态）；
3. `search.find` 返回 `isError=true`，`error` 带实际 URL；
4. `state` 新增 `searching` / `searchScope` / `searchQuery`，是搜索态的唯一可回读来源；
   `check_ai_meta.py` 第 17 项已登记 `searching`，缺回读会 FAIL。

真要修，需要换宿主（自带搜索带）或接 Windows Search（`ISearchFolderItemFactory` /
`IQueryParser`）。当前工具链的 MinGW 头里没有 `searchapi.h` / `structuredquery.h`，
要做就得自带声明 —— 属于独立任务，别混在导航修复里。
