# aichat/ — AI 面板的服务与 Web UI

这个目录是 AI 面板的**运行时内容**，由 `ExBarBrowser.exe` 在启动时拉起。
它不是独立的程序：没有主进程，它不知道自己该连哪个窗口。

## 结构

```
aichat/
├── service/
│   ├── server.js           HTTP + SSE 服务，端口由主进程经 AGENTCHAT_PORT 指定
│   ├── agent-stream.js     agent 循环（多步工具编排）
│   ├── tools-exbar.js      把 ExBarBrowser 的命令暴露成 LLM 的 tools
│   ├── pipe-exbar.js       连接到本实例的 AI 命名管道
│   ├── exbar-bridge.ps1    Node ⇄ Windows 命名管道之间的桥（纯 ASCII）
│   ├── exbar-context.js    取"用户现在在看什么"（路径 / 条目数 / 选中集）
│   ├── config.js           配置（data/ 下的 json + 环境变量覆盖）
│   └── prompt.js           系统提示词
├── ui/                     Web UI（原生 JS，无构建步骤）
└── logs/                   运行日志
```

`config.json` 与 `model.json` 是**用户私有文件**（后者含明文 API key），仓库和发布包都不带。
发布包里放的是 `config.example.json` / `model.example.json`：首次用 AI 面板时复制成正式文件，
在 `model.json` 里填自己的 `baseUrl` / `apiKey` / 模型 id。填好密钥的文件不要外发。

## 启动链路

```
ExBarBrowser.exe
  ├─ ai::StartServer()           建命名管道，管道名含 hwnd+pid（每次启动都变）
  ├─ CreateProcess(node server.js)
  │    环境变量：EXBAR_PIPE / EXBAR_TOKEN / AGENTCHAT_PORT / EXBAR_PARENT_PID
  ├─ WebView2（面板右侧）→ http://127.0.0.1:<port>/
  └─ 退出时 Job Object 连带结束 node
```

**为什么要由主进程拉起**：服务原本要自己"猜"该连哪个实例 —— 管道名内嵌
`hwnd + pid`，进程每次重启都变；`data/` 下还有几十个 profile 目录各有一套发现文件
（实测出现过 180 个候选，绝大多数是死实例）。现在主进程直接把管道名与令牌
通过环境变量传下去，服务不需要任何发现逻辑。

## 两个必须知道的坑

### 1. `.ps1` 脚本必须纯 ASCII

Windows PowerShell 5.1 按 **ANSI 代码页**（本机是 GBK）读 `.ps1`，UTF-8 的中文注释
会变成乱码并引发解析错误（`TerminatorExpectedAtEndOfString`）。
所以 `exbar-bridge.ps1` 的注释一律用英文。改这个文件时不要加中文。

### 2. Node 不能直连 Windows 命名管道

`net.createConnection("\\\\.\\pipe\\X")` 在 Node 侧一律 `ENOENT` —— libuv 把管道名
当普通文件路径解析。而 .NET 的 `NamedPipeClientStream` 连同一个管道是成功的。
所以必须经 `exbar-bridge.ps1` 这一层桥。

## 手动运行（调试用）

不启动主进程、单独调服务时，需要自己给管道信息：

```bash
# 先找到运行中实例的管道与令牌
cat data/ai-pipe.txt data/ai-token.txt

# 再带上它们启动（端口自选）
EXBAR_PIPE='\\.\pipe\ExBarBrowser_ai_xxxx_1234' \
EXBAR_TOKEN='...' \
AGENTCHAT_PORT=8792 \
node aichat/service/server.js
```

没有 `EXBAR_PIPE` / `EXBAR_TOKEN` 时会退回到"扫 data 下所有 ai-pipe 文件"的旧发现逻辑
（可用，但慢且容易连到死实例 —— 见 `pipe-exbar.js` 顶部注释）。

## 相关决策

- `docs/DECISIONS.md` **D12**：面板占同一客户区最右一列、squeeze、快捷键启停
- `docs/DECISIONS.md` **D13**：模型调用留在原生侧（密钥不进渲染进程）
- `docs/DECISIONS.md` **D14**：WebView2 + Node 子进程（含两个运行时依赖的显式豁免）