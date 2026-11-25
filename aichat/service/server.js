// server.js - HTTP front end for the Opus agent chat.
//
// TWO CLIENTS, TWO PROTOCOLS
//   /agent      AGENT/1 text protocol. Kept for the classic Opus script dialog
//               (command AgentChatClassic) and its regression tests. The JScript
//               engine has no JSON object, so this stays line-oriented text.
//
//   /api/*      JSON + SSE for the WebView2 window. This is the real UI: it
//               streams model tokens, emits one event per tool call, and can
//               pause on a human approval gate before any write.
//
//   Static files for the window are served from ../ui. The socket is loopback
//   only, but loopback is not an authentication boundary: any web page can
//   issue a localhost request. /api/* therefore requires a per-launch opaque
//   token (EXBAR_HTTP_TOKEN from the native host) plus Host/Origin checks.

const http = require("http");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const {
  config, modelReadiness, CONFIG_PATH, MODEL_PATH, reloadConfig, configStatus,
  saveAll, keyHint
} = require("./config");
const {
  protocolId, protocolById, protocolCatalog, vendorCatalog, vendorById,
  vendorLabel, requestSpec, parseResponse, modelIds
} = require("./protocols");
const { runTurn } = require("./agent");
const { runTurnStream } = require("./agent-stream");
const exbar = require("./exbar-context");
const { createTurnState } = require("./turn-state");
const turnVersions = require("./turn-versions");
const { createBranchState } = require("./branch-state");

const UI_DIR = path.join(__dirname, "..", "ui");
const SESSION_DIR = path.join(config.logDir, "sessions");
const CONTEXT_FILE = path.join(config.logDir, "context.txt");
// 原生侧每次启动都会生成新的随机 token 并通过 EXBAR_HTTP_TOKEN 传进来。
// 直接 `node service/server.js` 调试时没有 token，此时退化为无 token 模式，
// 仅用于本机手工排障；正式嵌入路径永远带 token。
const HTTP_TOKEN = String(process.env.EXBAR_HTTP_TOKEN || "");
const MAX_BODY_BYTES = 256 * 1024;
// 回合编号不再从历史条数推算：历史截断或 /api/clear 之后必须继续往上走。
const TURNS = createTurnState(SESSION_DIR);
// 用户切回旧版本后的活动分支。与 turnId 分开存，避免把"默认最大 attempt"写成事实。
const BRANCHES = createBranchState(SESSION_DIR);

// ---------------------------------------------------------------------------
// classic AGENT/1 session map (unchanged behaviour)
// ---------------------------------------------------------------------------

const SESSIONS = new Map();
const SESSION_TTL_MS = 6 * 60 * 60 * 1000;
const MAX_HISTORY_MSGS = 40;

function log(...args) {
  console.log(new Date().toISOString().slice(11, 19), ...args);
}

function ensureLogDir() {
  try { fs.mkdirSync(config.logDir, { recursive: true }); } catch (e) { }
  try { fs.mkdirSync(SESSION_DIR, { recursive: true }); } catch (e) { }
}

function trimSessions() {
  const now = Date.now();
  for (const [k, v] of SESSIONS) {
    if (now - v.lastSeen > SESSION_TTL_MS) SESSIONS.delete(k);
  }
}

function parseRequest(body) {
  const nl = body.indexOf("\n");
  if (nl < 0) return null;
  if (body.slice(0, nl).trim() !== "AGENT/1") return null;
  let p = body.slice(nl + 1);
  const head = [];
  for (let i = 0; i < 3; i++) {
    const j = p.indexOf("\n");
    if (j < 0) return null;
    head.push(p.slice(0, j));
    p = p.slice(j + 1);
  }
  return {
    sessionId: head[0] || "default",
    listerIndex: Number(head[1]),
    tabPath: head[2] || "",
    text: p
  };
}

function packReply(reply, actions) {
  return "<<R>>\n" + reply + "\n<<E>>\n<<A>>\n" + (actions || []).join("\n") + "\n<<E>>\n";
}

function packError(msg) {
  return "<<X>>\n" + String(msg).replace(/\r?\n/g, " ");
}

async function handleAgent(req) {
  let raw = "";
  req.setEncoding("utf8");
  for await (const chunk of req) raw += chunk;

  const parsed = parseRequest(raw);
  if (!parsed) return { status: 400, body: packError("malformed request (expected AGENT/1 header)") };

  const text = parsed.text.trim();
  if (text === "") return { status: 200, body: packError("empty message") };

  trimSessions();
  let sess = SESSIONS.get(parsed.sessionId);
  if (!sess) {
    sess = { history: [], lastSeen: Date.now() };
    SESSIONS.set(parsed.sessionId, sess);
  }
  sess.lastSeen = Date.now();

  log(`classic turn session=${parsed.sessionId} chars=${text.length}`);
  let out;
  try {
    out = await runTurn(text, {
      sessionId: parsed.sessionId,
      listerIndex: parsed.listerIndex,
      tabPath: parsed.tabPath,
      history: sess.history
    });
  } catch (e) {
    return { status: 200, body: packError(String(e && e.message ? e.message : e)) };
  }

  if (out.error) return { status: 200, body: packError(out.error) };

  sess.history.push({ role: "user", content: text });
  sess.history.push({ role: "assistant", content: out.reply });
  if (sess.history.length > MAX_HISTORY_MSGS) {
    sess.history = sess.history.slice(sess.history.length - MAX_HISTORY_MSGS);
  }
  return { status: 200, body: packReply(out.reply, out.actions) };
}

// ---------------------------------------------------------------------------
// persisted web conversation history
// ---------------------------------------------------------------------------

function safeId(s) {
  return String(s || "default").replace(/[^a-zA-Z0-9_-]/g, "_").slice(0, 80) || "default";
}

function historyPath(sessionId) {
  return path.join(SESSION_DIR, safeId(sessionId) + ".json");
}

function loadHistory(sessionId) {
  try {
    const arr = JSON.parse(fs.readFileSync(historyPath(sessionId), "utf8"));
    return Array.isArray(arr) ? arr : [];
  } catch (e) {
    return [];
  }
}

function saveHistory(sessionId, arr) {
  try {
    fs.mkdirSync(SESSION_DIR, { recursive: true });
    fs.writeFileSync(historyPath(sessionId), JSON.stringify(arr, null, 2), "utf8");
  } catch (e) {
    log("history write failed: " + e.message);
  }
}

// Two turns on the same session can overlap (one waiting on an approval card
// while the user sends another). Both would loadHistory() -> push ->
// saveHistory() and the second write would silently erase the first turn.
// Serialize every read-modify-write per session through a promise chain.
const HISTORY_WRITES = new Map();

function queueHistoryWrite(sessionId, mutate) {
  const prev = HISTORY_WRITES.get(sessionId) || Promise.resolve();
  const next = prev.catch(() => undefined).then(() => mutate());
  HISTORY_WRITES.set(sessionId, next);
  const clear = () => { if (HISTORY_WRITES.get(sessionId) === next) HISTORY_WRITES.delete(sessionId); };
  next.then(clear, clear);
  return next;
}

// ---------------------------------------------------------------------------
// health / config / context
// ---------------------------------------------------------------------------

// opus.probe() can wait up to 8 seconds when Opus is not running. The UI polls
// /api/health on startup and after every turn; without a cache each poll pays
// that cost. Share one in-flight probe and reuse the result briefly.
let BRIDGE_CACHE = { at: 0, value: null };
let BRIDGE_PENDING = null;

function cachedBridgeProbe() {
  const now = Date.now();
  if (BRIDGE_CACHE.value && now - BRIDGE_CACHE.at < 10000) return Promise.resolve(BRIDGE_CACHE.value);
  if (BRIDGE_PENDING) return BRIDGE_PENDING;
  BRIDGE_PENDING = exbar.Probe()
    .then((value) => {
      BRIDGE_CACHE = { at: Date.now(), value };
      return value;
    })
    .catch((e) => ({ ok: false, reason: String(e && e.message ? e.message : e) }))
    .finally(() => { BRIDGE_PENDING = null; });
  return BRIDGE_PENDING;
}

async function healthPayload() {
  const bridge = await cachedBridgeProbe();
  const ready = modelReadiness();
  return {
    ok: true,
    ui: true,
    version: "2.0.0",
    model: {
      ready: ready.ok,
      reason: ready.reason,
      baseUrl: config.baseUrl,
      protocol: protocolId(config.protocol),
      vendor: String(config.vendor || "").trim(),
      model: config.model,
      keySet: !!config.apiKey
    },
    bridge,
    dryRun: config.dryRun,
    sessions: SESSIONS.size,
    uptimeSec: Math.round(process.uptime())
  };
}

// ---------------------------------------------------------------------------
// model catalogue, shared by /api/config and the window's model dropdown
// ---------------------------------------------------------------------------

function clip(s, n) {
  const t = String(s == null ? "" : s).replace(/\s+/g, " ").trim();
  return t.length > n ? t.slice(0, n) + "…" : t;
}

function hostLabel(baseUrl) {
  try { return new URL(String(baseUrl || "")).hostname; }
  catch (e) { return String(baseUrl || "").replace(/^https?:\/\//i, "").split("/")[0] || "默认接口"; }
}

// "" means "not supplied", null means "not a usable http(s) URL".
function normalizeBaseUrl(v) {
  const s = String(v == null ? "" : v).trim().replace(/\/+$/, "");
  if (!s) return "";
  return /^https?:\/\//i.test(s) ? s : null;
}

function savedProviders() {
  return Array.isArray(config.providers)
    ? config.providers.filter((p) => p && typeof p === "object")
    : [];
}

// One endpoint may speak two protocols with two different keys, so identity
// is the pair (protocol, baseUrl). Legacy entries without a protocol field
// count as "openai" -- that is what protocolId("") returns.
function providerProtocol(p) {
  return protocolId(p && p.protocol);
}

function sameProvider(p, base, protocol) {
  return (normalizeBaseUrl(p && p.baseUrl) || "") === base &&
    providerProtocol(p) === protocol;
}

// Resolve the real key for a (protocol, base) pair. The UI never receives this;
// it passes an empty apiKey plus useSavedKey when a saved provider is selected.
function keyForProvider(protocol, base) {
  const p = savedProviders().find((x) => sameProvider(x, base, protocol));
  if (p && p.apiKey) return p.apiKey;
  if ((normalizeBaseUrl(config.baseUrl) || "") === base) return config.apiKey;
  return "";
}

function resolveKey(body, base, protocol) {
  const typed = String(body.apiKey || "").trim();
  if (typed) return typed;
  // Editing a saved provider may change its endpoint. The key still belongs to
  // that provider, but it is stored under the OLD base and keyForProvider()
  // looks up by (protocol, base), so it would come back empty. `editKey` is
  // the stable identity the dialog sends: resolve through it first, then fall
  // back to the endpoint lookup. Used by validate/probe as well as save, so
  // all three agree on which secret a validation receipt is signed with.
  const editKey = String(body.editKey || "").trim();
  if (editKey) {
    const p = savedProviders().find((x) => String(x.key || "") === editKey);
    if (p && p.apiKey) return p.apiKey;
  }
  return keyForProvider(protocol, base);
}

function uniqueIds(arr) {
  return [...new Set((Array.isArray(arr) ? arr : [])
    .map((x) => String(x == null ? "" : x).trim()).filter(Boolean))];
}

// A save is only allowed after /api/models/validate succeeded for exactly
// this (protocol, base, key) combination. Receipts live in memory for ten
// minutes and never contain the key itself -- only a hash of it. This keeps the
// gate server-side without a second network round trip on every save.
const VALIDATIONS = new Map();
const RECEIPT_TTL_MS = 10 * 60 * 1000;

function receiptKey(protocol, base, key) {
  return crypto.createHash("sha256")
    .update(protocol + "\u0000" + base + "\u0000" + key, "utf8")
    .digest("hex");
}

function grantReceipt(protocol, base, key) {
  VALIDATIONS.set(receiptKey(protocol, base, key), Date.now() + RECEIPT_TTL_MS);
}

function hasReceipt(protocol, base, key) {
  const until = VALIDATIONS.get(receiptKey(protocol, base, key));
  return !!until && until > Date.now();
}

// A flat list for the dropdown. Two providers may expose the same model id, so
// the triple (protocol, baseUrl, id) is the identity, not the id alone.
function modelList() {
  const out = [];
  const seen = new Set();
  const activeBase = normalizeBaseUrl(config.baseUrl) || "";
  const activeProtocol = protocolId(config.protocol);
  const push = (id, name, base, protocol, apiKey) => {
    const modelId = String(id || "").trim();
    if (!modelId) return;
    const key = protocol + "\u0000" + base + "\u0000" + modelId;
    if (seen.has(key)) return;
    seen.add(key);
    out.push({
      id: modelId,
      provider: vendorLabel(base, name),
      vendor: name || "",
      baseUrl: base,
      protocol,
      hasKey: !!apiKey,
      // Two relays can expose the same model id under the same vendor name.
      // The masked key (never the full key) lets the dropdown tell the rows
      // apart without putting an endpoint address back on screen.
      keyHint: keyHint(apiKey),
      active: modelId === config.model && base === activeBase && protocol === activeProtocol
    });
  };

  for (const p of savedProviders()) {
    const base = normalizeBaseUrl(p.baseUrl) || "";
    const protocol = providerProtocol(p);
    const name = String(p.vendor || p.name || "").trim();
    const off = disabledSet(p);
    const ids = effectiveModels(p).models;
    // A disabled id stays in the provider's catalogue but must not be offered
    // in the dropdown. The active model is re-added unconditionally below, so
    // this filter can never leave the app pointing at an invisible model.
    ids.forEach((id) => { if (!off.has(id)) push(id, name, base, protocol, p.apiKey); });
  }
  // Always show the active model, even before any provider has been saved.
  push(config.model, String(config.vendor || "").trim(), activeBase, activeProtocol, config.apiKey);
  return out;
}

// One effective catalogue, read by BOTH the dropdown (modelList) and the
// custom-model dialog (providersPayload). A config that predates provider
// support -- or a provider saved before any model was added -- can have
// config.model pointing at an id that is not in providers[].models. Without
// this helper the dropdown shows that extra model while the dialog does not.
function effectiveModels(p) {
  const saved = Array.isArray(p && p.models) ? p.models.map(String) : [];
  const base = normalizeBaseUrl(p && p.baseUrl) || "";
  const isActive = base === (normalizeBaseUrl(config.baseUrl) || "") &&
    providerProtocol(p) === protocolId(config.protocol);
  const activeId = String(config.model || "").trim();
  const implicit = isActive && activeId && !saved.includes(activeId) ? [activeId] : [];
  return { models: [...saved, ...implicit], implicit };
}

// Every write that touches the active triple or the provider list must also
// register the active model in its matching provider. Otherwise switching
// away would drop that id from the catalogue for good: modelList() only
// re-adds the *current* model, and /api/config/model rejects ids that are
// not in a saved provider.
//
// saveAll splits the flat object back into aichat.json (behaviour) and
// models.json (active triple + providers), so callers keep mutating one view.
function saveConfigNormalized(mutate) {
  return saveAll((raw) => {
    const next = mutate(raw) || raw;
    const base = normalizeBaseUrl(next.baseUrl) || "";
    const protocol = protocolId(next.protocol);
    const activeId = String(next.model || "").trim();
    const list = Array.isArray(next.providers) ? next.providers : [];
    if (base && activeId) {
      const p = list.find((x) => x && sameProvider(x, base, protocol));
      if (p) {
        const models = Array.isArray(p.models) ? p.models.map(String) : [];
        if (!models.includes(activeId)) p.models = [...models, activeId];
      }
    }
    return next;
  });
}

// Enabled state lives next to the id list, as the set of *disabled* ids. An
// absent/empty `disabled` array means "everything enabled", so configs written
// before this field existed keep behaving exactly as before.
function disabledSet(p) {
  return new Set(Array.isArray(p && p.disabled) ? p.disabled.map(String) : []);
}

function providersPayload() {
  const activeBase = normalizeBaseUrl(config.baseUrl) || "";
  const activeProtocol = protocolId(config.protocol);
  const activeId = String(config.model || "").trim();
  const out = savedProviders().map((p, i) => {
    const base = normalizeBaseUrl(p.baseUrl) || "";
    const protocol = providerProtocol(p);
    const eff = effectiveModels(p);
    const off = disabledSet(p);
    return {
      index: i,
      // Stable identifier: the key this provider has in models.json#providers.
      // Renaming the provider or editing its URL keeps it; index does not.
      key: String(p.key || ""),
      vendor: String(p.vendor || p.name || "").trim() || hostLabel(p.baseUrl),
      name: String(p.vendor || p.name || "").trim() || hostLabel(p.baseUrl),
      baseUrl: String(p.baseUrl || ""),
      protocol,
      protocolLabel: protocolById(protocol).label,
      hasKey: !!p.apiKey,
      keyHint: keyHint(p.apiKey),        // masked, never the real key
      models: eff.models,
      implicitModels: eff.implicit,
      disabledModels: [...off],
      enabledCount: eff.models.filter((id) => !off.has(id)).length,
      active: base === activeBase && protocol === activeProtocol
    };
  });
  // A fresh install -- or an install where the only saved provider was removed
  // -- still has a live (baseUrl, apiKey, model) triple. modelList() always
  // offers that model in the dropdown; without a mirror row here the custom
  // model dialog would claim there are no providers at all. Expose a clearly
  // marked synthetic group so both views describe exactly the same catalogue.
  const hasActiveProvider = out.some((p) =>
    (normalizeBaseUrl(p.baseUrl) || "") === activeBase &&
    protocolId(p.protocol) === activeProtocol);
  if (activeBase && activeId && !hasActiveProvider) {
    const name = String(config.vendor || "").trim() || vendorLabel(activeBase);
    out.push({
      index: out.length,
      key: "",
      vendor: name,
      name,
      baseUrl: config.baseUrl,
      protocol: activeProtocol,
      protocolLabel: protocolById(activeProtocol).label,
      hasKey: !!config.apiKey,
      keyHint: keyHint(config.apiKey),
      models: [activeId],
      implicitModels: [activeId],
      disabledModels: [],
      enabledCount: 1,
      active: true,
      synthetic: true
    });
  }
  return out;
}

function configPayload() {
  const st = configStatus();
  const list = modelList();
  return {
    version: "2.0.0",
    model: config.model,
    baseUrl: config.baseUrl,
    protocol: protocolId(config.protocol),
    protocolLabel: protocolById(config.protocol).label,
    vendor: String(config.vendor || "").trim(),
    protocols: protocolCatalog(),
    vendors: vendorCatalog(),
    models: list.map((m) => m.id),
    modelList: list,
    providers: providersPayload(),
    hasApiKey: !!config.apiKey,
    keyHint: keyHint(config.apiKey),
    dryRun: config.dryRun,
    denyCommands: config.denyCommands,
    allowedRoots: config.allowedRoots,
    maxSteps: config.maxSteps,
    approvalDefault: !config.dryRun,
    configPath: st.path,
    modelConfigPath: st.modelPath,
    configError: st.error
  };
}

// Fetch with a hard timeout; used by probe / validate / test so a dead relay
// cannot wedge the dialog.
async function fetchWithTimeout(url, opts, timeoutMs) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs || 12000);
  try {
    return await fetch(url, { ...opts, signal: ctrl.signal });
  } finally {
    clearTimeout(timer);
  }
}

function upstreamError(prefix, status, text) {
  let detail = text;
  try {
    const j = JSON.parse(text);
    detail = (j.error && (j.error.message || j.error.code || j.error.type)) || text;
  } catch (e) { }
  return `${prefix} HTTP ${status}${detail ? ": " + clip(detail, 240) : ""}`;
}

// GET {baseUrl}/models (or the protocol's equivalent). Proxied server-side so
// the browser never has to deal with CORS and the key is not put in a URL.
async function probeModels(base, key, protocol) {
  const spec = requestSpec(protocol, base, key, "models");
  try {
    const r = await fetchWithTimeout(spec.url, { headers: spec.headers }, 12000);
    const text = await r.text();
    if (!r.ok) {
      return { ok: false, error: `HTTP ${r.status} ${r.statusText}`, detail: clip(text, 240), url: spec.url };
    }
    let data;
    try { data = JSON.parse(text); }
    catch (e) { return { ok: false, error: "响应不是 JSON", detail: clip(text, 240), url: spec.url }; }

    const models = modelIds(data);
    if (!models) {
      return { ok: false, error: "响应里没有模型数组", detail: clip(text, 240), url: spec.url };
    }
    return { ok: true, models, url: spec.url, protocol: spec.protocol };
  } catch (e) {
    return {
      ok: false,
      error: e.name === "AbortError" ? "请求超时（12 秒）" : (e.message || String(e)),
      url: spec.url
    };
  }
}

// One minimal, tool-free chat request. Used both by validation (when /models
// is unavailable) and by the per-model "测试" button. Never streams, never
// touches Opus, and caps output at one token.
async function pingModel(base, key, protocol, model) {
  const spec = requestSpec(protocol, base, key, "chat");
  const body = {
    model: String(model || "").trim() || "gpt-3.5-turbo",
    messages: [{ role: "user", content: "hi" }],
    max_tokens: 1,
    temperature: 0,
    stream: false
  };
  const t0 = Date.now();
  try {
    const r = await fetchWithTimeout(spec.url, {
      method: "POST",
      headers: spec.headers,
      body: JSON.stringify(body)
    }, 20000);
    const text = await r.text();
    const ms = Date.now() - t0;
    if (!r.ok) {
      return { ok: false, ms, error: upstreamError("模型请求失败", r.status, text), url: spec.url };
    }
    let json;
    try { json = JSON.parse(text); }
    catch (e) { return { ok: false, ms, error: "模型响应不是 JSON", detail: clip(text, 240), url: spec.url }; }
    if (json.error) {
      return { ok: false, ms, error: String(json.error.message || json.error.type || json.error), url: spec.url };
    }
    const parsed = parseResponse(protocol, json);
    return {
      ok: true, ms, url: spec.url,
      preview: clip(String((parsed.message && parsed.message.content) || "(空回复)"), 80),
      usage: parsed.usage
    };
  } catch (e) {
    return {
      ok: false,
      ms: Date.now() - t0,
      error: e.name === "AbortError" ? "请求超时（20 秒）" : (e.message || String(e)),
      url: spec.url
    };
  }
}

// Prove endpoint + key + protocol before anything is written to aichat.json.
// /models is free and usually present; a 404/405 falls back to a one-token
// chat request. 401/403 short-circuits with a clear "key invalid" message.
async function validateEndpoint(base, protocol, key, model) {
  const t0 = Date.now();
  const modelsSpec = requestSpec(protocol, base, key, "models");
  let fallbackReason = "";
  try {
    const r = await fetchWithTimeout(modelsSpec.url, { headers: modelsSpec.headers }, 12000);
    const text = await r.text();
    if (r.ok) {
      let models = [];
      try { models = modelIds(JSON.parse(text)) || []; } catch (e) { }
      return { ok: true, via: "models", models, ms: Date.now() - t0, url: modelsSpec.url, keyHint: keyHint(key) };
    }
    if (r.status === 401 || r.status === 403) {
      return { ok: false, error: "API Key 无效或没有权限 (HTTP " + r.status + ")", detail: clip(text, 240), ms: Date.now() - t0 };
    }
    fallbackReason = upstreamError("模型列表不可用", r.status, text);
  } catch (e) {
    fallbackReason = e.name === "AbortError" ? "模型列表请求超时" : (e.message || String(e));
  }

  const ping = await pingModel(base, key, protocol, model);
  if (ping.ok) {
    return { ok: true, via: "chat", models: [], ms: Date.now() - t0, url: ping.url, keyHint: keyHint(key) };
  }
  if (/HTTP 401|HTTP 403|invalid.?api.?key|unauthor/i.test(ping.error || "")) {
    return { ok: false, error: "API Key 无效或没有权限", detail: ping.error, ms: Date.now() - t0 };
  }
  return {
    ok: false,
    error: "无法验证该接口",
    detail: `${fallbackReason}；最小请求：${ping.error}`,
    ms: Date.now() - t0
  };
}

function readAnchor() {
  const out = { tabPath: "", listerIndex: -1, selectedCount: null, ts: 0 };
  try {
    const buf = fs.readFileSync(CONTEXT_FILE);
    let s = buf.toString("utf16le").replace(/^\uFEFF/, "");
    // A UTF-8 file reinterpreted as UTF-16 contains NULs; fall back to UTF-8.
    if (s.indexOf("\u0000") >= 0) s = buf.toString("utf8").replace(/^\uFEFF/, "");
    const lines = s.split(/\r?\n/);
    out.tabPath = (lines[0] || "").trim();
    out.listerIndex = Number((lines[1] || "").trim());
    if (!isFinite(out.listerIndex)) out.listerIndex = -1;
    const n = Number((lines[2] || "").trim());
    out.selectedCount = isFinite(n) && n >= 0 ? n : null;
    out.ts = Number((lines[3] || "").trim()) || 0;
  } catch (e) { }
  return out;
}

async function contextPayload(refresh) {
  const anchor = readAnchor();
  const result = {
    ok: true,
    folder: anchor.tabPath,
    folderName: anchor.tabPath ? anchor.tabPath.replace(/[\\/]+$/, "").split(/[\\/]/).pop() : "",
    listerIndex: anchor.listerIndex,
    selectedCount: anchor.selectedCount,
    selected: [],
    bridgeOk: false,
    reason: "",
    updatedAt: anchor.ts
  };

  if (refresh) {
    // 【这里以前适配的是 Directory Opus 的 listers/selection 结构】
    // 现在数据源是 ExBarBrowser 自己的命令（tool.list / win.list），
    // 字段形状完全不同：exbar-context.js 直接给出 folder / itemCount / selection。
    // 所以不再做 listers → folder 的翻译，直接用它的结果。
    try {
      const r = await Promise.race([
        exbar.ContextLite(),
        new Promise((_, reject) => setTimeout(() => reject(new Error("ExBarBrowser 未在 3.5s 内应答")), 3500))
      ]);
      if (r && r.ok) {
        result.bridgeOk = true;
        result.folder = r.folder || "";
        result.folderName = r.folderName || "";
        result.itemCount = typeof r.itemCount === "number" ? r.itemCount : 0;
        result.selectedCount = typeof r.selectedCount === "number" ? r.selectedCount : 0;
        result.selected = Array.isArray(r.selected) ? r.selected.slice(0, 8) : [];
        if (r.windowTitle) result.windowTitle = r.windowTitle;
        if (r.pid) result.pid = r.pid;
      } else {
        result.bridgeOk = false;
        result.reason = (r && r.reason) || "ExBarBrowser 未连接";
      }
    } catch (e) {
      result.bridgeOk = false;
      result.reason = e.message;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// approval gate
// ---------------------------------------------------------------------------

const APPROVALS = new Map();
// Monotonic across the whole service lifetime, so an EventSource-style client's
// Last-Event-ID never collides with an id from a previous turn.
let SSE_SEQ = 0;

function makeApprovalWaiter(signal) {
  return (id) => new Promise((resolve) => {
    let settled = false;
    // Every exit path (user decision, timeout, stop) must remove the abort
    // listener. Leaving it attached keeps the AbortController -- and the whole
    // turn closure -- alive until the SSE connection is collected.
    function onAbort() {
      clearTimeout(timer);
      if (signal) signal.removeEventListener("abort", onAbort);
      APPROVALS.delete(id);
      if (!settled) {
        settled = true;
        resolve({ decision: "reject", reason: "turn stopped" });
      }
    }
    const timer = setTimeout(() => {
      clearTimeout(timer);
      if (signal) signal.removeEventListener("abort", onAbort);
      APPROVALS.delete(id);
      if (!settled) {
        settled = true;
        resolve({ decision: "reject", reason: "confirmation timed out" });
      }
    }, 120000);
    if (signal) {
      if (signal.aborted) { onAbort(); return; }
      signal.addEventListener("abort", onAbort, { once: true });
    }
    APPROVALS.set(id, (decision) => {
      clearTimeout(timer);
      if (signal) signal.removeEventListener("abort", onAbort);
      if (settled) return;
      settled = true;
      resolve(decision);
    });
  });
}

// ---------------------------------------------------------------------------
// /api/chat - server-sent events
// ---------------------------------------------------------------------------

async function readJson(req) {
  const chunks = [];
  let bytes = 0;
  for await (const chunk of req) {
    const buf = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
    bytes += buf.length;
    if (bytes > MAX_BODY_BYTES) {
      const err = new Error("request body too large");
      err.status = 413;
      throw err;
    }
    chunks.push(buf);
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}");
}

// 普通 send 只看活动分支；retryOf 则从旧回合之前分叉。纯函数，便于自测。
function historyForChat(fullHistory, retryOf, selection) {
  if (!retryOf) {
    return {
      ok: true,
      history: turnVersions.activeHistory(fullHistory, selection).slice(-MAX_HISTORY_MSGS),
      lineage: null
    };
  }
  const lineage = turnVersions.lineageFor(fullHistory, retryOf);
  const context = lineage ? turnVersions.contextForRetry(fullHistory, retryOf, selection) : null;
  if (!lineage || !context) return { ok: false, error: "retry target not found" };
  return { ok: true, history: context.slice(-MAX_HISTORY_MSGS), lineage };
}

async function handleChat(req, res) {
  let body;
  try {
    body = await readJson(req);
  } catch (e) {
    const tooLarge = e && e.status === 413;
    res.writeHead(tooLarge ? 413 : 400, { "Content-Type": "application/json; charset=utf-8" });
    res.end(JSON.stringify({ error: tooLarge ? "request body too large" : "invalid JSON" }));
    return;
  }

  const text = String(body.text || "").trim();
  const sessionId = safeId(body.sessionId);
  // retryOf 表示"从旧回合的分叉点重来"，不是继续旧回合。具体血缘要在拿到
  // 会话锁、读到历史以后才能确认；这里只做外形解析。
  const retryOf = turnVersions.num(body.retryOf);
  if (!text) {
    res.writeHead(400, { "Content-Type": "application/json; charset=utf-8" });
    res.end(JSON.stringify({ error: "empty message" }));
    return;
  }

  // 同会话单飞：第二个请求先在锁外排队，拿到锁以后再 loadHistory()。
  // 这样它看到的一定是第一个回合写回之后的历史，而不是同一份陈旧快照。
  // 客户端在排队期间断开则直接放弃，不占用后面的回合号。
  const ctrl = new AbortController();
  let finished = false;
  let streamedText = "";
  const runId = crypto.randomUUID
    ? crypto.randomUUID()
    : `run-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  let writeFailed = false;
  let writeChain = Promise.resolve();
  const abortTurn = () => {
    if (!finished) {
      finished = true;
      ctrl.abort();
    }
  };
  res.once("close", abortTurn);

  // 一条 SSE 帧的写入必须 Respect backpressure：res.write() 返回 false 时
  // 等 drain 再写下一帧，否则慢客户端会把整段回复堆在 Node 内存里。
  // 写失败/连接断开要 abort 上游模型，不能继续烧 token。
  const writeSse = (frame) => new Promise((resolve) => {
    let done = false;
    const settle = () => {
      if (done) return;
      done = true;
      res.removeListener("close", onClose);
      resolve();
    };
    const onClose = () => {
      writeFailed = true;
      abortTurn();
      settle();
    };
    res.once("close", onClose);
    let ok = true;
    try { ok = res.write(frame); }
    catch (e) { writeFailed = true; abortTurn(); settle(); return; }
    if (ok) settle();
    else res.once("drain", settle);
  });

  await TURNS.withTurnRun(sessionId, async () => {
    if (ctrl.signal.aborted) return;

    const resolved = historyForChat(loadHistory(sessionId), retryOf, BRANCHES.all(sessionId));
    if (!resolved.ok) {
      finished = true;
      res.writeHead(400, { "Content-Type": "application/json; charset=utf-8" });
      res.end(JSON.stringify({ error: resolved.error }));
      return;
    }
    const { history, lineage } = resolved;
    // 权威回合号：写进 .turn.json 后才发事件，刷新、截断历史、清空会话都不复用。
    const turnId = TURNS.nextTurnId(sessionId);
    if (lineage) lineage.turnId = turnId;
    // 新回合挂在哪个已存在回合之后：retry 复用分叉目标的父节点，普通 send
    // 挂在当前上下文的最后一个回合。父链是切换旧版本后过滤后代的依据。
    const parentTurnId = lineage ? lineage.parentTurnId : turnVersions.lastTurnId(history);

    res.writeHead(200, {
      "Content-Type": "text/event-stream; charset=utf-8",
      "Cache-Control": "no-cache, no-transform",
      Connection: "keep-alive",
      "X-Accel-Buffering": "no"
    });

    let localSeq = 0;
    const emit = (event) => {
      if (finished || writeFailed) return;
      if (event.type === "delta") streamedText += event.text;
      // 每个事件都带同一个回合身份和 run 身份。前端据此拒绝上一回合迟到的
      // done/error，避免旧事件清掉新回合的 busy 状态和审批卡。
      event.turn = turnId;
      event.turnId = turnId;
      event.run = runId;
      event.seq = ++localSeq;
      if (parentTurnId) event.parentTurnId = parentTurnId;
      if (lineage) {
        event.retryOf = lineage.targetTurnId;
        event.rootTurnId = lineage.rootTurnId;
        event.attempt = lineage.attempt;
      }
      // Stable ids let a reconnecting client state where it left off, and a
      // `retry` hint stops an EventSource-style client from hammering the service.
      const frame = `id: ${++SSE_SEQ}\ndata: ${JSON.stringify(event)}\n\n`;
      writeChain = writeChain.then(() => writeSse(frame));
    };

    writeChain = writeChain.then(() => writeSse("retry: 3000\n\n"));

    const keepalive = setInterval(() => {
      if (!finished && !writeFailed) writeChain = writeChain.then(() => writeSse(": keep\n\n"));
    }, 15000);

    // Approval ids created by this turn. Anything still pending when the turn
    // ends is resolved as "reject": otherwise the waiter, its 120 s timer and the
    // clickable card all outlive the conversation that created them.
    const pendingApprovals = new Set();
    const baseWaiter = makeApprovalWaiter(ctrl.signal);
    const waitForApproval = (id) => {
      pendingApprovals.add(id);
      return baseWaiter(id).finally(() => pendingApprovals.delete(id));
    };

    // Optional composer controls. Clients that predate them send neither field,
    // so the defaults below reproduce the old behaviour exactly: autoApprove is
    // read straight from the body and no attachment context is injected.
    const mode = ["confirm", "auto", "readonly"].includes(String(body.mode || ""))
      ? String(body.mode) : "";
    const attachments = (Array.isArray(body.attachments) ? body.attachments : [])
      .map((a) => String(typeof a === "string" ? a : ((a && (a.path || a.name)) || "")).trim())
      .filter(Boolean).slice(0, 8);
    const ctx = {
      sessionId,
      turnId,
      mode,
      tabPath: String(body.tabPath || readAnchor().tabPath || ""),
      listerIndex: Number.isFinite(Number(body.listerIndex)) ? Number(body.listerIndex) : readAnchor().listerIndex,
      history,
      autoApprove: mode ? mode === "auto" : body.autoApprove === true,
      readonly: mode === "readonly",
      attachments,
      modelOverride: String(body.modelOverride || "").trim(),
      signal: ctrl.signal,
      waitForApproval
    };

    if (lineage) {
      ctx.retryOf = lineage.targetTurnId;
      ctx.rootTurnId = lineage.rootTurnId;
      ctx.attempt = lineage.attempt;
    }

    log(`web turn session=${sessionId} turn=${turnId} chars=${text.length} autoApprove=${ctx.autoApprove}` +
      (mode ? ` mode=${mode}` : "") +
      (lineage ? ` retryOf=${lineage.targetTurnId} root=${lineage.rootTurnId} attempt=${lineage.attempt}` : "") +
      (attachments.length ? ` attachments=${attachments.length}` : ""));
    const t0 = Date.now();

    try {
      let out;
      try {
        out = await runTurnStream(text, ctx, emit);
      } catch (e) {
        emit({ type: "error", message: String(e && e.message ? e.message : e) });
        out = { error: String(e && e.message ? e.message : e), actions: [], tools: [], steps: 0 };
      }

      const ms = Date.now() - t0;
      log(`web turn session=${sessionId} turn=${turnId} done in ${ms} ms, ${out.steps || 0} step(s), ${(out.actions || []).length} action(s)` +
        (out.error ? `, error: ${out.error}` : ""));

      // Persist EVERY turn, including failed and stopped ones. The user's message
      // is real input even when the model never answered, and a partial reply is
      // worth keeping. An `error` marker lets the UI render it as a broken turn.
      const reply = out.reply || streamedText || "";
      const turnMeta = {
        parentTurnId,
        ...(lineage ? {
          retryOf: lineage.targetTurnId,
          rootTurnId: lineage.rootTurnId,
          attempt: lineage.attempt
        } : {})
      };
      await queueHistoryWrite(sessionId, () => {
        const h = loadHistory(sessionId);
        if (lineage) turnVersions.markSuperseded(h, lineage, turnId);
        h.push({ role: "user", content: text, ts: Date.now(), turn: turnId, turnId, ...turnMeta });
        if (reply || out.error) {
          const entry = {
            role: "assistant",
            content: reply,
            ts: Date.now(),
            actions: out.actions || [],
            turn: turnId,
            turnId,
            ...turnMeta,
            // 完整字段随历史落盘：活动流视图刷新后要能重建风险徽章、参数、
            // 断言与失败原因，只存 name/label/status 会退化成"只有聊天记录"。
            tools: (out.tools || []).map((t) => ({
              id: t.id, name: t.name, command: t.command || t.name,
              label: t.label, status: t.status, risk: t.risk || "",
              step: t.step || 0, ts: t.ts || 0, turn: turnId, turnId,
              ...turnMeta,
              args: t.args || {}, result: t.result || "",
              error: t.error || "", errorCode: t.errorCode || "",
              assertions: t.assertions || []
            }))
          };
          if (out.error) entry.error = out.error;
          h.push(entry);
        }
        if (h.length > MAX_HISTORY_MSGS) h.splice(0, h.length - MAX_HISTORY_MSGS);
        saveHistory(sessionId, h);
      });
      const selectedRoot = lineage ? lineage.rootTurnId : turnId;
      if (!BRANCHES.select(sessionId, selectedRoot, turnId)) {
        log(`branch selection write failed: session=${sessionId} root=${selectedRoot} turn=${turnId}`);
      }
    } finally {
      for (const id of pendingApprovals) {
        const fn = APPROVALS.get(id);
        APPROVALS.delete(id);
        if (fn) fn({ decision: "reject", reason: "turn ended" });
      }
      pendingApprovals.clear();
      finished = true;
      clearInterval(keepalive);
      // 先把队列里最后一个 done/error 真正写出去，再结束响应。
      await writeChain.catch(() => {});
      try { res.end(); } catch (e) { }
    }
  });
}

// ---------------------------------------------------------------------------
// static files
// ---------------------------------------------------------------------------

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".ico": "image/x-icon",
  ".json": "application/json; charset=utf-8"
};

function serveStatic(urlPath, res) {
  let rel = urlPath.split("?")[0];
  if (rel === "/" || rel === "") rel = "/index.html";
  const full = path.resolve(UI_DIR, "." + rel);
  if (full !== UI_DIR && !full.startsWith(UI_DIR + path.sep)) {
    res.writeHead(403); res.end("forbidden"); return;
  }
  fs.readFile(full, (err, data) => {
    if (err) {
      res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
      res.end("not found: " + rel);
      return;
    }
    res.writeHead(200, {
      "Content-Type": MIME[path.extname(full).toLowerCase()] || "application/octet-stream",
      // WebView2 keeps a persistent user-data folder. `no-cache` alone let a
      // restarted host reuse an old app.js/app.css; no-store makes UI edits
      // visible on the next window launch without clearing the profile.
      "Cache-Control": "no-store, no-cache, must-revalidate",
      "Pragma": "no-cache",
      "Expires": "0",
      // 页面 URL 上带 exbarHttpToken；别让它通过 Referer 漏给任何外链。
      "Referrer-Policy": "no-referrer"
    });
    res.end(data);
  });
}

// ---------------------------------------------------------------------------
// router
// ---------------------------------------------------------------------------

function loopbackName(value) {
  const s = String(value || "").trim().toLowerCase().replace(/^\[|\]$/g, "");
  return s === "127.0.0.1" || s === "localhost" || s === "::1";
}

// DNS rebinding / 跨端口页面都不能驱动这个本地 API。
function hostAllowed(req) {
  const host = String(req.headers.host || "").trim();
  const m = host.match(/^\[([^\]]+)\](?::(\d+))?$/);
  const name = m ? m[1] : host.replace(/:\d+$/, "");
  const port = m ? m[2] : (host.includes(":") ? host.split(":").pop() : "");
  if (!loopbackName(name)) return false;
  return !port || Number(port) === Number(config.port);
}

function originAllowed(req) {
  const origin = req.headers.origin;
  if (!origin) return true;   // curl / native 调用没有 Origin；token 仍是硬门
  try {
    const o = new URL(String(origin));
    return o.protocol === "http:" && loopbackName(o.hostname) &&
      (!o.port || Number(o.port) === Number(config.port));
  } catch (e) {
    return false;
  }
}

function tokenAllowed(req, url) {
  if (!HTTP_TOKEN) return true;   // 手工 `node server.js` 调试的兼容退路
  const header = String(req.headers["x-exbar-token"] || "");
  const query = String(url.searchParams.get("exbarHttpToken") || "");
  return header === HTTP_TOKEN || query === HTTP_TOKEN;
}

function json(res, status, obj) {
  res.writeHead(status, { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-cache" });
  res.end(JSON.stringify(obj));
}

async function handleApi(req, res, url) {
  const pathname = url.pathname;

  if (req.method === "GET" && (pathname === "/api/health" || pathname === "/health")) {
    json(res, 200, await healthPayload());
    return true;
  }
  if (req.method === "GET" && pathname === "/api/config") {
    json(res, 200, configPayload());
    return true;
  }
  // Prove endpoint + key + protocol before anything can be written. A
  // successful call grants a short-lived in-memory receipt that the save
  // endpoint requires. The key itself is never echoed back, only keyHint.
  if (req.method === "POST" && pathname === "/api/models/validate") {
    const body = await readJson(req);
    const base = normalizeBaseUrl(body.baseUrl);
    const protocol = protocolId(body.protocol);
    if (!base) {
      json(res, 400, { ok: false, error: "接口地址无效：请填写 http(s):// 开头的 URL" });
      return true;
    }
    const key = resolveKey(body, base, protocol);
    if (!key) {
      json(res, 400, { ok: false, error: "还没有 API Key：请填写，或选择一个已保存的服务商" });
      return true;
    }
    const preset = vendorById(body.vendorId);
    const model = String(body.model || "").trim()
      || (preset && preset.defaultModel) || config.model || "";
    const r = await validateEndpoint(base, protocol, key, model);
    if (r.ok) grantReceipt(protocol, base, key);
    json(res, 200, r);
    return true;
  }
  // Ask the endpoint for its model list. The key goes in the POST body (never
  // the URL); leaving it blank reuses the saved key for that (protocol, base)
  // pair, so the UI never needs to display a secret.
  if (req.method === "POST" && pathname === "/api/models/probe") {
    const body = await readJson(req);
    const base = normalizeBaseUrl(body.baseUrl);
    if (!base) {
      json(res, 400, { ok: false, error: "接口地址无效：请填写 http(s):// 开头的 URL" });
      return true;
    }
    const protocol = protocolId(body.protocol);
    const key = resolveKey(body, base, protocol);
    json(res, 200, await probeModels(base, key, protocol));
    return true;
  }
  // Per-model "测试": one tool-free, one-token request through the same
  // protocol adapter a real chat turn uses. No Opus work, no history, no SSE.
  if (req.method === "POST" && pathname === "/api/models/test") {
    const body = await readJson(req);
    const base = normalizeBaseUrl(body.baseUrl);
    const protocol = protocolId(body.protocol);
    const model = String(body.model || "").trim();
    if (!base || !model) {
      json(res, 400, { ok: false, error: "缺少接口地址或模型 ID" });
      return true;
    }
    const key = resolveKey(body, base, protocol);
    if (!key) {
      json(res, 400, { ok: false, error: "该服务商没有保存 API Key" });
      return true;
    }
    json(res, 200, await pingModel(base, key, protocol, model));
    return true;
  }
  // Switch the active model. Because the same id may exist on two endpoints
  // (and two protocols), the caller passes the full (protocol, baseUrl, id)
  // triple; the endpoint's own key and vendor are restored with it.
  if (req.method === "POST" && pathname === "/api/config/model") {
    const body = await readJson(req);
    const id = String(body.model || "").trim();
    const activeBase = normalizeBaseUrl(config.baseUrl) || "";
    const base = body.baseUrl ? normalizeBaseUrl(body.baseUrl) : activeBase;
    if (!id || !base) {
      json(res, 400, { error: "模型参数无效" });
      return true;
    }
    const wanted = body.protocol ? protocolId(body.protocol) : "";
    const item = modelList().find((m) => m.id === id && m.baseUrl === base &&
      (!wanted || m.protocol === wanted));
    const protocol = item ? item.protocol : protocolId(body.protocol || config.protocol);
    const p = savedProviders().find((x) => sameProvider(x, base, protocol) &&
      (Array.isArray(x.models) ? x.models : []).map(String).includes(id));
    if (p) {
      if (!p.apiKey) {
        json(res, 400, { error: "该接口没有保存 API Key，请在自定义模型里重新配置" });
        return true;
      }
      saveConfigNormalized((raw) => {
        raw.model = id;
        raw.baseUrl = p.baseUrl;
        raw.apiKey = p.apiKey;
        raw.protocol = protocol;
        raw.vendor = String(p.vendor || p.name || "").trim();
        return raw;
      });
    } else if (!(id === config.model && base === activeBase && protocol === protocolId(config.protocol))) {
      json(res, 404, { error: "该模型不在已保存的接口里，请先在自定义模型中添加" });
      return true;
    }
    log(`[config] model switched -> ${id} @ ${protocol}:${base}`);
    json(res, 200, configPayload());
    return true;
  }
  // Upsert a provider shell. Save is gated by a validation receipt; models
  // are merged rather than replaced, so editing a saved provider never drops
  // the model list the user already built.
  if (req.method === "POST" && pathname === "/api/config/providers") {
    const body = await readJson(req);
    const base = normalizeBaseUrl(body.baseUrl);
    const protocol = protocolId(body.protocol);
    if (!base) {
      json(res, 400, { error: "接口地址无效：请填写 http(s):// 开头的 URL" });
      return true;
    }
    const key = resolveKey(body, base, protocol);
    if (!key) {
      json(res, 400, { error: "这个接口还没有 API Key，请填写后再保存" });
      return true;
    }
    if (!hasReceipt(protocol, base, key)) {
      json(res, 403, { error: "API Key 尚未验证通过，请先点击「验证」" });
      return true;
    }
    const vendor = String(body.vendor || body.name || "").trim() || hostLabel(base);
    const incoming = uniqueIds(body.models);
    const mergeModels = body.mergeModels === true;
    const activate = String(body.activate || "").trim();

    // Identity is an explicit, stable models.json key -- NOT the display name
    // and NOT a guess from the endpoint. `editKey` is the only signal that
    // authorises replacing an existing provider, and it is precise enough to
    // let the endpoint itself change while `default` keeps pointing at the
    // same key. Without it the save is a plain (endpoint, protocol) upsert or
    // a brand-new provider; nothing is inferred from the typed vendor name.
    const editKey = String(body.editKey || "").trim();
    // Legacy clients sent `editFrom`. Honour it only when there is no real
    // editKey, and only for the same-endpoint rename it was designed for. A
    // different endpoint is always a new provider: deleting the old entry
    // here used to hand its key to the new one (silent data loss).
    const editFrom = !editKey && body.editFrom && typeof body.editFrom === "object"
      ? body.editFrom : null;
    const oldBase = editFrom ? normalizeBaseUrl(editFrom.baseUrl) : "";
    const oldProtocol = editFrom ? protocolId(editFrom.protocol) : "";
    const sameEndpointEdit = !!oldBase &&
      oldBase.toLowerCase() === String(base || "").toLowerCase();

    // Check the edit target BEFORE any write. A stale key or an endpoint that
    // already belongs to another provider must fail loudly instead of quietly
    // overwriting a saved entry or creating two providers for one endpoint.
    if (editKey) {
      const current = savedProviders();
      const target = current.find((p) => String(p.key || "") === editKey);
      if (!target) {
        json(res, 400, { error: "要编辑的服务商已不存在，请重新选择后再保存" });
        return true;
      }
      const clash = current.find((p) =>
        String(p.key || "") !== editKey && sameProvider(p, base, protocol));
      if (clash) {
        json(res, 400, {
          error: `该接口地址已属于服务商「${String(clash.vendor || clash.name || clash.key).trim()}」，请先移除重复项或改回原地址`
        });
        return true;
      }
    }

    saveConfigNormalized((raw) => {
      const list = Array.isArray(raw.providers) ? raw.providers : [];
      let idx = -1;
      let legacy = null;
      if (editKey) {
        // Exact replacement: the key pins the entry even when baseUrl or
        // protocol was edited in the form. No name matching, no guessing.
        idx = list.findIndex((p) => p && String(p.key || "") === editKey);
      } else {
        idx = list.findIndex((p) => p && sameProvider(p, base, protocol));
        if (idx < 0 && sameEndpointEdit) {
          const oldIdx = list.findIndex((p) => p && sameProvider(p, oldBase, oldProtocol));
          if (oldIdx >= 0) { legacy = list[oldIdx]; list.splice(oldIdx, 1); }
        }
      }
      const prev = idx >= 0 ? list[idx] : legacy;
      let models;
      if (mergeModels) {
        models = uniqueIds([...(prev && Array.isArray(prev.models) ? prev.models : []), ...incoming]);
      } else if (incoming.length) {
        models = incoming;
      } else {
        models = prev && Array.isArray(prev.models) ? prev.models.slice() : [];
      }
      const entry = { vendor, name: vendor, baseUrl: base, apiKey: key, protocol, models };
      // Keep the stable key so a rename or a URL edit cannot orphan the
      // `default` reference. An explicit editKey always wins; otherwise reuse
      // the key of the entry actually being replaced. config.js mints a fresh
      // slug only when there is none -- it never falls back to the name.
      if (editKey) entry.key = editKey;
      else if (prev && prev.key) entry.key = prev.key;
      // Saving the provider must not flip every switch back on, but ids sent in
      // this save are clearly wanted, so they come back enabled. Anything that
      // is no longer in `models` is dropped from the set to avoid orphans.
      const prevOff = prev && Array.isArray(prev.disabled) ? prev.disabled.map(String) : [];
      const kept = prevOff.filter((x) => models.includes(x) && !incoming.includes(x));
      if (kept.length) entry.disabled = kept;
      if (idx >= 0) list[idx] = entry; else list.push(entry);
      raw.providers = list;
      raw.baseUrl = base;
      raw.apiKey = key;
      raw.protocol = protocol;
      raw.vendor = vendor;
      if (activate) raw.model = activate;
      else if (models.length && !models.includes(raw.model)) raw.model = models[0];
      return raw;
    });
    log(`[config] provider saved -> ${vendor} [${protocol}] (${incoming.length} new, merged=${mergeModels})`);
    json(res, 200, configPayload());
    return true;
  }
  // Single-model add / edit / delete. Credentials do not change here, so no
  // validation receipt is needed -- but the provider must already be saved.
  if (req.method === "POST" && pathname === "/api/config/providers/models") {
    const body = await readJson(req);
    const base = normalizeBaseUrl(body.baseUrl);
    const protocol = protocolId(body.protocol);
    const action = String(body.action || "add").trim();
    const model = String(body.model || "").trim();
    const next = String(body.newModel || "").trim();
    if (!base) {
      json(res, 400, { error: "接口地址无效" });
      return true;
    }
    const activeBase = normalizeBaseUrl(config.baseUrl) || "";
    const isActivePair = base === activeBase && protocol === protocolId(config.protocol);
    const exists = savedProviders().some((p) => sameProvider(p, base, protocol));
    // "add" is also how the dialog registers an implicit model. When the live
    // triple has no saved provider yet, that must create the shell from the
    // credentials already in use instead of failing with a 404.
    if (!exists && !(isActivePair && action === "add")) {
      json(res, 404, { error: "请先验证并保存该服务商，再添加模型" });
      return true;
    }
    const additions = uniqueIds(body.models || (model ? [model] : []));
    if (action === "add" && !additions.length) {
      json(res, 400, { error: "请填写要添加的模型 ID" });
      return true;
    }
    if (action !== "add" && !model) {
      json(res, 400, { error: "缺少模型 ID" });
      return true;
    }
    let error = "";
    saveConfigNormalized((raw) => {
      const list = Array.isArray(raw.providers) ? raw.providers : [];
      let idx = list.findIndex((p) => p && sameProvider(p, base, protocol));
      if (idx < 0 && isActivePair && action === "add") {
        // Fresh install / only provider removed: the active triple is the only
        // source of truth. Build its provider shell from credentials that are
        // already live, so no unvalidated key can enter models.json here.
        const name = String(config.vendor || "").trim() || vendorLabel(base);
        list.push({ vendor: name, name, baseUrl: base, apiKey: config.apiKey,
          protocol, models: [] });
        idx = list.length - 1;
      }
      if (idx < 0) { error = "服务商不存在"; return raw; }
      const p = list[idx];
      let models = Array.isArray(p.models) ? p.models.map(String) : [];
      // `disabled` mirrors `models`: every id that is switched off. Kept in
      // sync here so a rename / re-add / delete never strands an orphan entry
      // that would silently disable a *future* model carrying the same id.
      let off = Array.isArray(p.disabled) ? p.disabled.map(String) : [];

      if (action === "add") {
        for (const id of additions) if (!models.includes(id)) models.push(id);
        // Re-adding a switched-off model is an explicit "I want it back".
        if (additions.length) off = off.filter((x) => !additions.includes(x));
        if (body.activate === true && additions.length) {
          raw.model = additions[additions.length - 1];
          raw.baseUrl = p.baseUrl;
          raw.apiKey = p.apiKey;
          raw.protocol = protocol;
          raw.vendor = String(p.vendor || p.name || "").trim();
        }
      } else if (action === "edit") {
        if (!next || next === model) { error = "新模型 ID 不能为空或与原值相同"; return raw; }
        const i = models.indexOf(model);
        if (i < 0) { error = "该服务商下没有这个模型"; return raw; }
        if (models.includes(next)) { error = "该模型 ID 已存在"; return raw; }
        models[i] = next;
        off = off.map((x) => (x === model ? next : x));
        // Per-model metadata in models.json is keyed by id (display name,
        // contextWindow, cost…). Follow the rename or it becomes an orphan and
        // the entry silently degrades to a bare {id}.
        if (p.meta && Object.prototype.hasOwnProperty.call(p.meta, model)) {
          p.meta[next] = p.meta[model];
          delete p.meta[model];
        }
        if (raw.model === model) raw.model = next;
      } else if (action === "delete") {
        const before = models.length;
        models = models.filter((x) => x !== model);
        if (models.length === before) { error = "该服务商下没有这个模型"; return raw; }
        // Deleting the only model of the interface currently in use would be a
        // silent no-op: raw.model falls back to the same id and
        // saveConfigNormalized() re-registers it. Reject with an actionable
        // message instead of pretending the delete succeeded.
        const activeHere = (normalizeBaseUrl(raw.baseUrl) || "") === base &&
          protocolId(raw.protocol) === protocol &&
          String(raw.model || "").trim() === model;
        if (activeHere && !models.length) {
          error = "这是当前生效接口里唯一的模型，请先添加其它模型或切换接口后再删除";
          return raw;
        }
        off = off.filter((x) => x !== model);
        if (p.meta) delete p.meta[model];   // no orphan metadata left behind
        // Keep the provider shell; only fall back to its first remaining model.
        if (raw.model === model) raw.model = models[0] || model;
      } else if (action === "enable" || action === "disable") {
        if (!models.includes(model)) { error = "该服务商下没有这个模型"; return raw; }
        if (action === "disable") {
          // A switched-off model is invisible in the dropdown, so disabling
          // the one currently in use would leave the chip pointing at a model
          // the user can no longer re-select. Require a switch first.
          const activeHere = (normalizeBaseUrl(raw.baseUrl) || "") === base &&
            protocolId(raw.protocol) === protocol &&
            String(raw.model || "").trim() === model;
          if (activeHere) {
            error = "当前生效的模型不能停用，请先切换到其它模型";
            return raw;
          }
          if (!off.includes(model)) off.push(model);
        } else {
          off = off.filter((x) => x !== model);
        }
      } else {
        error = "未知操作：" + action;
        return raw;
      }
      p.models = models;
      if (off.length) p.disabled = off; else delete p.disabled;
      list[idx] = p;
      raw.providers = list;
      return raw;
    });
    if (error) { json(res, 400, { error }); return true; }
    log(`[config] model ${action} -> ${model || additions.join(",")} @ ${protocol}:${base}`);
    json(res, 200, configPayload());
    return true;
  }
  if (req.method === "POST" && pathname === "/api/config/providers/remove") {
    const body = await readJson(req);
    const base = normalizeBaseUrl(body.baseUrl);
    if (!base) {
      json(res, 400, { error: "接口地址无效" });
      return true;
    }
    // Omitting protocol removes every entry at that base URL (legacy behaviour).
    const protocol = body.protocol ? protocolId(body.protocol) : "";
    saveConfigNormalized((raw) => {
      const list = Array.isArray(raw.providers) ? raw.providers : [];
      raw.providers = list.filter((p) => !(p &&
        (normalizeBaseUrl(p.baseUrl) || "") === base &&
        (!protocol || providerProtocol(p) === protocol)));
      return raw;
    });
    log(`[config] provider removed -> ${protocol || "all"}:${base}`);
    json(res, 200, configPayload());
    return true;
  }
  if (req.method === "GET" && pathname === "/api/context") {
    json(res, 200, await contextPayload(url.searchParams.get("refresh") === "1"));
    return true;
  }
  if (req.method === "GET" && pathname === "/api/history") {
    const sid = safeId(url.searchParams.get("sessionId"));
    json(res, 200, { sessionId: sid, messages: loadHistory(sid) });
    return true;
  }
  if (req.method === "GET" && pathname === "/api/branches") {
    const sid = safeId(url.searchParams.get("sessionId"));
    json(res, 200, {
      sessionId: sid,
      branches: turnVersions.branchList(loadHistory(sid), BRANCHES.all(sid))
    });
    return true;
  }
  if (req.method === "POST" && pathname === "/api/branch") {
    const body = await readJson(req);
    const sid = safeId(body.sessionId);
    const root = turnVersions.num(body.rootTurnId);
    const turn = turnVersions.num(body.turnId);
    await TURNS.withTurnRun(sid, async () => {
      const h = loadHistory(sid);
      const found = turnVersions.findTurn(h, turn);
      if (!found || !turnVersions.canSelectBranch(h, root, turn)) {
        json(res, 400, { error: "分支不存在" });
        return;
      }
      const selectedRoot = root || found.meta.rootTurnId;
      if (!BRANCHES.select(sid, selectedRoot, turn)) {
        json(res, 500, { error: "分支选择写盘失败" });
        return;
      }
      json(res, 200, { ok: true, rootTurnId: selectedRoot, turnId: turn });
    });
    return true;
  }
  if (req.method === "POST" && pathname === "/api/clear") {
    const body = await readJson(req);
    const sid = safeId(body.sessionId);
    // Go through the same per-session queue as /api/chat, otherwise clearing
    // during an in-flight turn races with that turn's history write.
    await queueHistoryWrite(sid, () => {
      saveHistory(sid, []);
      BRANCHES.clear(sid);
    });
    json(res, 200, { ok: true });
    return true;
  }
  if (req.method === "POST" && pathname === "/api/approve") {
    const body = await readJson(req);
    const id = String(body.id || "");
    const fn = APPROVALS.get(id);
    if (!fn) {
      json(res, 404, { error: "no pending approval with that id" });
      return true;
    }
    APPROVALS.delete(id);
    fn({ decision: body.decision === "approve" ? "approve" : "reject", reason: body.reason || "" });
    json(res, 200, { ok: true });
    return true;
  }
  if (req.method === "POST" && pathname === "/api/chat") {
    await handleChat(req, res);
    return true;
  }
  return false;
}

function start() {
  ensureLogDir();

  // 嵌入式模式：父进程（ExBarBrowser.exe）通过环境变量告诉我们它是谁。
  // 父进程被强杀时我们应当自己退出 —— Job Object 已经能兜住这一点，
  // 但那是 Windows 侧机制；这里再加一条，覆盖"进程 kill 但 job 未关"的边角，
  // 也让 `npm start` 手动运行时不受影响（没有这个变量就不启用）。
  const parentPid = Number(process.env.EXBAR_PARENT_PID || 0);
  if (parentPid > 0) {
    const timer = setInterval(() => {
      try {
        // process.kill(pid, 0) 只探测存活，不发信号。
        process.kill(parentPid, 0);
      } catch (e) {
        console.log(`[embed] parent ${parentPid} is gone, shutting down`);
        clearInterval(timer);
        process.exit(0);
      }
    }, 2000);
    timer.unref();   // 不阻止进程自然退出
    console.log(`[embed] parent pid = ${parentPid}`);
  }

  const server = http.createServer(async (req, res) => {
    // Pick up aichat.json edits on every request. reloadConfig() only re-reads
    // the file when its mtime/size actually changed, so this is a cheap stat,
    // not a JSON parse per request. A long-lived SSE chat reloads once at the
    // start of the turn, so the model cannot swap halfway through a reply.
    if (reloadConfig()) {
      log(`[config] reloaded -> model=${config.model} @ ${config.baseUrl}`);
    }
    const url = new URL(req.url, `http://${config.host}:${config.port}`);

    // 本地 HTTP 的安全边界：Host / Origin 防 DNS rebinding 与跨站页面，
    // token 防本机任意页面直接驱动 /api/*。静态页面和 health 保持免 token，
    // 方便诊断，但它们什么命令都不能执行。
    if (!hostAllowed(req)) return json(res, 403, { error: "host not allowed" });
    if (!originAllowed(req)) return json(res, 403, { error: "origin not allowed" });
    const healthPath = url.pathname === "/api/health" || url.pathname === "/health";
    const needsToken = url.pathname.startsWith("/api/") && !healthPath
      || url.pathname === "/agent";
    if (needsToken && !tokenAllowed(req, url)) {
      return json(res, 401, { error: "本地令牌缺失或已过期，请从 AI 面板打开页面" });
    }

    try {
      // 优雅退出端点：宿主（ExBarBrowser.exe）关闭时先发带 token 的 POST，
      // 让本服务立刻关监听、释放端口、退出，而不是等"父进程 PID 消失"的 2s 轮询
      // —— 父进程那一刻正卡在 WaitForSingleObject 里等本进程，两边互等会白耗 2.5s。
      // server.close 会等现有 SSE 长连接断开，故加一个短兜底强制退出。
      if (req.method === "POST" && url.pathname === "/api/shutdown") {
        json(res, 200, { ok: true });
        let done = false;
        const finish = () => { if (!done) { done = true; process.exit(0); } };
        try { server.close(finish); } catch (e) { finish(); }
        setTimeout(finish, 300).unref();
        return;
      }

      // Classic text protocol.
      if (req.method === "POST" && url.pathname === "/agent") {
        const r = await handleAgent(req);
        res.writeHead(r.status, { "Content-Type": "text/plain; charset=utf-8" });
        res.end(r.body);
        return;
      }

      // JSON / SSE API.
      if (await handleApi(req, res, url)) return;
      if (url.pathname.startsWith("/api/")) {
        return json(res, 404, { error: "not found" });
      }

      // Static UI.
      if (req.method === "GET") return serveStatic(url.pathname, res);

      res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
      res.end("not found");
    } catch (e) {
      log("request error: " + (e && e.message ? e.message : e));
      if (e && e.status) {
        if (!res.headersSent) json(res, e.status, { error: e.message });
        return;
      }
      if (!res.headersSent) {
        res.writeHead(200, { "Content-Type": "text/plain; charset=utf-8" });
      }
      try { res.end(packError("server error: " + String(e && e.message ? e.message : e))); } catch (e2) { }
    }
  });

  server.on("error", (e) => {
    if (e.code === "EADDRINUSE") {
      console.error(`[fatal] port ${config.port} is already in use.`);
      console.error(`        stop the other process, or change "port" in ${CONFIG_PATH}`);
    } else {
      console.error(`[fatal] ${e.message}`);
    }
    process.exit(1);
  });

  server.listen(config.port, config.host, async () => {
    const ready = modelReadiness();
    console.log("");
    console.log("  AI 助手服务  v2.1");
    console.log("  ---------------------------------------------");
    console.log(`  listening   http://${config.host}:${config.port}`);
    console.log(`  web ui      http://${config.host}:${config.port}/`);
    console.log(`  model       ${config.model} @ ${config.baseUrl}`);
    console.log(`  api key     ${config.apiKey ? "set" : "NOT SET"}`);
    // 【打印真实生效的安全边界，不打印已废弃的 Opus 桥路径】
    // 以前这里打 `bridge ${config.bridgeDir}` —— 迁移到命名管道后那个路径
    // 不再被任何代码读取，留着会把排查方向引到"桥目录配错了"上。
    // 现在打的是 guard.js 真正会执行的三个开关：模型侧双门的第一道 + 面板侧两条。
    console.log(`  aiAllowDanger ${config.dryRun ? "n/a (dry run)" : "宿主侧开关（改设置页 aiAllowDanger）"}`);
    console.log(`  dry run     ${config.dryRun ? "ON (commands are described, not run)" : "off"}`);
    console.log(`  denyCommands ${config.denyCommands.length ? config.denyCommands.join(", ") : "(空)"}`);
    console.log(`  allowedRoots ${config.allowedRoots.length ? config.allowedRoots.join(" ; ") : "(空 = 不限制路径)"}`);
    console.log(`  config      ${CONFIG_PATH}`);
    console.log(`  models      ${MODEL_PATH}  (${savedProviders().length} provider)`);
    if (!ready.ok) console.log(`  [warn] ${ready.reason}`);
    console.log("");
    console.log("  classic dialog:   POST /agent  (AGENT/1 text protocol)");
    console.log("");
  });
}

if (require.main === module) start();

module.exports = {
  start, parseRequest, packReply, packError, configPayload, contextPayload, turns: TURNS,
  // 纯函数导出，供本地安全边界自测；不会启动 HTTP 服务。
  security: { loopbackName, hostAllowed, originAllowed, tokenAllowed },
  readJson,
  historyForChat,
  branches: BRANCHES
};
