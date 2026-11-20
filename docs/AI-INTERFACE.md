# AI 操作接口

> 目标：本程序由人与 AI 共同操作。人能通过鼠标/键盘做的事，AI 通过接口也能做；
> 反过来，接口能做的事人也能做（不存在只有 AI 能触发的动作）。

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

---

## 2. 命令能暴露给 AI 的条件

一条命令要出现在 `tools/list` 里，必须同时满足：

| 条件 | 原因 |
|---|---|
| `implemented == true` | 未实现的命令调用只会失败，暴露出去是噪音 |
| `aiExposed == true` | 显式登记，不是默认开放 |
| `argsSchema` 是合法 JSON | MCP `inputSchema` 直接复用；缺失会让客户端 schema 校验失败 |
| `needsSelection == false` | 依赖当前选中项的命令，AI 无法用参数指定目标 |

**为什么不暴露依赖选中项的命令**（`CmdDelete` / `CmdRename` / `CmdCopy` / `CmdPaste`…）：
它们的语义是"对用户当前选中的东西做什么"。AI 只知道自己想删什么，不知道用户此刻
选了什么 —— 暴露出去的结果就是"AI 以为删了 A，实际删了用户选中的 B"。要支持这类
操作，正确做法是新增能**用参数指定目标**的语义命令（mod 的 `ebop` 层），而不是
把 UI 命令硬暴露出去。

同理不暴露的还有：弹系统对话框的（`FolderOptions` / `Preferences` / `MapDrive`
——阻塞且语义不可控）、改进程形态的（`win.quit`（原名 `QuitExplorer`） / `NewWindow`）、写剪贴板的
（`PathCopy*` ——修改外部状态却不产生可回退的记录）。

当前暴露 **86 条**（137 条目录中已实现 108 条，扣除 22 条依赖选中或纯人机 UI 的命令），
覆盖导航、标签、视图、书签、文件夹操作（确定式 `Fs*`）、目录统计（`Op*`）与会话。
完整清单以 `tools/list` 为准，静态判据见 `tools/check_ai_meta.py` 第 14 项。

---

## 3. 通道协议

本地命名管道，一行一个请求（UTF-8 JSON），一行一个响应。

**默认关闭。** 在 `data\settings.json` 里设 `"aiEnabled": true` 才监听。

启动后写两个发现文件（同目录）：

| 文件 | 内容 |
|---|---|
| `ai-pipe.txt` | 完整管道名（`\\.\pipe\ExBarBrowser_ai_<窗柄>`） |
| `ai-token.txt` | 本次运行的随机令牌（16 字节 hex） |

令牌每次启动重新生成，进程退出即失效。客户端必须先读这两个文件再连接。

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
   "tabs":["D:\\Windhawk"],"itemCount":15,"viewMode":3}

// 调用命令
→ {"token":"...","method":"tools/call","name":"ViewMode","args":{"preset":5}}
← {"ok":true,"command":"ViewMode","risk":"write"}

// 危险命令
→ {"token":"...","method":"tools/call","name":"TabCloseOther","approve":true}
← {"ok":false,"command":"TabCloseOther","risk":"danger","error":"danger_disabled"}
```

### 错误码

| 错误 | 含义 |
|---|---|
| `invalid_json` | 请求不是合法 JSON 对象 |
| `bad_token` | 令牌错/缺失/通道未启用（**不泄露工具清单**） |
| `unknown_method` | method 不是 `tools/list` / `state` / `tools/call` |
| `missing_name` | `tools/call` 缺 `name` |
| `未知或未暴露给 AI 的命令` | 该命令不在白名单内 |
| `danger_disabled` | 设置里 `aiAllowDanger=false`，危险命令一律拒绝 |
| `need_approval` | 危险命令缺 `approve=true` |
| `该命令不接受参数` | 无参数命令被传了 args（显式拒绝，不静默忽略） |
| `缺少必填参数：xxx` | argsSchema 的 required 项没给 |
| `timeout` | UI 线程 10 秒内没处理完（通常是在模态对话框里） |

---

## 4. 安全边界

| 层 | 机制 | 防的是什么 |
|---|---|---|
| 通道 | 默认关闭，需显式改配置 | 安装即对外开放 |
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
4. 跑 `python tools/check_ai_meta.py` —— 它会检查暴露/参数/选中项是否自洽。
5. 跑 `tests/ai_api_test.cpp`。

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

# 协议层：非法输入 / 令牌 fail-closed / 不泄露能力面（8 项）
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
