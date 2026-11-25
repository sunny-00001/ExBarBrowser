// protocols.js - the single source of truth for vendor presets and the two
// wire protocols the window supports.
//
// SCOPE
//   The agent loop keeps messages in the OpenAI shape internally. This module
//   owns everything that differs on the wire: endpoint path, auth header,
//   request body (including a full Anthropic /messages translation), SSE frame
//   decoding and non-streaming response decoding. Both protocols return the
//   same {message, finish, usage} shape to the loop.
//
// Nothing here holds a secret. Callers pass the key in and get headers back.

const PROTOCOLS = [
  {
    id: "openai",
    label: "OpenAI 兼容",
    chatPath: "/chat/completions",
    modelsPath: "/models",
    auth: "bearer"
  },
  {
    id: "anthropic",
    label: "Anthropic Messages",
    chatPath: "/messages",
    modelsPath: "/models",
    auth: "x-api-key"
  }
];

// Agent loops mark the boundary between the stable system prefix (base prompt
// + project knowledge card) and per-turn text (retrieved sections + session
// anchor). OpenAI-compatible endpoints just get the two halves joined; the
// Anthropic translator turns the first half into a cacheable system block.
// NUL keeps the marker out of anything a model could legitimately generate.
const CACHE_SPLIT = "\u0000EXBAR_CACHE_SPLIT\u0000";

// `defaultModel` is only used as a last-resort probe model when an endpoint
// does not expose /models. It is never silently activated.
const VENDORS = [
  { id: "openai",      name: "OpenAI",              baseUrl: "https://api.openai.com/v1",                              protocol: "openai",    defaultModel: "gpt-4o-mini" },
  { id: "deepseek",     name: "DeepSeek",            baseUrl: "https://api.deepseek.com/v1",                           protocol: "openai",    defaultModel: "deepseek-chat" },
  { id: "moonshot",     name: "Moonshot 月之暗面",   baseUrl: "https://api.moonshot.cn/v1",                            protocol: "openai",    defaultModel: "moonshot-v1-8k" },
  { id: "zhipu",        name: "智谱 GLM",            baseUrl: "https://open.bigmodel.cn/api/paas/v4",                  protocol: "openai",    defaultModel: "glm-4-flash" },
  { id: "siliconflow",  name: "硅基流动 SiliconFlow", baseUrl: "https://api.siliconflow.cn/v1",                          protocol: "openai",    defaultModel: "Qwen/Qwen2.5-7B-Instruct" },
  { id: "dashscope",    name: "阿里云百炼",          baseUrl: "https://dashscope.aliyuncs.com/compatible-mode/v1",       protocol: "openai",    defaultModel: "qwen-plus" },
  { id: "ark",          name: "火山方舟",            baseUrl: "https://ark.cn-beijing.volces.com/api/v3",               protocol: "openai",    defaultModel: "doubao-pro-32k" },
  { id: "anthropic",    name: "Anthropic",           baseUrl: "https://api.anthropic.com/v1",                          protocol: "anthropic", defaultModel: "claude-3-5-haiku-latest" },
  { id: "ollama",       name: "Ollama 本地",         baseUrl: "http://localhost:11434/v1",                              protocol: "openai",    defaultModel: "llama3.1" }
];

function protocolId(v) {
  const s = String(v == null ? "" : v).trim().toLowerCase();
  return PROTOCOLS.some((p) => p.id === s) ? s : "openai";
}

function protocolById(v) {
  const id = protocolId(v);
  return PROTOCOLS.find((p) => p.id === id) || PROTOCOLS[0];
}

function protocolCatalog() {
  return PROTOCOLS.map((p) => ({ id: p.id, label: p.label, chatPath: p.chatPath }));
}

function vendorCatalog() {
  return VENDORS.map((v) => ({
    id: v.id, name: v.name, baseUrl: v.baseUrl,
    protocol: v.protocol, defaultModel: v.defaultModel
  }));
}

function vendorById(v) {
  const s = String(v == null ? "" : v).trim().toLowerCase();
  return VENDORS.find((x) => x.id === s) || null;
}

// Human-readable service-provider name used to group models in the dropdown.
// Explicit names typed in the custom-model dialog always win; otherwise we
// recognise a preset by hostname, then fall back to a short brand-like label.
// Deliberately never returns a URL: the menu groups by provider, not by
// interface address. Only a whole endpoint gets a label -- inferring from a
// model id would split one relay's 300+ models into a dozen fake providers.
const VENDOR_BRANDS = [
  ["deepseek", "DeepSeek"], ["openai", "OpenAI"], ["anthropic", "Anthropic"],
  ["moonshot", "Moonshot 月之暗面"], ["kimi", "Moonshot 月之暗面"],
  ["bigmodel", "智谱 GLM"], ["zhipu", "智谱 GLM"],
  ["siliconflow", "硅基流动 SiliconFlow"], ["dashscope", "阿里云百炼"],
  ["aliyun", "阿里云百炼"], ["qwen", "阿里云百炼"],
  ["volces", "火山方舟"], ["doubao", "火山方舟"],
  ["ollama", "Ollama 本地"], ["groq", "Groq"], ["mistral", "Mistral"],
  ["gemini", "Google Gemini"], ["googleapis", "Google Gemini"]
];

function vendorLabel(baseUrl, vendor) {
  const explicit = String(vendor == null ? "" : vendor).trim();
  if (explicit) return explicit;

  const base = String(baseUrl == null ? "" : baseUrl).trim().replace(/\/+$/, "");
  let host = "";
  try { host = new URL(base).hostname.toLowerCase(); }
  catch (e) { host = base.replace(/^https?:\/\//i, "").split("/")[0].toLowerCase(); }
  if (!host) return "自定义接口";

  const preset = VENDORS.find((v) => {
    try { return new URL(v.baseUrl).hostname.toLowerCase() === host; }
    catch (e) { return false; }
  });
  if (preset) return preset.name;

  for (const [needle, label] of VENDOR_BRANDS) {
    if (host.includes(needle)) return label;
  }

  // A bare IP or localhost has no brand to borrow.
  if (/^(\d{1,3}\.){3}\d{1,3}$/.test(host) || host === "localhost") return "本地接口";

  // Last resort: the first hostname segment, without protocol/path/port.
  const first = host.split(".")[0].replace(/^(www|open|gateway)[-_.]?/i, "");
  return first ? first.charAt(0).toUpperCase() + first.slice(1) : "自定义接口";
}

// Build the URL + headers for one upstream call. `kind` is "chat" or "models".
// A fresh object is returned every time so callers can freely add Accept etc.
function requestSpec(protocol, baseUrl, apiKey, kind) {
  const p = protocolById(protocol);
  const base = String(baseUrl == null ? "" : baseUrl).trim().replace(/\/+$/, "");
  const url = base + (kind === "models" ? p.modelsPath : p.chatPath);
  const headers = {
    "Content-Type": "application/json",
    Accept: "application/json"
  };
  if (p.auth === "bearer") {
    headers.Authorization = `Bearer ${apiKey || ""}`;
  } else {
    headers["x-api-key"] = apiKey || "";
    headers["anthropic-version"] = "2023-06-01";
  }
  return { url, headers, protocol: p.id };
}

// Build the request body for one chat call. The agent loop keeps messages in
// the OpenAI shape internally; for the Anthropic wire protocol they are
// translated here (system as a top-level field, tool_use/tool_result content
// blocks, max_tokens required). A relay that speaks the Anthropic wire format
// while using the OpenAI schema is NOT supported: prefer the OpenAI-compatible
// preset for relays.
function buildChatBody(protocol, { model, messages, temperature, maxTokens, tools, stream = false }) {
  if (protocolById(protocol).id !== "anthropic") {
    const out = {
      model,
      // The cache marker is internal. Strict OpenAI-compatible relays reject
      // NUL bytes, so strip it before the wire call without mutating the loop.
      messages: (messages || []).map((m) => (
        m && m.role === "system" && typeof m.content === "string"
          ? { ...m, content: m.content.split(CACHE_SPLIT).join("") }
          : m
      )),
      temperature,
      max_tokens: maxTokens,
      tools,
      tool_choice: "auto",
      stream: !!stream
    };
    // include_usage is only legal on a streaming request; sending it on the
    // classic non-streaming path makes strict OpenAI-compatible relays 400.
    if (stream) out.stream_options = { include_usage: true };
    return out;
  }

  // Anthropic Messages: `system` is top-level, tool results are `tool_result`
  // blocks inside a user message, and a run of consecutive tool results must
  // be merged into ONE user message (consecutive same-role turns are invalid).
  // Build a fresh array instead of mutating the loop's history in place: the
  // original algorithm left stray role:"tool" entries in the payload, which
  // /messages rejects outright.
  const systemParts = [];
  const convo = [];
  for (const m of messages || []) {
    if (m.role === "system") {
      // Split on the cache marker and remember which half is stable. Only the
      // first half of the first system block is marked cacheable; the rest is
      // the per-turn tail (retrieved doc sections + session anchor).
      const halves = String(m.content || "").split(CACHE_SPLIT);
      halves.forEach((text, i) => {
        if (text) systemParts.push({ text, cache: systemParts.length === 0 && i === 0 && halves.length > 1 });
      });
      continue;
    }

    if (m.role === "user") {
      const text = typeof m.content === "string" ? m.content : "";
      convo.push({ role: "user", content: [{ type: "text", text }] });
      continue;
    }

    if (m.role === "assistant") {
      const text = typeof m.content === "string" ? m.content : "";
      const content = text ? [{ type: "text", text }] : [];
      for (const tc of (m.tool_calls || [])) {
        let input = {};
        try { input = JSON.parse((tc.function && tc.function.arguments) || "{}"); } catch (e) { }
        content.push({
          type: "tool_use",
          id: tc.id || "",
          name: (tc.function && tc.function.name) || "",
          input
        });
      }
      // /messages rejects an assistant message with zero content blocks.
      if (!content.length) content.push({ type: "text", text: "" });
      convo.push({ role: "assistant", content });
      continue;
    }

    if (m.role === "tool") {
      let payload = typeof m.content === "string" ? m.content : JSON.stringify(m.content || "");
      // Keep tool payloads bounded, matching summariseResult() in the loop.
      if (payload.length > 8000) payload = payload.slice(0, 8000) + " …[result trimmed]";
      const block = { type: "tool_result", tool_use_id: m.tool_call_id || "", content: payload };
      const last = convo[convo.length - 1];
      if (last && last.role === "user" && last.content.some((b) => b.type === "tool_result")) {
        last.content.push(block);
      } else {
        convo.push({ role: "user", content: [block] });
      }
    }
  }

  const out = {
    // `model` and `max_tokens` are required by /messages.
    model,
    max_tokens: maxTokens,
    messages: convo,
    tools: (tools || []).map((t) => ({
      name: (t.function && t.function.name) || "",
      description: (t.function && t.function.description) || "",
      input_schema: (t.function && t.function.parameters) || { type: "object", properties: {} }
    })),
    tool_choice: { type: "auto" },
    stream: !!stream
  };
  const systemBlocks = systemParts.map((p) => {
    const block = { type: "text", text: p.text };
    // Anthropic refuses to cache very short prefixes. The base prompt alone
    // clears this by a wide margin; a missing-doc fallback does not.
    if (p.cache && p.text.length >= 2000) block.cache_control = { type: "ephemeral" };
    return block;
  });
  if (systemBlocks.length === 1 && !systemBlocks[0].cache_control) out.system = systemBlocks[0].text;
  else if (systemBlocks.length) out.system = systemBlocks;
  if (temperature != null) out.temperature = temperature;
  return out;
}

// Normalize any usage object (OpenAI-style or Anthropic-style) into the
// {prompt_tokens, completion_tokens, total_tokens} shape the UI reads. Anthropic
// reports input/output plus cache tokens, which are part of the prompt cost.
function normalizeUsage(u) {
  if (!u || typeof u !== "object") return null;
  const prompt = Number(u.prompt_tokens || u.input_tokens || 0)
    + Number(u.cache_creation_input_tokens || 0)
    + Number(u.cache_read_input_tokens || 0);
  const completion = Number(u.completion_tokens || u.output_tokens || 0);
  if (!prompt && !completion) return null;
  return { prompt_tokens: prompt, completion_tokens: completion, total_tokens: prompt + completion };
}

// Usage arrives differently across protocols: OpenAI sends one final chunk,
// Anthropic splits input (message_start) and output (message_delta) across
// events, and some relays repeat the same cumulative numbers in later chunks.
// Taking the max per side merges Anthropic's split without double counting
// repeated OpenAI chunks. Raw Anthropic objects MUST be normalized first --
// otherwise their input_tokens/output_tokens were silently dropped.
function mergeUsage(a, b) {
  const an = normalizeUsage(a);
  const bn = normalizeUsage(b);
  if (!an) return bn || null;
  if (!bn) return an;
  const prompt = Math.max(an.prompt_tokens, bn.prompt_tokens);
  const completion = Math.max(an.completion_tokens, bn.completion_tokens);
  return { prompt_tokens: prompt, completion_tokens: completion, total_tokens: prompt + completion };
}

// Decode ONE SSE frame. `event` is the `event:` line (Anthropic uses it,
// OpenAI does not); `data` is the raw `data:` payload. The return shape is
// deliberately the same for both protocols so agent-stream.js has one loop.
function parseFrame(protocol, event, data) {
  const out = { text: "", thinking: "", toolCalls: [], finish: "", usage: null, done: false, error: "" };
  if (!data) return out;
  if (data === "[DONE]") { out.done = true; return out; }

  let j;
  try { j = JSON.parse(data); } catch (e) { return out; }
  if (!j || typeof j !== "object") return out;

  // OpenAI-shaped chunk. Many Anthropic-compatible relays emit exactly this,
  // so shape sniffing wins over the declared protocol.
  if (Array.isArray(j.choices)) {
    if (j.usage) out.usage = normalizeUsage(j.usage);
    const ch = j.choices[0] || {};
    if (ch.finish_reason) out.finish = ch.finish_reason;
    const d = ch.delta || {};
    if (typeof d.content === "string") out.text = d.content;
    // DeepSeek / OpenAI-o1 style reasoning is carried in `reasoning_content`.
    if (typeof d.reasoning_content === "string") out.thinking = d.reasoning_content;
    if (Array.isArray(d.tool_calls)) {
      for (const tc of d.tool_calls) {
        out.toolCalls.push({
          index: Number(tc.index) || 0,
          id: tc.id || "",
          name: (tc.function && tc.function.name) || "",
          arguments: (tc.function && tc.function.arguments) || ""
        });
      }
    }
    return out;
  }

  // Anthropic Messages event stream.
  const type = String(j.type || event || "");
  if (type === "content_block_start") {
    const cb = j.content_block || {};
    if (cb.type === "tool_use") {
      out.toolCalls.push({
        index: Number(j.index) || 0,
        id: cb.id || "",
        name: cb.name || "",
        arguments: ""
      });
    }
  } else if (type === "content_block_delta") {
    const d = j.delta || {};
    if (d.type === "text_delta") out.text = String(d.text || "");
    else if (d.type === "thinking_delta") out.thinking = String(d.thinking || "");
    else if (d.type === "input_json_delta") {
      out.toolCalls.push({
        index: Number(j.index) || 0,
        id: "", name: "", arguments: String(d.partial_json || "")
      });
    }
  } else if (type === "message_delta") {
    const d = j.delta || {};
    if (d.stop_reason) out.finish = d.stop_reason === "tool_use" ? "tool_calls" : d.stop_reason;
    if (j.usage) out.usage = normalizeUsage(j.usage);
  } else if (type === "message_start") {
    if (j.message && j.message.usage) out.usage = normalizeUsage(j.message.usage);
  } else if (type === "message_stop" || type === "done") {
    out.done = true;
  } else if (type === "error") {
    out.error = String((j.error && (j.error.message || j.error.type)) || "stream error");
  }
  return out;
}

// Decode a NON-streaming JSON response into the agent loop's {message, finish,
// usage}. Again, both shapes are accepted regardless of the declared protocol.
function parseResponse(protocol, json) {
  if (!json || typeof json !== "object") return { message: { role: "assistant", content: "" }, finish: "", usage: null };
  if (Array.isArray(json.choices)) {
    const ch = json.choices[0] || {};
    return {
      message: ch.message || { role: "assistant", content: "" },
      finish: ch.finish_reason || "",
      usage: normalizeUsage(json.usage)
    };
  }
  const blocks = Array.isArray(json.content) ? json.content : [];
  let text = "";
  const toolCalls = [];
  for (const b of blocks) {
    if (!b || typeof b !== "object") continue;
    if (b.type === "text") text += String(b.text || "");
    else if (b.type === "tool_use") {
      toolCalls.push({
        id: b.id || "", type: "function",
        function: { name: b.name || "", arguments: JSON.stringify(b.input || {}) }
      });
    }
  }
  return {
    message: { role: "assistant", content: text, tool_calls: toolCalls },
    finish: json.stop_reason === "tool_use" ? "tool_calls" : String(json.stop_reason || ""),
    usage: normalizeUsage(json.usage)
  };
}

// Pull a sorted, de-duplicated model-id array out of a /models response.
function modelIds(json) {
  const arr = Array.isArray(json) ? json
    : Array.isArray(json.data) ? json.data
      : Array.isArray(json.models) ? json.models : null;
  if (!arr) return null;
  const ids = arr
    .map((x) => typeof x === "string" ? x : (x && (x.id || x.name || x.model)))
    .filter(Boolean).map(String);
  return [...new Set(ids)].sort((a, b) => a.localeCompare(b));
}

module.exports = {
  PROTOCOLS, VENDORS,
  protocolId, protocolById, protocolCatalog, vendorCatalog, vendorById,
  vendorLabel,
  requestSpec, parseFrame, parseResponse, modelIds,
  buildChatBody, normalizeUsage, mergeUsage, CACHE_SPLIT
};
