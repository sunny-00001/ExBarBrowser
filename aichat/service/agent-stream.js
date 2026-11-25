// agent-stream.js - the same tool-use loop as agent.js, but event-driven.
//
// WHY A SECOND LOOP
//   agent.js returns one finished string. A modern agent window needs to show
//   tokens as they arrive, one card per tool call, and a human gate before any
//   write. Those are all *timing* concerns, so they cannot be layered on top of
//   a function that only resolves at the end. This file is the streaming twin:
//   identical prompt, identical tool registry, identical step budget, but every
//   meaningful moment is emitted to the browser over SSE.
//
//   The shared helpers (SYSTEM_PROMPT, clip, summariseResult, needsApproval,
//   formatAction) live in prompt.js so the two loops cannot drift.
//
// EVENTS (all sent as `data: {"type": ...}\n\n`)
//   start      {sessionId, messageId}
//   status     {text}                      thinking / executing / wrapping up
//   delta      {text}                      assistant text chunk
//   thinking   {text}                      reasoning-model thinking chunk
//   tool       {id,name,label,args,status,result?,error?}
//   approval   {id,name,title,description,details,args}
//   done       {reply, actions, steps, usage}
//   error      {message}

const crypto = require("crypto");

const { config, modelReadiness } = require("./config");
const { TOOL_SCHEMAS, dispatch, EnsureReadyWithFallback, CapabilityNote, ApprovalOf, ToolTitle, ToolMeta } = require("./tools-exbar");
const { SYSTEM_PROMPT, clip, summariseResult, formatAction, needsApproval } = require("./prompt");
const { isMutating, shouldAskApproval } = require("./approval-policy");
const { knowledgeCard, knowledgeHits } = require("./project-knowledge");
const { matchFastIntent } = require("./intents");
const { requestSpec, parseFrame, parseResponse, buildChatBody, mergeUsage, CACHE_SPLIT } = require("./protocols");

// 一条工具调用在界面上的短标题。
//
// 【为什么从工具元数据推导，而不是再写一张 switch】exbar 的命令是点分命名的
// （fs.delete / view.sort / tab.new），共 171 条且还在增长。以前那张按 Opus
// 时代工具名（opus_run_command / fs_write）写的 switch 在迁移后**一条都命中不了**
// —— 每个卡片都退回工具名本身。现在：中文标题取命令表的 title（与命令面板、
// 工具清单同一份），参数摘要取最具体的那一个，够用户分辨"这是对哪个路径做的事"。
function toolLabel(name, args) {
  const a = args || {};
  const zh = ToolTitle(name);
  let detail = "";
  if (Array.isArray(a.targets) && a.targets.length) {
    detail = a.targets.length === 1 ? String(a.targets[0]) : `${a.targets.length} 项`;
  } else if (a.path) {
    detail = String(a.path);
  } else if (a.query) {
    detail = `「${a.query}」`;
  } else if (a.from && a.to) {
    detail = `${a.from} → ${a.to}`;
  } else if (a.pattern) {
    detail = String(a.pattern);
  }
  if (zh && detail) return `${zh} · ${detail}`;
  if (zh) return zh;
  return detail ? `${name} ${detail}` : name;
}

// 审批卡的文案。标题/风险从命令表拿（同一个事实，宿主侧危险双门也读它），
// details 给用户"到底要动什么"。
function approvalInfo(name, args) {
  const a = args || {};
  const label = toolLabel(name, a);
  const meta = ApprovalOf(name);

  // 未连上 / 命令表里没有这个工具：ApprovalOf 已 fail-closed 为"需要确认"。
  // 此时**不能**假装知道它做什么，文案要如实说明"这是未知操作"。
  if (!meta.known) {
    return {
      title: `需要确认：${name}`,
      description: "这个操作不在当前已连接的命令表里，无法判断它会改什么。确认前请先看清楚参数。",
      details: label
    };
  }

  const title = meta.title || name;
  // description 优先用命令表自己的描述（它由 effects 生成，与 annotations 同源），
  // 只说一句"它会改本机内容"等于没说清是哪一类改动。
  const description = meta.description || "这个操作会修改本机内容，确认后才会执行。";
  const lines = [];
  lines.push(`命令：${name}`);
  if (meta.risk) lines.push(`风险：${meta.risk}`);
  if (meta.undoable) lines.push("可撤销：是（执行后能用撤销找回）");
  else lines.push("可撤销：否（执行后无法用命令撤销）");
  const argText = Object.keys(a).length ? JSON.stringify(a, null, 2) : "（无参数）";
  lines.push("参数：");
  lines.push(argText);

  return { title, description, details: lines.join("\n") };
}

function uid(prefix) {
  const id = crypto.randomUUID
    ? crypto.randomUUID().replace(/-/g, "")
    : `${Date.now().toString(36)}${Math.random().toString(36).slice(2)}`;
  return `${prefix}-${id}`;
}

// 命令的风险档位，供活动流视图的徽章与筛选使用（ROADMAP P4-a）。
// 判据只有命令表自己的 annotations —— 与审批门（needsApproval）同源，
// 不在前端另写一张会过期的 switch：
//   destructiveHint → danger（不可逆/破坏性）
//   readOnlyHint    → read
//   其余            → write（会改状态但可回退）
// 查不到元数据时按 danger（fail closed），与审批门的"未知工具 fail closed"一致。
function toolRisk(name) {
  const t = ToolMeta(name);
  if (!t) return "danger";
  const a = t.annotations || {};
  if (a.destructiveHint === true) return "danger";
  if (a.readOnlyHint === true) return "read";
  return "write";
}

// 把一段流式 tool_call 合并进 slot。名字和参数都要处理"中转反复下发完整值"
// 与"正常分片"两种情况；以前名字用覆盖，遇到分片只剩最后一个字。
function mergeToolCallChunk(slot, tc) {
  if (tc.id) slot.id = tc.id;
  if (tc.name) {
    const raw = String(tc.name);
    let chunk = raw;
    let whole = false;
    try {
      const parsed = JSON.parse(raw);
      if (typeof parsed === "string") { chunk = parsed; whole = true; }
    } catch (e) { }
    if (!slot.function.name || whole || chunk === slot.function.name) {
      slot.function.name = chunk;
    } else if (chunk.startsWith(slot.function.name)) {
      slot.function.name = chunk;
    } else {
      slot.function.name += chunk;
    }
  }
  if (tc.arguments) {
    const chunk = tc.arguments;
    let whole = false;
    try {
      const v = JSON.parse(chunk);
      whole = !!v && typeof v === "object" && !Array.isArray(v);
    } catch (e) { }
    if (whole) slot.function.arguments = chunk;
    else slot.function.arguments += chunk;
  }
  return slot;
}

// ---------------------------------------------------------------------------
// streaming model call
// ---------------------------------------------------------------------------

// cfg 是回合开始时对共享 config 对象的快照。server.js 会在每个 HTTP 请求上
// reloadConfig()；不快照的话，用户中途在设置页切模型/改安全开关，回合后半段
// 会突然换到另一套参数上。
async function callModelStream(messages, signal, emit, modelOverride, cfg = config) {
  // The same body shape for both protocols; buildChatBody translates to the
  // Anthropic wire format and adds stream_options (usage) for OpenAI-compatible
  // endpoints (see service/protocols.js).
  const spec = requestSpec(cfg.protocol, cfg.baseUrl, cfg.apiKey, "chat");
  const body = buildChatBody(cfg.protocol, {
    model: modelOverride || cfg.model,
    messages,
    temperature: cfg.temperature,
    maxTokens: cfg.maxTokens,
    tools: TOOL_SCHEMAS,
    stream: true
  });

  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), cfg.timeoutMs || 180000);
  const onAbort = () => ctrl.abort();
  if (signal) {
    if (signal.aborted) ctrl.abort();
    else signal.addEventListener("abort", onAbort, { once: true });
  }

  let res;
  try {
    res = await fetch(spec.url, {
      method: "POST",
      headers: { ...spec.headers, Accept: "text/event-stream" },
      body: JSON.stringify(body),
      signal: ctrl.signal
    });
  } catch (e) {
    clearTimeout(timer);
    if (signal) signal.removeEventListener("abort", onAbort);
    const m = e && e.name === "AbortError" ? "request timed out or was stopped" : String(e && e.message ? e.message : e);
    throw new Error(`cannot reach the model endpoint ${spec.url}: ${m}`);
  }

  if (!res.ok) {
    clearTimeout(timer);
    if (signal) signal.removeEventListener("abort", onAbort);
    const text = await res.text();
    let detail = text;
    try {
      const j = JSON.parse(text);
      detail = (j.error && (j.error.message || j.error.code)) || text;
    } catch (e) { }
    throw new Error(`model endpoint returned HTTP ${res.status}: ${clip(detail, 600)}`);
  }

  // A provider that ignores `stream:true` and answers with a plain JSON object
  // is still usable. Detect it from the content type and fall back.
  const ctype = String(res.headers.get("content-type") || "");
  if (ctype.includes("application/json")) {
    clearTimeout(timer);
    if (signal) signal.removeEventListener("abort", onAbort);
    const parsed = parseResponse(cfg.protocol, await res.json());
    const msg = parsed.message || { role: "assistant", content: "" };
    if (msg.content) emit({ type: "delta", text: String(msg.content) });
    return { message: msg, finish: parsed.finish || "", usage: parsed.usage };
  }

  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  let eventName = "";
  let dataBuf = [];           // SSE events can span several `data:` lines
  let content = "";
  let toolCalls = [];
  let finish = "";
  let usage = null;
  let streamError = "";

  // Some OpenAI-compatible relays re-send the full tool call arguments in every
  // chunk. Blind string concatenation then produces duplicated JSON; the merge
  // helper above decides per chunk whether it is a full value or a fragment.
  const applyTool = (tc) => {
    const i = Number(tc.index) || 0;
    if (!toolCalls[i]) {
      toolCalls[i] = { id: "", type: "function", function: { name: "", arguments: "" } };
    }
    mergeToolCallChunk(toolCalls[i], tc);
  };

  const emitThinking = (text) => { if (text) emit({ type: "thinking", text }); };

  // Decode once, in protocols.js. Keeping a second copy of the wire-format
  // shape sniffing here is exactly how the two agent loops used to drift.
  const handleFrame = (data) => {
    const f = parseFrame(cfg.protocol, eventName, data);
    if (f.done) return;
    if (f.error) { streamError = f.error; return; }
    if (f.usage) usage = mergeUsage(usage, f.usage);
    if (f.finish) finish = f.finish;
    if (f.text) {
      content += f.text;
      emit({ type: "delta", text: f.text });
    }
    if (f.thinking) emitThinking(f.thinking);
    for (const tc of f.toolCalls) applyTool(tc);
  };

  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      let nl;
      while ((nl = buffer.indexOf("\n")) >= 0) {
        const line = buffer.slice(0, nl).replace(/\r$/, "");
        buffer = buffer.slice(nl + 1);
        // Blank line ends one SSE frame; Anthropic puts the event type on an
        // `event:` line, OpenAI does not. Comments are keepalives.
        if (line === "") {
          if (dataBuf.length) { handleFrame(dataBuf.join("\n")); dataBuf = []; }
          eventName = "";
          continue;
        }
        if (line.startsWith(":")) continue;
        if (line.startsWith("event:")) { eventName = line.slice(6).trim(); continue; }
        if (line.startsWith("data:")) { dataBuf.push(line.slice(5).replace(/^ /, "")); continue; }
        if (line.startsWith("id:")) continue;
        if (line.startsWith("retry:")) continue;
      }
      if (streamError) break;
    }
    if (dataBuf.length) handleFrame(dataBuf.join("\n"));   // final frame without a trailing blank line
  } finally {
    clearTimeout(timer);
    if (signal) signal.removeEventListener("abort", onAbort);
    try { reader.releaseLock(); } catch (e) { }
  }

  if (streamError) throw new Error("model stream error: " + streamError);

  toolCalls = toolCalls.filter(Boolean);
  return {
    message: { role: "assistant", content, tool_calls: toolCalls },
    finish,
    usage
  };
}

// ---------------------------------------------------------------------------
// the turn
// ---------------------------------------------------------------------------

/**
 * @param {string} userText
 * @param {object} ctx { sessionId, turnId, listerIndex, tabPath, history, autoApprove,
 *                       modelOverride, signal, waitForApproval,
 *                       readonly, attachments }
 * @param {(event:object)=>void} emit
 */
async function runTurnStream(userText, ctx, emit) {
  // 回合开始就冻结配置：server.js 在每个 HTTP 请求上 reloadConfig()，
  // 用户中途切模型/改安全设置不能让同一个回合后半段换一套参数。
  const cfg = { ...config };

  const actions = [];
  const tools = [];
  let steps = 0;
  let lastText = "";
  let usage = null;
  let stopped = false;
  const cancelled = () => !!(ctx.signal && ctx.signal.aborted);

  emit({ type: "start", sessionId: ctx.sessionId, messageId: uid("msg") });

  // 高频、无歧义的意图直接命中结构化命令，不经过模型。
  // 放在 modelReadiness() 之前：即使没有配置 API Key，本地导航类命令也应能用。
  // readonly 模式跳过直调——它承诺"不执行任何修改"，连 view/tab 状态都不动，
  // 这类请求交给模型按只读提示词给出计划。
  const fast = ctx.readonly ? null : matchFastIntent(userText);
  if (fast && !cancelled()) {
    const label = toolLabel(fast.name, fast.args);
    const fastMeta = ApprovalOf(fast.name);
    emit({ type: "status", text: `正在执行 ${label}…` });
    const card = {
      id: uid("tool"), name: fast.name, command: fast.name, risk: toolRisk(fast.name),
      step: 1, ts: Date.now(), label, args: { ...fast.args }, status: "running",
      undoGroup: fastMeta.undoGroup, undoDomain: fastMeta.undoDomain
    };
    tools.push(card);
    emit({ type: "tool", ...card });

    const result = await dispatch(fast.name, fast.args, ctx.signal, false);
    const ok = !(result && result.error);
    const assertions = result && Array.isArray(result.__assertions) ? result.__assertions : null;
    const assertFailed = !!(assertions && assertions.some((a) => a && a.ok === false));
    card.status = ok ? (assertFailed ? "mismatch" : "done") : "error";
    card.result = clip(JSON.stringify(result), 8000);
    if (!ok) card.error = String(result.error);
    if (result && result.errorCode) card.errorCode = String(result.errorCode);
    if (assertions) card.assertions = assertions;
    emit({ type: "tool", ...card });

    const reply = ok
      ? (assertFailed ? `${fast.reply}（效果校验未通过，请看活动流详情）` : fast.reply)
      : `${label} 执行失败：${result.error}`;
    actions.push(formatAction(fast.name, fast.args, result, ok && !assertFailed));
    emit({ type: "delta", text: reply });
    emit({ type: "done", reply, actions, tools, steps: 1, usage: null });
    return { reply, actions, tools, steps: 1, usage: null };
  }

  const ready = modelReadiness();
  if (!ready.ok) {
    emit({ type: "error", message: ready.reason });
    return { reply: "", actions, tools, steps, error: ready.reason };
  }

  // 【首轮不能空着工具清单去问模型】TOOL_SCHEMAS 是首次连桥时才填充的；
  // 若第一次模型调用抢在握手前发出，模型会以为自己没有工具。程序刚重启时
  // 旧管道可能刚退场、新管道还没建好，所以这里允许一次受控重探；重探后
  // 仍不可用才明确降级，并且结果要在状态流里说清楚。
  const toolPreflight = await EnsureReadyWithFallback(ctx.signal, 1200);
  const toolsReady = !!toolPreflight.ok;
  if (cancelled()) {
    return { reply: "", actions, tools, steps, usage: null, error: "stopped" };
  }
  if (toolPreflight.retried && toolsReady) {
    emit({ type: "status", text: `工具桥已恢复：本轮可用 ${toolPreflight.tools} 个工具` });
  }
  if (!toolsReady) {
    const reason = toolPreflight.reason ? `（${clip(toolPreflight.reason, 160)}）` : "";
    emit({ type: "status", text: `ExBarBrowser 未连接：本轮没有可用工具（已回退重探一次）${reason}` });
  }

  const history = Array.isArray(ctx.history) ? ctx.history : [];
  const anchor = [];
  if (typeof ctx.tabPath === "string" && ctx.tabPath) anchor.push(`the user's active folder is ${ctx.tabPath}`);
  if (typeof ctx.listerIndex === "number" && ctx.listerIndex >= 0) anchor.push(`Lister index ${ctx.listerIndex}`);
  // Structured attachments from the composer are explicit context, unlike the
  // folder chip (which only says where the window is). Keep them in the system
  // message so they never get mistaken for words the user typed.
  if (Array.isArray(ctx.attachments) && ctx.attachments.length) {
    anchor.push(`the user explicitly attached these paths: ${ctx.attachments.join(", ")}`);
  }
  // In read-only mode mutating tools are short-circuited below. Tell the model
  // up front so it answers with a plan instead of retrying the same call.
  if (ctx.readonly) {
    anchor.push("READ-ONLY MODE: do not change any file or folder; describe what you would do and list the exact commands instead");
  }
  if (!toolsReady) {
    anchor.push("ExBarBrowser tool bridge is not connected after one fallback retry; you have no tools this turn. Say so instead of pretending to act.");
  }
  const anchorText = anchor.length
    ? `\n\nSESSION CONTEXT (from the Opus window, may be stale by a turn): ${anchor.join("; ")}`
    : "";

  // Stable prefix (base prompt + knowledge card) is identical every turn, so
  // Anthropic can cache it; only retrieved sections and session state change.
  const systemText = SYSTEM_PROMPT + knowledgeCard() + CACHE_SPLIT
    + knowledgeHits(userText) + anchorText + CapabilityNote();

  const messages = [
    { role: "system", content: systemText },
    ...history.map((h) => ({ role: h.role === "assistant" ? "assistant" : "user", content: String(h.content || "") })),
    { role: "user", content: userText }
  ];

  const firstStatus = toolPreflight.retried
    ? (toolsReady ? "工具桥已恢复，正在理解意图…" : "工具桥仍不可用，继续理解意图…")
    : "正在理解意图…";
  while (steps < cfg.maxSteps && !cancelled()) {
    steps++;
    emit({ type: "status", text: steps === 1 ? firstStatus : "继续处理…" });

    let out;
    try {
      out = await callModelStream(messages, ctx.signal, emit, ctx.modelOverride, cfg);
    } catch (e) {
      const m = String(e && e.message ? e.message : e);
      emit({ type: "error", message: m, usage });
      return { reply: lastText, actions, tools, steps, usage, error: m };
    }
    // A tool-use turn makes several model calls. Usage must be the SUM of them,
    // not just the first step's numbers (max-based merge also avoids double
    // counting relays that repeat a cumulative usage chunk).
    usage = mergeUsage(usage, out.usage);

    const msg = out.message;
    const toolCalls = msg.tool_calls || [];
    if (msg.content) lastText = String(msg.content);

    if (toolCalls.length === 0) {
      if (cancelled()) { stopped = true; break; }
      const reply = lastText || "(the model returned an empty reply)";
      emit({ type: "done", reply, actions, tools, steps, usage });
      return { reply, actions, tools, steps, usage };
    }

    messages.push({
      role: "assistant",
      content: msg.content || "",
      tool_calls: toolCalls
    });

    emit({ type: "status", text: `正在执行 ${toolCalls.length} 个操作…` });

    for (const tc of toolCalls) {
      if (cancelled()) { stopped = true; break; }
      const id = tc.id || uid("tool");
      const name = tc.function && tc.function.name ? tc.function.name : "";
      const rawArgs = tc.function && tc.function.arguments ? tc.function.arguments : "{}";
      let args = {};
      try {
        args = JSON.parse(rawArgs);
      } catch (e) {
        const err = `arguments were not valid JSON: ${clip(rawArgs, 300)}`;
        messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify({ error: err }) });
        actions.push(`${name} (bad args)`);
        const card = { id, name, command: name, risk: toolRisk(name), step: tools.length + 1, ts: Date.now(), label: toolLabel(name, {}), args: {}, status: "error", error: err };
        tools.push(card);
        emit({ type: "tool", ...card });
        continue;
      }

      const label = toolLabel(name, args);
      // 工具元数据在整段回合里冻结一次：审批判据与发给状态栏的撤销域必须是
      // 同一份，否则"卡片说不可撤销、peek 却给撤销按钮"会变成假承诺。
      const cardUndoMeta = ApprovalOf(name);

      // Read-only mode: a mutating tool must never reach dispatch() and must
      // never open an approval card either. Returning a "not executed" result
      // lets the loop continue with read-only tools and produce a plan.
      // 判据从 destructiveHint 改成 readOnlyHint：以前只读模式只拦危险命令，
      // fs.rename / view.sort 这类普通写会悄悄执行，和"只读"文案不符。
      if (ctx.readonly && isMutating(cardUndoMeta)) {
        const result = { error: "not executed: read-only mode is active" };
        messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify(result) });
        actions.push(`${label}  [READ-ONLY]`);
        const card = { id, name, command: name, risk: toolRisk(name), step: tools.length + 1, ts: Date.now(), label, args, status: "rejected", error: "read-only mode" };
        tools.push(card);
        emit({ type: "tool", ...card });
        continue;
      }

      // --- human gate -----------------------------------------------------
      // 三种模式的真实口径（与 composer 菜单文案一致）：
      //   confirm : 所有会改状态的命令都弹卡
      //   auto    : 普通写自动执行；destructive 命令**仍然弹卡**
      //   readonly: 所有写命令在上一支已经被拦掉
      //
      // 绝不能因为 auto 模式就把 approved=true 传给 dispatch()：
      // 会话级免确认不是"这一次有人确认过"，宿主侧危险双门也不会认。
      // 现在 auto 模式遇到危险命令会正常弹卡，用户点确认后才带 approve=true。
      let approved = false;
      const approvalMeta = cardUndoMeta;
      if (shouldAskApproval(approvalMeta, ctx.autoApprove)) {
        const info = approvalInfo(name, args);
        const approvalId = uid("appr");
        emit({
          type: "approval",
          id: approvalId,
          toolId: id,
          name,
          title: info.title,
          description: info.description,
          details: info.details,
          args
        });
        let decision;
        if (typeof ctx.waitForApproval === "function") {
          try {
            decision = await ctx.waitForApproval(approvalId);
          } catch (e) {
            decision = { decision: "reject", reason: String(e && e.message ? e.message : e) };
          }
        } else {
          decision = { decision: "reject", reason: "no approval channel" };
        }
        if (!decision || decision.decision !== "approve") {
          const reason = (decision && decision.reason) || "user rejected this action";
          const result = { error: `not executed: ${reason}` };
          messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify(result) });
          actions.push(`${label}  [SKIPPED]`);
          const card = { id, name, command: name, risk: toolRisk(name), step: tools.length + 1, ts: Date.now(), label, args, status: "rejected", error: reason };
          tools.push(card);
          emit({ type: "tool", ...card });
          continue;
        }
        // 只有这一条路径能把 approved 置真：用户在本回合点了确认。
        approved = true;
      }

      // --- execute --------------------------------------------------------
      const card = { id, name, command: name, risk: toolRisk(name), step: tools.length + 1,
                     ts: Date.now(), label, args, status: "running",
                     undoGroup: approvalMeta.undoGroup, undoDomain: approvalMeta.undoDomain };
      tools.push(card);
      emit({ type: "tool", ...card });

      const result = await dispatch(name, args, ctx.signal, approved);
      // Stop may land while a bridge call is in flight. Mark the card as
      // cancelled rather than leaving a spinner running forever, and keep the
      // tool-result slot so the message array stays protocol-valid.
      if (cancelled()) {
        card.status = "cancelled";
        card.error = "stopped";
        emit({ type: "tool", ...card });
        messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify({ error: "stopped" }) });
        stopped = true;
        break;
      }
      const ok = !(result && result.error);
      actions.push(formatAction(name, args, result, ok));

      // verdict：断言存在时，它是"效果是否真的达成"的唯一判据。
      // 一条命令 handler 返回成功、但断言不符（handler 自称成了、实际没成）
      // 是最危险的一类结果 —— 卡片不能是绿色"完成"，必须单独一档。
      const assertions = result && Array.isArray(result.__assertions) ? result.__assertions : null;
      const assertFailed = !!(assertions && assertions.some((a) => a && a.ok === false));

      card.status = ok ? (assertFailed ? "mismatch" : "done") : "error";
      card.result = clip(JSON.stringify(result), 8000);   // same cap as summariseResult
      if (!ok) card.error = String(result.error);
      if (result && result.errorCode) card.errorCode = String(result.errorCode);
      if (assertions) card.assertions = assertions;
      emit({ type: "tool", ...card });

      messages.push({
        role: "tool",
        tool_call_id: tc.id,
        content: summariseResult(result)
      });
    }

    if (cfg.verbose) console.log(`[agent-stream] step ${steps}: ${toolCalls.length} tool call(s)`);
  }

  if (cancelled() || stopped) {
    emit({ type: "error", message: "已停止", usage });
    return { reply: lastText, actions, tools, steps, usage, error: "stopped" };
  }

  const reply = lastText
    ? `${lastText}\n\n(stopped after ${cfg.maxSteps} steps without a final answer)`
    : `Stopped after ${cfg.maxSteps} steps without reaching an answer. The request may need to be split into smaller parts.`;
  emit({ type: "done", reply, actions, tools, steps, usage });
  return { reply, actions, tools, steps, usage };
}

module.exports = { runTurnStream, needsApproval, toolLabel, approvalInfo, mergeToolCallChunk };
