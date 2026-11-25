// tools-exbar.js - 用 ExBarBrowser 的 AI 命名管道作为 agent 的工具后端。
//
// 【为什么不是 --mcp-stdio】（这是踩过的坑，别改回去）
//   `--mcp-stdio` 会强制 `forceNew = true`（src/main.cpp），即**自己开一个独立实例**。
//   那个实例有自己的标签页和视图，≠ 用户眼前那个窗口 —— 表现就是
//   "AI 能读到数据，但操作的不是你打开的窗口"，甚至改了半天什么都没变。
//   命名管道没有这个问题：它连的是**已经在运行的那个窗口**。
//
// 【发现机制】本工程本来就为外部客户端准备了发现文件：
//     <dataDir>/ai-pipe.<pid>.txt   → \\.\pipe\ExBarBrowser_ai_<hash>_<pid>
//     <dataDir>/ai-token.<pid>.txt  → 32 位令牌（每次请求都要带）
//
// 【管道只有一个连接槽】服务端 CreateNamedPipe 的 nMaxInstances = 1。
//   所以**同一时刻只能有一个客户端**。若有别的客户端（或残留的 --mcp-stdio
//   子进程）占着，连接会一直卡在 ConnectNamedPipe 上直到超时。
//   这就是"管道文件在、名字对、却连不上"的最常见原因 —— 先关掉占用者。
//
// 【接口契约】导出与 tools.js 相同的名字，两条循环一行都不用改：
//   TOOL_SCHEMAS（数组，握手后原地填充）、dispatch(name, args, signal)
"use strict";

const { ConnectAny, ListEndpoints } = require("./pipe-exbar");
// 安全边界的唯一实现（denyCommands / allowedRoots），见 guard.js 文件头。
// 这里 require 它的 config 是为了在调用点读**当前**配置 —— config.js 每次
// 请求都会按文件 mtime 重载，缓存一份就会让"改了白名单不生效"。
const guard = require("./guard");
const { config } = require("./config");

// 导出引用必须全程不变（agent-stream 在模块加载时就拿走了它）
const TOOL_SCHEMAS = [];

let client = null;
let started = false;
let startPromise = null;
let lastReadyReason = "";

// 首轮工具回退：程序重启时，第一次连桥可能刚好落在旧管道退场、新管道还没建好
// 的窗口里。这里只补一次“等一下再连”，不重试整轮模型调用，也不绕过任何安全门。
const FALLBACK_DELAY_MS = 900;

// 被边界拦下的调用要留一行日志。理由与宿主的审计同一条：
// 拒绝本身是**结果**，不留痕的话"AI 为什么突然不干活了"无从查起 ——
// 而这类问题最常见的解释就是"撞白名单了"，却没有任何证据。
function LogGuardBlocked(name, reason) {
  try {
    console.error(`[guard] 已拦截 ${name}：${reason}`);
  } catch (e) { }
}

// 连接（或重连）。ListEndpoints 已把"规范文件 ai-pipe.txt"排在前面，
// 后备是每个 ai-pipe.<pid>.txt —— 进程频繁重启时这条链路能自动跟上。
async function connectAny() {
  const r = await ConnectAny(5000);
  if (!r.ok) return { ok: false, reason: r.reason };
  return { ok: true, client: r.client, tried: r.tried, total: r.total };
}

function EnsureReady() {
  if (started && client && client.ready) return Promise.resolve(true);
  if (startPromise) return startPromise;
  startPromise = (async () => {
    // 旧连接已经不能用时，旧的工具清单也作废。否则连桥失败后模型仍会收到
    // 上一次会话的工具，看起来“有工具”，真正执行时却连不上。
    TOOL_SCHEMAS.length = 0;
    const r = await connectAny();
    if (!r.ok) {
      lastReadyReason = r.reason || "ExBarBrowser bridge unavailable";
      console.error("[tools-exbar] " + r.reason);
      startPromise = null;   // 允许下次重试
      return false;
    }
    client = r.client;
    const schemas = client.ToolSchemas();
    TOOL_SCHEMAS.length = 0;
    for (const s of schemas) TOOL_SCHEMAS.push(s);
    started = true;
    lastReadyReason = TOOL_SCHEMAS.length ? "" : "tools/list 返回空工具列表";
    const pid = client.inst ? client.inst.pid : 0;
    console.log(`[tools-exbar] 已连上 ExBarBrowser pid=${pid}：${TOOL_SCHEMAS.length} 个工具` +
      (r.tried ? `（跳过 ${r.tried} 个不可用候选）` : ""));
    return true;
  })();
  return startPromise;
}

function ResetConnection() {
  if (client) {
    try { client.Close(); } catch (e) { }
  }
  client = null;
  started = false;
  startPromise = null;
  TOOL_SCHEMAS.length = 0;
}

function WaitBeforeFallback(ms, signal) {
  if (!ms || (signal && signal.aborted)) return Promise.resolve();
  return new Promise((resolve) => {
    let timer = null;
    const finish = () => {
      if (timer) clearTimeout(timer);
      if (signal) signal.removeEventListener("abort", onAbort);
      resolve();
    };
    const onAbort = () => finish();
    if (signal) signal.addEventListener("abort", onAbort, { once: true });
    timer = setTimeout(finish, ms);
  });
}

// 回合开始前的一次性工具回退。返回结构化结果给 UI/系统提示词，
// 而不是只返回 bool —— 用户需要知道“工具桥恢复了”还是“确实没有工具”。
// 注意这里只重探连接，不会自动重跑被安全边界拦下的命令。
async function EnsureReadyWithFallback(signal, waitMs = FALLBACK_DELAY_MS) {
  const firstOk = await EnsureReady();
  const needRetry = !firstOk || TOOL_SCHEMAS.length === 0;
  if (!needRetry) return { ok: true, tools: TOOL_SCHEMAS.length, retried: false, reason: "" };

  const firstReason = lastReadyReason
    || (!firstOk ? "ExBarBrowser bridge unavailable" : "tools/list 返回空工具列表");
  if (signal && signal.aborted) {
    return { ok: false, tools: TOOL_SCHEMAS.length, retried: false, reason: "cancelled", cancelled: true };
  }

  await WaitBeforeFallback(waitMs, signal);
  if (signal && signal.aborted) {
    return { ok: false, tools: TOOL_SCHEMAS.length, retried: true, reason: "cancelled", cancelled: true };
  }

  ResetConnection();
  const secondOk = await EnsureReady();
  const ok = !!secondOk && TOOL_SCHEMAS.length > 0;
  return {
    ok,
    tools: TOOL_SCHEMAS.length,
    retried: true,
    reason: ok ? "" : (lastReadyReason || firstReason),
    cancelled: false
  };
}

// 视角提示：让模型知道"命令作用在用户眼前那个窗口上"，以及哪些命令需要视图就绪。
function CapabilityNote() {
  // 桥没连上、或 tools/list 返回空表时，别给模型发一段“你有 N 个工具”
  // 的空提示；让它直接按 no tools this turn 的口径回答。
  if (!started || !TOOL_SCHEMAS.length) return "";
  return `\n\nTOOL NOTES (ExBarBrowser, connected to the user's live window)\n`
    + `- You have ${TOOL_SCHEMAS.length} tools with dotted names (nav.back, fs.open, view.sort, tab.new...).\n`
    + `- These operate on the SAME window the user is looking at. What you change, they see immediately.\n`
    + `- Commands that inspect the file view (tool.list, tool.stat, view.*) need an initialized shell view.\n`
    + `  If one fails, the window may still be loading: wait a moment and retry.\n`
    + `- Destructive commands always need the user's confirmation.\n`;
}

// 单次工具调用的超时。优先用宿主 tools/list 暴露的 timeoutMs，未知时保持
// 原有 90s；钳制上下界，避免元数据写错成 0 或一天。
function ToolTimeout(name) {
  const t = ToolMeta(name);
  const raw = Number(t && (t.timeoutMs !== undefined ? t.timeoutMs : (t.annotations || {}).timeoutMs));
  if (!Number.isFinite(raw)) return 90000;
  return Math.max(1000, Math.min(Math.floor(raw), 600000));
}

// dispatch(name, args, signal, approve)
//   approve=true 表示"面板里已经弹过审批卡且用户点了确认"，此时宿主侧的
//   危险双门才会放行。其余情况一律 false —— 权限不能靠默认值蒙过去。
async function dispatch(name, args, signal, approve) {
  if (signal && signal.aborted) return { error: "cancelled" };

  // 【安全边界在唯一的出口上】dispatch 是两条 agent 循环调用工具的**唯一**
  // 通路，所以 allowedRoots / denyCommands 只在这里判一次就够了（见 guard.js
  // 的文件头：这两个配置项此前没有任何活的执行点）。
  // 放在连桥**之前**：越界的请求根本不该发出去 —— 发出去再拒绝，等于让宿主
  // 先看到一次越权调用（它在审计里会留痕，看起来像"试过"）。
  const blocked = guard.Check(name, args, config);
  if (blocked) {
    LogGuardBlocked(name, blocked);
    return { error: blocked, errorCode: "blocked_by_policy" };
  }

  // 【并行开发的关键】程序会被频繁重启，所以**每次调用前**都确认连接还活着。
  // 死的连接重试一次就够 —— 不值得为它加复杂的状态机，但绝不能沿用旧结果。
  if (started && client && !client.ready) {
    started = false;
    startPromise = null;
  }

  let ok = await EnsureReady();
  if (!ok) {
    // 第一次失败可能正赶上程序在重启：给一次机会（等管道被新实例建好）
    await new Promise((r) => setTimeout(r, 1500));
    started = false;
    startPromise = null;
    ok = await EnsureReady();
  }
  if (!ok) return { error: "连不上 ExBarBrowser（程序可能在重启，或没有打开窗口）" };
  if (signal && signal.aborted) return { error: "已取消", errorCode: "cancelled" };

  const timeoutMs = ToolTimeout(name);
  let r = await client.Call(name, args || {}, timeoutMs, approve === true, signal);

  // 传输层失败 = 连接断了（程序重启/窗口关闭）。重连一次再试，
  // 让"边开发边用"不至于每次重启都要手动刷新页面。
  // 取消与超时不是传输故障：重试只会把用户已经停下的动作再跑一遍。
  if (!r.ok && (r.errorCode === "transport" || !client.ready) && !(signal && signal.aborted)) {
    client.Close();
    started = false;
    startPromise = null;
    await new Promise((res) => setTimeout(res, 1200));
    const re = await EnsureReady();
    if (re) r = await client.Call(name, args || {}, timeoutMs, approve === true, signal);
  }

  if (r.errorCode === "timeout" || r.errorCode === "cancelled") {
    return { error: r.text || r.errorCode, errorCode: r.errorCode };
  }

  if (r.ok) {
    const payload = r.structured && Object.keys(r.structured).length
      ? r.structured
      : (r.text || "");
    const out = typeof payload === "string" ? { text: payload } : payload;
    // 断言透传给 UI：verdict 与 ok 不一致的那一档（handler 说成了、效果没成）
    // 必须在面板上可见，否则用户看到的是一个绿色的"完成"。
    if (r.assertions && r.assertions.length) out.__assertions = r.assertions;
    return out;
  }
  if (!client.ready) { started = false; startPromise = null; }
  const failed = { error: r.text || `tool failed (${r.errorCode || "unknown"})`, errorCode: r.errorCode };
  if (r.assertions && r.assertions.length) failed.__assertions = r.assertions;
  return failed;
}

function Stop() {
  if (client) client.Close();
  client = null;
  started = false;
  startPromise = null;
}

// ---------------------------------------------------------------------------
// 审批判定的唯一真源：命令表自己的 annotations
// ---------------------------------------------------------------------------

// 【为什么不能用手写清单】
//   本文件原先在 prompt.js 里维护一份 MUTATING 名字集合，那是从 Directory Opus
//   时代抄过来的。实测（2026-11-23，连上活实例读 tools/list）两边已经双向漂移：
//     漏问 12 条：tab.close.others/left/right/prev/next、bmk.remove、dev.build、
//                 dev.test、search.history.clear、nav.history.clear、
//                 tab.close.duplicates、ext.terminal.admin
//     多问  9 条：fs.copy、fs.rename、file.undo、file.new.folder、file.new.text、
//                 file.paste、bmk.sort、session.named.delete、dev.archive.decide
//   漏问是安全问题（真破坏性操作不弹审批），多问是体验问题（普通写入也要点两下）。
//   命令表已经用 `annotations.destructiveHint` 明确回答了这件事，再手抄一份就等于
//   在自己之外又养一个会过期的真相 —— 而两个真相不一致时，永远是宽松的那个生效。
//
// 【未知工具 fail closed】查不到元数据时按"需要确认"处理。宁可多问一次，
//   也不能因为"工具刚加、清单没跟上"就把一个破坏性操作放过去。
function ToolMeta(name) {
  if (!client || !Array.isArray(client.tools)) return null;
  return client.tools.find((t) => t && t.name === name) || null;
}

// 返回 { needed, known, readOnly, undoable, undoGroup, undoDomain, risk, title, description }。
// 命令表标的 destructiveHint。与宿主侧危险双门（aiAllowDanger + approve）
// 是同一个事实的两个消费者，不是两套规则。readOnly 供 readonly 模式与
// confirm 模式的"普通写也要问"使用。
function ApprovalOf(name) {
  const t = ToolMeta(name);
  if (!t) {
    return { needed: true, known: false, readOnly: false, undoable: false,
             undoGroup: "none", undoDomain: "", risk: "", title: "", description: "" };
  }
  const a = t.annotations || {};
  return {
    needed: a.destructiveHint === true,
    known: true,
    readOnly: a.readOnlyHint === true,
    undoable: a.undoable === true,
    undoGroup: String(a.undoGroup || "none"),
    undoDomain: String(a.undoDomain || ""),
    risk: String(t.risk || ""),
    title: String(t.title || ""),
    description: String(t.description || "")
  };
}

// 工具的显示名（中文标题，来自命令表）。取不到就回落工具名本身 ——
// 不编造一个看起来像人话的假标题。
function ToolTitle(name) {
  const t = ToolMeta(name);
  return t && t.title ? String(t.title) : "";
}

// 给 /api/health 用：报告连的是哪个 pid
function Status() {
  return {
    connected: started && !!client && client.ready,
    pid: client && client.inst ? client.inst.pid : 0,
    tools: TOOL_SCHEMAS.length,
    server: client && client.serverInfo ? client.serverInfo.name : ""
  };
}

// 底层客户端（供 exbar-context 发厂商扩展方法如 exbar/state）。
// 返回 null = 尚未连接；调用方应先 EnsureReady()。
function RawClient() {
  return started && client && client.ready ? client : null;
}

// 把命令表查询能力交给 guard.js（它要用 readOnlyHint 判 dryRun 该不该拦）。
// 【为什么是注入而不是 guard require 本文件】本文件已经 require guard 来在
// dispatch 里做校验，反向依赖就成环。注入同时让 guard_test.js 不连桥也能跑。
guard.SetMetaProvider(ToolMeta);

module.exports = {
  TOOL_SCHEMAS, dispatch, EnsureReady, EnsureReadyWithFallback, CapabilityNote, Stop, Status, RawClient,
  ApprovalOf, ToolTitle, ToolMeta
};