// agent.js - the reasoning loop (classic AGENT/1 path).
//
// SHAPE
//   One user turn = a bounded loop of (model call -> tool calls -> tool results).
//   This is the standard tool-use loop, written out explicitly so the failure
//   modes are visible in one file: step budget, tool errors fed back as text
//   rather than thrown, and a final answer extracted from whatever the last
//   assistant turn produced.
//
// WHY ERRORS GO BACK TO THE MODEL AS TEXT
//   A tool error is information, not a crash. "path not found" lets the model
//   correct itself; an exception would end the turn with a stack trace the user
//   cannot act on. Only transport failures (no service, no key) abort.
//
// WHY THE ACTION LIST IS RETURNED SEPARATELY
//   The Opus window shows what was actually run, distinct from what the model
//   says it did. A model that claims "I deleted them" without calling anything
//   becomes visible immediately, which is the whole point of having a window
//   rather than a black-box script.
//
// THE SHARED GATE
//   needsApproval lives in prompt.js and is reused by both loops, because the
//   classic dialog and the web window must refuse mutating tools under the
//   same rules. The classic path has no approval UI and no waitForApproval,
//   so a mutating tool is refused with a reason the model can act on instead
//   of executing silently.

const { config, modelReadiness } = require("./config");
const { TOOL_SCHEMAS, dispatch, EnsureReady, CapabilityNote } = require("./tools-exbar");
const { requestSpec, parseResponse, buildChatBody, CACHE_SPLIT } = require("./protocols");
const { SYSTEM_PROMPT, clip, summariseResult, formatAction, needsApproval } = require("./prompt");
const { knowledgeCard, knowledgeHits } = require("./project-knowledge");

async function callModel(messages) {
  // The agent loop keeps messages in the OpenAI shape internally. The protocol
  // adapter chooses the endpoint path, auth header, request shape and response
  // decoder (buildChatBody translates to the Anthropic wire format when needed).
  const spec = requestSpec(config.protocol, config.baseUrl, config.apiKey, "chat");
  const body = buildChatBody(config.protocol, {
    model: config.model,
    messages,
    temperature: config.temperature,
    maxTokens: config.maxTokens,
    tools: TOOL_SCHEMAS,
    stream: false
  });

  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), config.timeoutMs || 180000);

  let res;
  try {
    res = await fetch(spec.url, {
      method: "POST",
      headers: spec.headers,
      body: JSON.stringify(body),
      signal: ctrl.signal
    });
  } catch (e) {
    clearTimeout(timer);
    const m = e && e.name === "AbortError" ? "request timed out" : String(e && e.message ? e.message : e);
    throw new Error(`cannot reach the model endpoint ${spec.url}: ${m}`);
  }
  clearTimeout(timer);

  const text = await res.text();
  if (!res.ok) {
    // Surface the provider's own message: "invalid api key" and "model not found"
    // are the two failures users actually hit, and both are in the body.
    let detail = text;
    try {
      const j = JSON.parse(text);
      detail = (j.error && (j.error.message || j.error.code)) || text;
    } catch (e) { }
    throw new Error(`model endpoint returned HTTP ${res.status}: ${clip(detail, 600)}`);
  }

  let json;
  try {
    json = JSON.parse(text);
  } catch (e) {
    throw new Error(`model endpoint returned non-JSON: ${clip(text, 400)}`);
  }

  const parsed = parseResponse(config.protocol, json);
  if (!parsed.message || (!parsed.message.content && !(parsed.message.tool_calls || []).length)) {
    // An empty, tool-free message usually means the response used an unexpected
    // shape. Keep the raw text in the error so it is diagnosable.
    if (!Array.isArray(json.choices) && !Array.isArray(json.content)) {
      throw new Error(`model endpoint returned an unrecognised response: ${clip(text, 400)}`);
    }
  }
  return parsed;
}

/**
 * Run one user turn to completion.
 *
 * @param {string} userText
 * @param {object} ctx   { sessionId, listerIndex, tabPath, history,
 *                         readonly?, attachments? }
 * @returns {{reply:string, actions:string[], steps:number, error?:string}}
 */
async function runTurn(userText, ctx = {}) {
  const ready = modelReadiness();
  if (!ready.ok) {
    return { reply: "", actions: [], steps: 0, error: ready.reason };
  }

  // 与 Web 回合同一条规则：第一次模型调用前完成工具握手。否则 TOOL_SCHEMAS
  // 还是空的，模型在首轮看不到任何命令，只会给出"我不能操作"的假答案。
  const toolsReady = await EnsureReady();

  const history = Array.isArray(ctx.history) ? ctx.history : [];
  const anchor = [];
  if (typeof ctx.tabPath === "string" && ctx.tabPath) anchor.push(`the user's active folder is ${ctx.tabPath}`);
  if (typeof ctx.listerIndex === "number" && ctx.listerIndex >= 0) anchor.push(`Lister index ${ctx.listerIndex}`);
  if (Array.isArray(ctx.attachments) && ctx.attachments.length) {
    anchor.push(`the user explicitly attached these paths: ${ctx.attachments.join(", ")}`);
  }
  // In read-only mode mutating tools are refused below. Tell the model up front
  // so it answers with a plan instead of retrying the same call.
  if (ctx.readonly) {
    anchor.push("READ-ONLY MODE: do not change any file or folder; describe what you would do and list the exact commands instead");
  }
  if (!toolsReady) {
    anchor.push("ExBarBrowser tool bridge is not connected; you have no tools this turn. Say so instead of pretending to act.");
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

  const actions = [];
  let steps = 0;
  let lastText = "";

  while (steps < config.maxSteps) {
    steps++;

    let out;
    try {
      out = await callModel(messages);
    } catch (e) {
      return {
        reply: "",
        actions,
        steps,
        error: String(e && e.message ? e.message : e)
      };
    }

    const msg = out.message;
    const toolCalls = msg.tool_calls || [];

    if (msg.content) lastText = String(msg.content);

    // No tool calls: this is the answer.
    if (toolCalls.length === 0) {
      return {
        reply: lastText || "(the model returned an empty reply)",
        actions,
        steps
      };
    }

    // Push the assistant turn verbatim -- the provider requires the exact
    // tool_calls array it produced to be echoed before the tool results.
    messages.push({
      role: "assistant",
      content: msg.content || "",
      tool_calls: toolCalls
    });

    for (const tc of toolCalls) {
      const name = tc.function && tc.function.name ? tc.function.name : "";
      let args = {};
      const rawArgs = tc.function && tc.function.arguments ? tc.function.arguments : "{}";
      try {
        args = JSON.parse(rawArgs);
      } catch (e) {
        // A model can emit malformed JSON. Report it back as the tool result so
        // the next turn can retry correctly, instead of throwing.
        messages.push({
          role: "tool",
          tool_call_id: tc.id,
          content: JSON.stringify({ error: `arguments were not valid JSON: ${clip(rawArgs, 300)}` })
        });
        actions.push(`${name} (bad args)`);
        continue;
      }

      // --- the shared gate ----------------------------------------------
      // Read-only mode: never reach dispatch(), never execute.
      if (ctx.readonly && needsApproval(name, args)) {
        messages.push({
          role: "tool",
          tool_call_id: tc.id,
          content: JSON.stringify({ error: "not executed: read-only mode is active" })
        });
        actions.push(`${name}  [READ-ONLY]`);
        continue;
      }

      // Approval: the classic AGENT/1 dialog has no approval UI and no
      // waitForApproval in this path. Refusing -- with a reason the model can
      // act on -- is the safe default; the request is carried out by the web
      // window where a human can confirm it. Executing silently would mean a
      // mutating tool runs with no confirmation at all.
      if (needsApproval(name, args)) {
        messages.push({
          role: "tool",
          tool_call_id: tc.id,
          content: JSON.stringify({ error: "not executed: this tool needs confirmation; run the request in the web window where you can approve it" })
        });
        actions.push(`${name}  [REFUSED]`);
        continue;
      }

      const result = await dispatch(name, args);
      const ok = !(result && result.error);
      actions.push(formatAction(name, args, result, ok));

      messages.push({
        role: "tool",
        tool_call_id: tc.id,
        content: summariseResult(result)
      });
    }

    if (config.verbose) {
      console.log(`[agent] step ${steps}: ${toolCalls.length} tool call(s)`);
    }
  }

  return {
    reply: lastText
      ? `${lastText}\n\n(stopped after ${config.maxSteps} steps without a final answer)`
      : `Stopped after ${config.maxSteps} steps without reaching an answer. The request may need to be split into smaller parts.`,
    actions,
    steps
  };
}

module.exports = { runTurn, SYSTEM_PROMPT, formatAction, clip, summariseResult };
