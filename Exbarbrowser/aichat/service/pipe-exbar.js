// pipe-exbar.js - 连到运行中的 ExBarBrowser 窗口的 AI 命名管道。
//
// 【两条硬事实，都是实测得来的】
//
// 1. 不能用 --mcp-stdio。
//    `src/main.cpp` 里 --mcp-stdio 强制 forceNew = true（等价 --new-window），
//    它**自己开一个独立实例**再当服务器。那个实例有自己的标签页/视图，
//    ≠ 用户眼前那个窗口 —— 表现成"AI 能取数据，但操作的不是你打开的窗口"。
//
// 2. Node 直连 Windows 命名管道**不通**，必须经 PowerShell 桥。
//    实测：Node 的 `net.createConnection("\\\\.\\pipe\\X")` 一律 ENOENT，
//    而 PowerShell 的 `System.IO.Pipes.NamedPipeClientStream` 连同一个管道成功。
//    差异在 libuv 对 Windows 管道名的处理。所以本模块拉起
//    `exbar-bridge.ps1`（常驻），协议与直连完全一致，只换传输。
//
// 【本项目并行开发、频繁重启 —— 这是核心约束】
//    管道名内嵌 hwnd 与 pid（`\\.\pipe\ExBarBrowser_ai_<hwnd>_<pid>`），
//    进程一重启名字就变，**无法固定**（改管道名要动 C++ 侧）。
//    所以"固定"落在**发现文件**这一层：
//      <dataDir>/ai-pipe.txt  / ai-token.txt        → 最后启动的实例（规范文件）
//      <dataDir>/ai-pipe.<pid>.txt / ai-token.<pid>.txt → 每实例副本
//    客户端每次**重连都重读这两个文件**，自然跟上最新实例。
//    这就是本工程的"固定端口"等价物。
"use strict";

const fs = require("fs");
const path = require("path");
const { spawn } = require("child_process");
const readline = require("readline");

const DATA_DIR = process.env.EXBAR_DATA_DIR || "E:\\ExBarBrowser\\data";
const BRIDGE_PS1 = path.join(__dirname, "exbar-bridge.ps1");

function readText(p) {
  try { return fs.readFileSync(p, "utf8").trim(); } catch (e) { return ""; }
}

// 【为什么必须扫所有 profile 目录】
//   本工程支持 `EXBARBROWSER_PROFILE` 隔离（并行开发的测试会话、--multi、
//   回归脚本都会用自己的 profile）。每个 profile 目录都有**自己的一套**
//   ai-pipe.txt / ai-pipe.<pid>.txt，互相不可见。
//   实测：只读 data/ai-pipe.txt 会指向一个早已退出的实例，而真正活着的是
//   data/profile-multi/ 下的那个 —— 表现就是"文件在、名字对、连不上"。
//   所以这里递归扫 data/ 下所有目录（一层 + 两层，够用且不慢）。
function AllPipeFiles() {
  const out = [];
  const push = (p) => { try { if (fs.statSync(p).isFile()) out.push(p); } catch (e) { } };

  push(path.join(DATA_DIR, "ai-pipe.txt"));
  let dirs = [];
  try { dirs = fs.readdirSync(DATA_DIR, { withFileTypes: true }); } catch (e) { return out; }
  for (const d of dirs) {
    const full = path.join(DATA_DIR, d.name);
    if (d.isDirectory()) {
      push(path.join(full, "ai-pipe.txt"));
      let inner = [];
      try { inner = fs.readdirSync(full, { withFileTypes: true }); } catch (e) { continue; }
      for (const d2 of inner) {
        if (d2.name.startsWith("ai-pipe")) push(path.join(full, d2.name));
      }
    } else if (d.name.startsWith("ai-pipe")) {
      push(full);
    }
  }
  return out;
}

// 读一个 ai-pipe 文件对应的 {pipe, token, pid, tokenPath}
function EndpointFromPipeFile(pipeFile) {
  const pipe = readText(pipeFile);
  if (!pipe) return null;
  const dir = path.dirname(pipeFile);
  const base = path.basename(pipeFile);
  // ai-pipe.txt → ai-token.txt；ai-pipe.<pid>.txt → ai-token.<pid>.txt
  const m = base.match(/^ai-pipe(\.\d+)?\.txt$/);
  const tokenName = m ? `ai-token${m[1] || ""}.txt` : "ai-token.txt";
  let token = readText(path.join(dir, tokenName));
  if (!token) token = readText(path.join(dir, "ai-token.txt"));
  if (!token) return null;
  // pid：优先从文件名取，其次从管道名尾部取（..._<hwnd>_<pid>）
  let pid = 0;
  if (m && m[1]) pid = Number(m[1].slice(1));
  if (!pid) {
    const pm = pipe.match(/_(\d+)$/);
    if (pm) pid = Number(pm[1]);
  }
  let mtime = 0;
  try { mtime = fs.statSync(pipeFile).mtimeMs; } catch (e) { }
  return { pipe, token, pid, pipeFile, profile: path.basename(dir), mtime };
}

// 所有候选端点。**不按"进程是否存活"过滤** —— 那个判断本身不可靠
// （见 pidAliveNow 的注释），让它去试连接比先斩后奏可靠。
// 但存活状态**用于排序**：活的排前面，否则 155 个候选里前几个全是残留，
// 每次连接都要白等好几轮。
function pidAliveNow(pid) {
  if (!pid) return false;
  try {
    // 用 process.kill(pid, 0)：Windows 上 Node 支持，纯本进程判断、
    // 不依赖 tasklist/PowerShell 的会话视图差异。
    process.kill(pid, 0);
    return true;
  } catch (e) {
    // EPERM = 进程存在但无权限（也算活）；ESRCH = 不存在
    return e.code === "EPERM";
  }
}

// 【本模块的两条连法，按优先级】
//
// 1. **环境变量直读**（嵌入主进程后的正式路径）
//    主进程启动 Node 服务时，把自己当前实例的管道名与令牌通过环境变量传下来
//    （EXBAR_PIPE / EXBAR_TOKEN）。此时**不枚举任何文件、不猜 pid** ——
//    管道名由父进程直接给，不可能错。
//
// 2. **发现文件枚举**（独立启动服务的旧路径）
//    没有环境变量时，扫 data 下所有 ai-pipe 文件逐个试。
//    这条路径的复杂度来自"多实例 + profile 隔离 + 并行开发"，
//    每次重启都要重新猜，容易连到死实例。**仅为兼容保留**。
function EndpointFromEnv() {
  const pipe = process.env.EXBAR_PIPE;
  const token = process.env.EXBAR_TOKEN;
  if (!pipe || !token) return null;
  const m = pipe.match(/_(\d+)$/);
  return {
    pipe, token,
    pid: m ? Number(m[1]) : 0,
    pipeFile: "(env)",
    profile: "env",
    mtime: 0,
    fromEnv: true
  };
}

function ListEndpoints() {
  // 环境变量在时直接用它，跳过整个枚举
  const envEp = EndpointFromEnv();
  if (envEp) return [envEp];

  const seen = new Set();
  const out = [];
  for (const f of AllPipeFiles()) {
    const ep = EndpointFromPipeFile(f);
    if (!ep || seen.has(ep.pipe)) continue;
    seen.add(ep.pipe);
    ep.alive = pidAliveNow(ep.pid);
    out.push(ep);
  }
  // 排序：**活进程优先**（这是最省时间的判据），然后常驻 profile 优先，
  // 最后按文件新旧。死实例仍留在列表里 —— 万一进程探测失败，还有机会连上。
  const rank = (ep) => {
    if (ep.profile === "data") return 0;
    if (/^profile-multi/.test(ep.profile)) return 1;
    if (/^profile-(manual|verify|final)/.test(ep.profile)) return 2;
    return 3;
  };
  out.sort((a, b) => {
    if (a.alive !== b.alive) return a.alive ? -1 : 1;
    const ra = rank(a), rb = rank(b);
    if (ra !== rb) return ra - rb;
    return (b.mtime || 0) - (a.mtime || 0);
  });
  return out;
}

class PipeClient {
  constructor(inst) {
    this.inst = inst || null;
    this.child = null;
    this.nextId = 1;
    this.pending = new Map();
    this.ready = false;
    this.lastError = "";
    this.serverInfo = null;
    this.tools = [];
  }

  // 拉起 PowerShell 桥并握手。失败返回 false（原因在 lastError）。
  async Connect(timeoutMs) {
    if (this.ready) return true;

    // 若没指定端点，现场读一次（支持"程序刚重启"）
    if (!this.inst) {
      const all = ListEndpoints();
      if (!all.length) {
        this.lastError = `在 ${DATA_DIR} 下找不到任何 ai-pipe 文件（ExBarBrowser 在运行吗？）`;
        return false;
      }
      this.inst = all[0];
    }
    if (!fs.existsSync(BRIDGE_PS1)) {
      this.lastError = `找不到桥脚本 ${BRIDGE_PS1}`;
      return false;
    }

    const env = {
      ...process.env,
      EXBAR_DATA_DIR: DATA_DIR,
      EXBAR_PIPE: this.inst.pipe,
      EXBAR_TOKEN: this.inst.token
    };

    this.child = spawn("powershell", [
      "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
      "-File", BRIDGE_PS1
    ], { env, windowsHide: true, stdio: ["pipe", "pipe", "pipe"] });

    this.child.on("error", (e) => {
      this.lastError = `启动桥失败：${e.message}`;
      this.ready = false;
      this._failAll(this.lastError);
    });
    this.child.on("exit", (code) => {
      this.ready = false;
      if (code !== 0) this._failAll(`桥退出（code=${code}）`);
    });
    if (this.child.stderr) {
      readline.createInterface({ input: this.child.stderr }).on("line", (l) => {
        if (l.trim()) this.lastError = l.trim();
      });
    }

    const rl = readline.createInterface({ input: this.child.stdout });
    rl.on("line", (line) => this._onLine(line));

    // 等桥的 ready 通知
    const readyOk = await new Promise((resolve) => {
      const t = setTimeout(() => resolve(false), timeoutMs || 12000);
      const check = (line) => {
        let m;
        try { m = JSON.parse(line); } catch (e) { return; }
        if (m && m.ready) { clearTimeout(t); resolve(true); }
        else if (m && m.ok === false && m.error) {
          this.lastError = m.error;
          clearTimeout(t); resolve(false);
        }
      };
      const origOnLine = this._onLine.bind(this);
      this._onLine = (line) => { check(line); origOnLine(line); };
    });
    if (!readyOk) {
      if (!this.lastError) this.lastError = "桥没有就绪（超时）";
      this.Close();
      return false;
    }

    try {
      const init = await this._request("initialize", {
        protocolVersion: "2024-11-05",
        capabilities: {},
        clientInfo: { name: "exbarbrowser-aichat", version: "1.0" }
      }, 15000);
      this.serverInfo = (init && init.serverInfo) || null;
      this._notify("notifications/initialized", {});
      const res = await this._request("tools/list", {}, 15000);
      this.tools = (res && Array.isArray(res.tools)) ? res.tools : [];
      this.ready = true;
      return true;
    } catch (e) {
      this.lastError = `握手失败：${e.message}`;
      this.Close();
      return false;
    }
  }

  ToolSchemas() {
    return this.tools.map((t) => ({
      type: "function",
      function: {
        name: t.name,
        description: t.description || "",
        parameters: t.inputSchema || { type: "object", properties: {} }
      }
    }));
  }

  // 发一条任意 JSON-RPC 方法（不限于 tools/call）。
  // MCP 标准方法之外，本工程还有 `exbar/state` 这类厂商扩展方法 ——
  // 它们比工具调用便宜（不扫目录），是"当前路径 / 选中集"的权威来源。
  Request(method, params, timeoutMs) {
    return this._request(method, params || {}, timeoutMs || 20000);
  }

  // approve：宿主侧对 destructive 命令要求 approve=true（危险双门的第二道）。
  // 【为什么必须显式传】宿主侧 ai_api.cpp 的判定是"设置允许（aiAllowDanger）
  // + 调用方声明 approve"。面板里的审批卡走完如果**不**把 approve 带下去，
  // 用户点了"确认执行"，命令仍会被宿主以 need_approval 拒绝 ——
  // 表现成"审批点了没用"，而两边都以为自己是对的。
  // 只有面板里真的弹过卡且用户点了确认，调用方才会传 true；别处一律 false。
  async Call(name, args, timeoutMs, approve, signal) {
    if (signal && signal.aborted) {
      return { ok: false, text: "已取消", errorCode: "cancelled" };
    }
    if (!this.ready) {
      const ok = await this.Connect();
      if (!ok) return { ok: false, text: this.lastError, errorCode: "not_connected" };
    }
    let res;
    const params = { name, arguments: args || {} };
    if (approve === true) params.approve = true;
    let requestId = null;
    const onAbort = () => {
      if (requestId !== null) this.Cancel(requestId, "用户停止了本回合", "cancelled");
    };
    if (signal) signal.addEventListener("abort", onAbort, { once: true });
    try {
      const pending = this._request("tools/call", params, timeoutMs || 60000);
      requestId = pending.requestId;
      res = await pending;
    } catch (e) {
      const code = e && e.code;
      if (code === "cancelled" || code === "timeout" || (signal && signal.aborted)) {
        return {
          ok: false,
          text: e && e.message ? e.message : "已取消",
          errorCode: code || "cancelled"
        };
      }
      return { ok: false, text: String(e.message || e), errorCode: "transport" };
    } finally {
      if (signal) signal.removeEventListener("abort", onAbort);
    }
    const text = (res && Array.isArray(res.content))
      ? res.content.filter((c) => c && c.type === "text").map((c) => c.text).join("\n")
      : "";
    const isError = !!(res && res.isError);
    return {
      ok: !isError,
      text,
      structured: res && res.structuredContent ? res.structuredContent : null,
      // 【断言必须往上传】宿主的自断言说的是"效果到底对不对"，与 ok（handler
      // 自称成功）是两件事。以前这里只挑 structuredContent / 文本，断言被整个丢掉
      // —— 于是"handler 说成了、实际没成"这种最危险的情况在面板上显示成绿色完成。
      // assertions 的形状：[{expr,expect,actual,ok}]，原样给 UI 用。
      assertions: res && Array.isArray(res.assertions) ? res.assertions : null,
      errorCode: res && res.errorCode ? res.errorCode : (isError ? "tool_error" : "")
    };
  }

  Close() {
    if (this.child) {
      try { this.child.kill(); } catch (e) { }
      this.child = null;
    }
    this.ready = false;
    this._failAll("closed");
  }

  _onLine(line) {
    const s = String(line || "").trim();
    if (!s) return;
    let msg;
    try { msg = JSON.parse(s); } catch (e) { return; }
    if (msg.id === undefined || msg.id === null) return;   // 通知 / ready
    const slot = this.pending.get(msg.id);
    if (!slot) return;
    this.pending.delete(msg.id);
    clearTimeout(slot.timer);
    if (msg.error) slot.reject(new Error(msg.error.message || JSON.stringify(msg.error)));
    else slot.resolve(msg.result);
  }

  _send(obj) {
    if (!this.child || !this.child.stdin || this.child.stdin.destroyed) {
      throw new Error("桥未连接");
    }
    this.child.stdin.write(JSON.stringify(obj) + "\n");
  }

  _notify(method, params) {
    try { this._send({ jsonrpc: "2.0", method, params }); } catch (e) { }
  }

  // 本地取消一个尚未返回 JSON-RPC 响应：既给宿主发标准通知，也立刻让调用方
  // 的 Promise 失败。宿主目前没有可抢占的 job 句柄，所以 errorCode 只表示
  // "我们不再等了"，不代表执行已经被物理中断 —— 卡片文案如实区分。
  Cancel(requestId, reason, code) {
    const slot = this.pending.get(requestId);
    if (!slot) return false;
    this.pending.delete(requestId);
    clearTimeout(slot.timer);
    this._notify("notifications/cancelled", {
      requestId,
      reason: reason || "cancelled",
      method: slot.method || "",
      name: slot.params && slot.params.name ? slot.params.name : ""
    });
    const err = new Error(reason || "cancelled");
    err.name = "RequestCancelled";
    err.code = code || "cancelled";
    slot.reject(err);
    return true;
  }

  _request(method, params, timeoutMs) {
    const id = this.nextId++;
    const promise = new Promise((resolve, reject) => {
      const limit = timeoutMs || 20000;
      const timer = setTimeout(() => {
        this.Cancel(id, `${method} timeout（${limit}ms）`, "timeout");
      }, limit);
      this.pending.set(id, { resolve, reject, timer, method, params });
      try {
        this._send({ jsonrpc: "2.0", id, method, params });
      } catch (e) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(e);
      }
    });
    // 让 Call() 能在 abort/timeout 时准确取消这一条，而不是靠猜最后一项。
    promise.requestId = id;
    return promise;
  }

  _failAll(reason) {
    for (const [, slot] of this.pending) {
      clearTimeout(slot.timer);
      slot.reject(new Error(reason));
    }
    this.pending.clear();
  }
}

// 依次尝试端点，返回第一个握手成功的客户端。
async function ConnectAny(timeoutMs) {
  const list = ListEndpoints();
  if (!list.length) {
    return { ok: false, reason: `没有找到 ${path.join(DATA_DIR, "ai-pipe.txt")}（ExBarBrowser 未运行？）` };
  }
  const why = [];
  // 每个候选只给较短时间：候选可能有几十个（每个 profile 都能留下一堆），
  // 逐个等 12 秒会让"程序没开"这种情况变成几分钟的卡顿。
  // 真正能连上的那个会很快返回（桥启动 + 两次握手约 1-2 秒）。
  const perTry = timeoutMs || 6000;
  for (const ep of list) {
    const cli = new PipeClient(ep);
    const ok = await cli.Connect(perTry);
    if (ok) return { ok: true, client: cli, tried: why.length };
    why.push(`${ep.profile}/pid=${ep.pid || "?"}：${cli.lastError}`);
    cli.Close();
  }
  return { ok: false, reason: "所有端点都连不上：\n  " + why.join("\n  ") };
}

module.exports = { PipeClient, ListEndpoints, ConnectAny, DATA_DIR };