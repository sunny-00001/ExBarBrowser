// config.js - service configuration, split across two files with two lifetimes.
//
//   config.json   行为配置：端口、超时、安全边界、allowedRoots……低频变更、无密钥、
//                 可以放心进版本库。
//   model.json    模型目录：providers、apiKey、模型清单、默认模型。高频变更、含
//                 密钥、.gitignore 掉。
//
// model.json 的结构照抄其它 agent 工具的通行做法（不是自创）：
//   - Pi Agent      ~/.pi/agent/models.json  providers{} -> models[{id,name,…}]
//   - OpenClaw      models.json              providers{} -> models[{id,name}]
//   - OpenCode      opencode.json            provider{}  -> models{id:{name,limit}}
//   - Crush         crush.json               providers{} -> models[{id,cost,…}]
//   - Continue      config.yaml              models[]（扁平，每条自带 provider）
//   五个实现一致的三件事：
//     1. providers 是字典，键是稳定 id，不是数组下标；
//     2. models 是对象（能挂显示名、上下文长度、单价），不是纯字符串；
//     3. 当前模型是一个引用（"provider/model"），不在文件里复制一份 baseUrl+apiKey。
//
// 所以 v2 长这样：
//
//   { "version": 2,
//     "default": "agent2api/CodeArts:glm-5.3-flash",
//     "providers": {
//       "agent2api": { "name": "Agent2api",
//                      "baseUrl": "https://…/v1",
//                      "api": "openai-completions",
//                      "apiKey": "sk-…",
//                      "models": [ { "id": "CodeArts:glm-5.3-flash" },
//                                   { "id": "deepseek-v4.1-flash", "enabled": false } ] } } }
//
// 启用状态挂在条目上（`enabled: false`），没有第二份 disabled 数组；协议用
// `api` 枚举（其它工具的名字），旧的 `protocol: "openai"` 继续可读。
//
// 优先级：环境变量 > model.json > config.json > 内置默认值。环境变量只影响运行期，
// 不回写文件。
//
// 向后兼容：v1 文件（active 快照 + providers 数组 + disabled 数组）、以及老
// config.json 里的 baseUrl / apiKey / model / protocol / vendor / providers，都在
// 读取时归一化成 v2，在下次写盘时升级成 v2 结构，无需手工迁移。

const fs = require("fs");
const path = require("path");

// AGENTCHAT_CONFIG lets tests point the service at a throwaway file so the
// model-management endpoints never touch the user's real config.json.
// model.json always lives next to it, so a test can never write into service/.
const CONFIG_PATH = process.env.AGENTCHAT_CONFIG
  ? path.resolve(process.env.AGENTCHAT_CONFIG)
  : path.join(__dirname, "config.json");
const MODEL_PATH = process.env.AGENTCHAT_MODEL_CONFIG
  ? path.resolve(process.env.AGENTCHAT_MODEL_CONFIG)
  : path.join(path.dirname(CONFIG_PATH), "model.json");

// Bumped only when the on-disk shape changes in a way that needs code to migrate.
// 1 = {version, active:{…}, providers:[…]};  2 = {version, default, providers:{…}}
const MODEL_VERSION = 2;

// The five keys describing "which endpoint / model is live right now". They are
// no longer stored as a snapshot in v2 (the `default` reference replaces them),
// but they are still the flat view every caller works with.
const ACTIVE_KEYS = ["vendor", "baseUrl", "apiKey", "model", "protocol"];
// Everything that belongs to model.json -- never written to config.json.
const MODEL_KEYS = new Set([...ACTIVE_KEYS, "providers", "default"]);
// resolveActive() also returns `ref` (the resolved "key/id" pointer) and
// callers may carry `defaultRef`. They are in-memory bookkeeping, not config
// fields: they must be stripped on write but must NOT count as "legacy model
// keys" and trigger a config.json migration.
const INTERNAL_KEYS = new Set([...MODEL_KEYS, "ref", "defaultRef"]);

// Wire-protocol names: config files written by other agent tools use the long
// form, this service uses the short one internally. Anything unknown is treated
// as OpenAI-compatible, which is what protocolId("") already did.
const API_BY_PROTOCOL = {
  openai: "openai-completions",
  anthropic: "anthropic-messages"
};
const PROTOCOL_BY_API = {
  openai: "openai",
  "openai-completions": "openai",
  "openai-chat": "openai",
  "openai-responses": "openai",
  "openai-compatible": "openai",
  anthropic: "anthropic",
  "anthropic-messages": "anthropic"
};

const DEFAULTS = {
  // --- transport -----------------------------------------------------------
  host: "127.0.0.1",
  port: 8792,

  // --- model ---------------------------------------------------------------
  // Only fallbacks: the live triple normally resolves from model.json#default.
  // Works with OpenAI, DeepSeek, Moonshot, Zhipu, SiliconFlow, Ollama, vLLM,
  // LM Studio, one-api/new-api gateways -- anything that speaks
  // POST {baseUrl}/chat/completions with tools/function calling.
  baseUrl: "https://api.deepseek.com/v1",
  apiKey: "",
  model: "deepseek-chat",
  // Wire protocol for the active endpoint. Old config files have no such field
  // and therefore land on "openai" via DEFAULTS -- no migration needed.
  protocol: "openai",
  // Display name of the active service provider (free text, including custom
  // names typed into the dialog). Purely cosmetic; never used as an identity.
  vendor: "",
  temperature: 0.2,
  maxTokens: 2048,
  // Hard ceiling on agent steps per user turn. Each step is one model call plus
  // the tool calls it requests. Beyond this the turn is abandoned with a note
  // rather than silently looping.
  maxSteps: 12,

  // --- Opus bridge (reuses the existing dopus-mcp file IPC channel) --------
  bridgeDir: "D:\\dopus-mcp\\bridge",
  dopusRt: "D:\\dopus\\dopusrt.exe",
  opusTimeoutMs: 20000,

  // --- safety --------------------------------------------------------------
  // 命令名黑名单（点分名，如 fs.delete / fs.* 前缀族）。执行点在 guard.js，
  // 由 tools-exbar.dispatch 调用 —— 那里是两条 agent 循环调工具的唯一出口。
  //
  // 【默认必须是空的】此前这里是 ["Delete", "Shift", "ShiftDelete"] —— Opus 时代
  // 的动词。迁移到 ExBarBrowser 的点分命令后，这三个词**永远匹配不上任何命令**，
  // 是一道看起来存在、实际零拦截的假防线（比空着更糟：用户以为设了限制）。
  // 真正的第一道是宿主的危险双门（aiAllowDanger + approve），第二道是面板的
  // 审批卡（按命令表 annotations.destructiveHint 逐次弹）。黑名单只用来表达
  // "这个部署里某类命令一律不许"，所以默认不设 —— 需要时由用户显式加。
  denyCommands: [],
  // When true, destructive-looking commands are described back to the user
  // instead of executed, and the turn ends. Flip to false once you trust the
  // setup.
  dryRun: false,

  // In-memory view of model.json#providers; never read from config.json.
  providers: [],

  // 工具可触碰的绝对路径白名单（含子目录）。空数组 = 不限制。
  // 执行点同样是 guard.js：检查参数里所有"看起来像绝对路径"的值。
  // 空白名单 = 不限制是刻意的 —— 默认行为必须与"没有这个功能"完全一致。
  allowedRoots: [],

  // --- misc ----------------------------------------------------------------
  logDir: path.join(__dirname, "..", "logs"),
  verbose: false
};

// ---------------------------------------------------------------------------
// file io
// ---------------------------------------------------------------------------

function logLine(msg) {
  // console only: config.js loads before the file logger exists, and a bad file
  // must be visible on the console start.cmd keeps open.
  console.error(msg);
}

// Returns {} when the file is absent, null when it exists but is unreadable or
// not valid JSON. The two cases must be distinguishable: a broken file must NOT
// silently reset a working configuration back to DEFAULTS.
function readJsonAt(file, label) {
  try {
    if (!fs.existsSync(file)) return {};
    const obj = JSON.parse(fs.readFileSync(file, "utf8"));
    return obj && typeof obj === "object" && !Array.isArray(obj) ? obj : null;
  } catch (e) {
    logLine(`[config] ${label} ${file} is unreadable or not valid JSON: ${e.message}`);
    return null;
  }
}

// temp + rename, so a reader never observes a half-written JSON.
function writeJsonAtomic(file, obj) {
  const tmp = file + ".tmp";
  fs.writeFileSync(tmp, JSON.stringify(obj, null, 2) + "\n", "utf8");
  fs.renameSync(tmp, file);
}

function stampOf(file) {
  try {
    const st = fs.statSync(file);
    // mtime + size is not sufficient on Windows: a same-length edit (swapping
    // one model id for another of equal length) can land in the same cached
    // NTFS timestamp tick and be completely invisible to hot-reload. These are
    // tiny files, so an exact content fingerprint is cheap and removes the
    // whole class of "the config changed but the service didn't notice" bugs.
    const buf = fs.readFileSync(file);
    let h = 2166136261;                       // FNV-1a, no crypto dependency
    for (let i = 0; i < buf.length; i++) {
      h ^= buf[i];
      h = Math.imul(h, 16777619);
    }
    return `${st.mtimeMs}:${st.size}:${(h >>> 0).toString(16)}`;
  } catch (e) {
    return "missing";
  }
}

// ---------------------------------------------------------------------------
// model.json shape: v1 / v2 / flat hand-written -> one canonical view
// ---------------------------------------------------------------------------

function apiForProtocol(protocol) {
  const p = String(protocol || "").trim().toLowerCase();
  return API_BY_PROTOCOL[p] || (p ? p : API_BY_PROTOCOL.openai);
}

function protocolForApi(api, fallback) {
  const s = String(api || "").trim().toLowerCase();
  return PROTOCOL_BY_API[s] || (fallback || "openai");
}

// Provider keys double as JSON object keys, so they must be stable and unique.
// Derived once from the display name, then kept forever: renaming the provider
// or editing its baseUrl must not orphan the `default` reference.
function slugify(name, taken) {
  let base = String(name || "")
    .trim().toLowerCase()
    .replace(/^https?:\/\//, "")
    .replace(/[^a-z0-9\u4e00-\u9fa5]+/g, "-")
    .replace(/^-+|-+$/g, "");
  if (!base) base = "provider";
  let key = base;
  let n = 2;
  while (taken.has(key)) key = `${base}-${n++}`;
  taken.add(key);
  return key;
}

// A single model entry from any of the accepted shapes: "id" | {id, name, …}.
function modelEntry(item) {
  if (typeof item === "string") return { id: item.trim(), meta: {} };
  if (!item || typeof item !== "object") return { id: "", meta: {} };
  const id = String(item.id || item.model || item.name || "").trim();
  const meta = {};
  for (const k of Object.keys(item)) if (k !== "id") meta[k] = item[k];
  return { id, meta };
}

// Canonical provider entry. `models` is an id array (what server.js and the UI
// work with), `meta` keeps every extra field the user or another tool wrote
// (name / contextWindow / cost / …), `disabled` is the switched-off subset.
function canonicalProvider(key, raw, taken) {
  const src = raw && typeof raw === "object" ? raw : {};
  const name = String(
    src.name || src.vendor || key || ""
  ).trim() || key;
  // v2: models is an object keyed by id (OpenCode) or an array (Pi/Crush).
  let items = [];
  if (Array.isArray(src.models)) {
    items = src.models.map(modelEntry);
  } else if (src.models && typeof src.models === "object") {
    items = Object.keys(src.models).map((id) => {
      const v = src.models[id];
      return modelEntry({ id, ...(v && typeof v === "object" ? v : {}) });
    });
  }
  const models = [];
  const meta = {};
  const disabled = [];
  // A canonical entry passed straight back in (saveAll re-normalises its own
  // output) carries extras in `meta` instead of on the model objects, so start
  // from there and let explicit per-model fields win.
  const prevMeta = src.meta && typeof src.meta === "object" && !Array.isArray(src.meta)
    ? src.meta
    : {};
  for (const { id, meta: m } of items) {
    if (!id || models.includes(id)) continue;
    models.push(id);
    const inherited = prevMeta[id] && typeof prevMeta[id] === "object" ? prevMeta[id] : {};
    const extra = { ...inherited, ...m };
    // `enabled` is the only start/stop truth in v2. Anything else (v1's
    // separate `disabled` array, a stray `enabled: true`) is folded here so a
    // hand-edited file cannot end up with two contradicting switches.
    if (extra.enabled === false) disabled.push(id);
    delete extra.enabled;
    if (Object.keys(extra).length) meta[id] = extra;
  }
  // v1 carried the switched-off ids in a sibling array.
  for (const id of Array.isArray(src.disabled) ? src.disabled : []) {
    const s = String(id || "").trim();
    if (s && models.includes(s) && !disabled.includes(s)) disabled.push(s);
  }
  const protocol = protocolForApi(
    src.api || src.protocol,
    String(src.protocol || "").trim().toLowerCase() === "anthropic" ? "anthropic" : "openai"
  );
  return {
    key: key || slugify(name, taken),
    name,
    vendor: name,
    baseUrl: String(src.baseUrl || ""),
    apiKey: String(src.apiKey || ""),
    api: apiForProtocol(protocol),
    protocol,
    models,
    disabled,
    meta
  };
}

// Find the provider an (endpoint, model) pair belongs to. Precedence: exact
// protocol+id, then id at that endpoint, then any provider at that endpoint.
// The last step matters because a protocol may be omitted in a hand-written
// file while the provider block still names it.
function matchProvider(providers, baseUrl, id, protocol) {
  const b = String(baseUrl || "").replace(/\/+$/, "");
  const m = String(id || "").trim();
  if (!b) return null;
  const atBase = (providers || []).filter((p) => p.baseUrl.replace(/\/+$/, "") === b);
  if (!atBase.length) return null;
  return atBase.find((p) => p.protocol === protocol && m && p.models.includes(m)) ||
    atBase.find((p) => m && p.models.includes(m)) ||
    atBase.find((p) => p.protocol === protocol) ||
    atBase[0];
}

// Normalises every accepted on-disk shape into:
//   { version, defaultRef, active:{…}, providers:[canonicalProvider…] }
// v2 (providers is a dictionary keyed by stable id) is the canonical form; v1
// arrays keep their original key derived from the display name.
function normalizeModel(raw) {
  const src = raw && typeof raw === "object" ? raw : {};
  const nested = src.active && typeof src.active === "object" && !Array.isArray(src.active)
    ? src.active
    : {};
  const active = {};
  for (const k of ACTIVE_KEYS) {
    if (typeof nested[k] === "string") active[k] = nested[k];
    else if (typeof src[k] === "string") active[k] = src[k];
  }

  const providers = [];
  const taken = new Set();
  if (Array.isArray(src.providers)) {
    // v1 / flat hand-written form.
    for (const p of src.providers) {
      if (!p || typeof p !== "object") continue;
      const name = String(p.name || p.vendor || "").trim();
      const key = String(p.key || "").trim() || slugify(name, taken);
      taken.add(key);
      providers.push(canonicalProvider(key, p, taken));
    }
  } else if (src.providers && typeof src.providers === "object") {
    // v2 dictionary form.
    for (const key of Object.keys(src.providers)) {
      const p = src.providers[key];
      if (!p || typeof p !== "object") continue;
      taken.add(key);
      providers.push(canonicalProvider(key, { ...p, key }, taken));
    }
  }

  // `default` is "provider-key/model-id". v1 had no such field: derive it from
  // the active snapshot so the first save upgrades it instead of dropping it.
  // `defaultRef` is accepted too, because saveAll hands its own output back in
  // (normalisation has to be idempotent or the reference is lost on every write).
  let defaultRef = typeof src.default === "string" ? src.default.trim()
    : (typeof src.defaultRef === "string" ? src.defaultRef.trim() : "");
  if (!defaultRef) {
    const id = String(active.model || "").trim();
    const host = matchProvider(providers, active.baseUrl, id, protocolForApi(active.protocol, "openai"));
    if (host) {
      // Keep an id the catalogue does not list: that is the "live but
      // unregistered" state the dialog offers a `+` for. Replacing it with the
      // first listed model would silently switch the user's model.
      const model = id || host.models[0];
      if (model) defaultRef = `${host.key}/${model}`;
    }
  }

  return {
    version: MODEL_VERSION,
    defaultRef,
    active,
    providers
  };
}

function readModelFile() {
  const raw = readJsonAt(MODEL_PATH, "model config");
  return raw === null ? null : normalizeModel(raw);
}

// Canonical view -> on-disk v2 object. Only `enabled: false` is written; an
// absent flag means enabled, so files stay close to the Pi/Crush layout.
function modelToDisk(state) {
  const providers = {};
  for (const p of state.providers || []) {
    const models = (p.models || []).map((id) => {
      const entry = { id };
      const extra = p.meta && p.meta[id];
      if (extra && typeof extra === "object") {
        for (const k of Object.keys(extra)) if (k !== "id" && k !== "enabled") entry[k] = extra[k];
      }
      if ((p.disabled || []).includes(id)) entry.enabled = false;
      return entry;
    });
    const out = { name: p.name };
    if (p.baseUrl) out.baseUrl = p.baseUrl;
    out.api = p.api || apiForProtocol(p.protocol);
    if (p.apiKey) out.apiKey = p.apiKey;
    out.models = models;
    providers[p.key] = out;
  }
  const disk = { version: MODEL_VERSION };
  if (state.defaultRef) disk.default = state.defaultRef;
  disk.providers = providers;
  // Only present when the live triple cannot be expressed as a reference
  // (e.g. an endpoint that was never saved as a provider). Read back as a
  // fallback so nothing is lost.
  const snap = {};
  for (const k of ACTIVE_KEYS) {
    if (typeof state.active[k] === "string" && state.active[k] !== "") snap[k] = state.active[k];
  }
  if (Object.keys(snap).length) disk.active = snap;
  return disk;
}

// ---------------------------------------------------------------------------
// one-time migration out of config.json
// ---------------------------------------------------------------------------

// Move legacy model keys out of config.json. Runs at startup and again whenever
// config.json actually changes, so a hand edit / a restored .bak that still
// carries baseUrl+apiKey+models keeps working instead of becoming a dead copy.
// Idempotent: after the first pass the keys are gone and this is one scan over
// six property names.
//
// Precedence when both files carry model data: model.json wins, because it is
// the file the dialog writes and therefore the newest truth; config.json's copy
// is a leftover from the single-file era and only fills fields model.json does
// not have (a truly fresh install, or a hand-authored behaviour file). Getting
// this backwards would silently roll the user back to whatever model happened
// to be in config.json when the split happened.
// config.json is rewritten *without* those keys and the previous version is
// kept as config.json.bak-<timestamp>.
// Returns true when it wrote something.
function migrateLegacyModelKeys() {
  const cfgRaw = readJsonAt(CONFIG_PATH, "config");
  if (cfgRaw === null) return false;                 // broken file: leave it alone
  const legacy = Object.keys(cfgRaw).filter((k) => MODEL_KEYS.has(k));
  if (!legacy.length) return false;                  // already migrated

  const existing = readModelFile() || normalizeModel({});
  const target = {
    version: MODEL_VERSION,
    defaultRef: existing.defaultRef,
    active: { ...existing.active },
    providers: existing.providers.slice()
  };

  // Which file is authoritative turns on whether model.json actually has a
  // catalogue: if it does, it is the file the dialog writes and therefore the
  // newest truth (config.json's copy is a leftover and must not roll the user
  // back to an older model); if it does not, config.json is the only source
  // left and wins outright.
  const modelHasCatalogue = existing.providers.length > 0;
  for (const k of ACTIVE_KEYS) {
    if (typeof cfgRaw[k] !== "string" || cfgRaw[k] === "") continue;
    if (!modelHasCatalogue || !target.active[k]) target.active[k] = cfgRaw[k];
  }
  if (!target.providers.length && Array.isArray(cfgRaw.providers) && cfgRaw.providers.length) {
    target.providers = normalizeModel({ providers: cfgRaw.providers }).providers;
  }
  // Re-derive the reference: keep model.json's own pointer if it had one (v2),
  // otherwise fall back to the active triple (v1 / config.json keys), otherwise
  // whatever the catalogue can offer.
  const rebased = normalizeModel({
    providers: target.providers,
    active: target.active,
    defaultRef: target.defaultRef
  });
  target.defaultRef = rebased.defaultRef;
  target.providers = rebased.providers;
  target.active = target.defaultRef ? {} : rebased.active;

  const nextCfg = {};
  for (const k of Object.keys(cfgRaw)) if (!MODEL_KEYS.has(k)) nextCfg[k] = cfgRaw[k];

  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  try {
    fs.copyFileSync(CONFIG_PATH, `${CONFIG_PATH}.bak-${stamp}`);
  } catch (e) {
    logLine(`[config] could not back up config.json: ${e.message}`);
  }
  writeJsonAtomic(MODEL_PATH, modelToDisk(target));
  writeJsonAtomic(CONFIG_PATH, nextCfg);

  logLine(`[config] moved ${legacy.join(", ")} from config.json to model.json` +
    `; backup: ${path.basename(CONFIG_PATH)}.bak-${stamp}`);
  return true;
}

migrateLegacyModelKeys();

// A v1 file keeps working forever, but the point of this change is the file on
// disk, not just the in-memory view -- so the first start after the upgrade
// rewrites it in the v2 layout. The previous version is kept beside it
// (model.json.bak-<timestamp>), so downgrading is a copy away. Writes only when
// the normalised form actually differs, so a hand-formatted v2 file is left
// exactly as the user typed it.
function upgradeModelFile() {
  const raw = readJsonAt(MODEL_PATH, "model config");
  if (raw === null) return false;                    // broken: leave it alone
  const state = normalizeModel(raw);
  if (!state.providers.length && !state.defaultRef && !Object.keys(state.active).length) return false;
  const next = modelToDisk({ ...state, active: state.defaultRef ? {} : state.active });
  if (JSON.stringify(next) === JSON.stringify(raw)) return false;

  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  try {
    fs.copyFileSync(MODEL_PATH, `${MODEL_PATH}.bak-${stamp}`);
  } catch (e) {
    logLine(`[config] could not back up model.json: ${e.message}`);
  }
  writeJsonAtomic(MODEL_PATH, next);
  logLine(`[config] model.json upgraded to the v2 layout (${Object.keys(next.providers).length} provider(s), ` +
    `default=${next.default || "none"}); backup: model.json.bak-${stamp}`);
  return true;
}

upgradeModelFile();

// ---------------------------------------------------------------------------
// effective configuration
// ---------------------------------------------------------------------------

function envOverride(cfg) {
  const E = process.env;
  const out = { ...cfg };
  if (E.AGENTCHAT_PORT) out.port = Number(E.AGENTCHAT_PORT);
  if (E.AGENTCHAT_BASE_URL) out.baseUrl = E.AGENTCHAT_BASE_URL;
  if (E.AGENTCHAT_API_KEY) out.apiKey = E.AGENTCHAT_API_KEY;
  if (E.AGENTCHAT_MODEL) out.model = E.AGENTCHAT_MODEL;
  if (E.AGENTCHAT_BRIDGE_DIR) out.bridgeDir = E.AGENTCHAT_BRIDGE_DIR;
  if (E.AGENTCHAT_DOPUS_RT) out.dopusRt = E.AGENTCHAT_DOPUS_RT;
  if (E.AGENTCHAT_DRY_RUN) out.dryRun = E.AGENTCHAT_DRY_RUN === "1";
  if (E.AGENTCHAT_VERBOSE) out.verbose = E.AGENTCHAT_VERBOSE === "1";
  // Semicolon-separated, because a comma is legal inside a Windows path and a
  // colon is already taken by the drive letter. Set to an empty string to mean
  // "no restriction", matching an empty array in the config file.
  if (E.AGENTCHAT_ALLOWED_ROOTS !== undefined) {
    out.allowedRoots = E.AGENTCHAT_ALLOWED_ROOTS
      .split(";").map((s) => s.trim()).filter((s) => s !== "");
  }
  return out;
}

// Resolve `default` ("provider-key/model-id") into the flat live triple. Falls
// back to the legacy `active` snapshot -- completed from its provider block, so
// a hand-written file that omits `protocol` still works -- then to the first
// enabled model of the first provider, so deleting a provider cannot leave the
// service pointing at an endpoint that no longer exists.
function resolveActive(state) {
  const providers = state.providers || [];
  const ref = String(state.defaultRef || "").trim();
  if (ref) {
    const cut = ref.indexOf("/");
    const key = cut > 0 ? ref.slice(0, cut) : "";
    const id = cut > 0 ? ref.slice(cut + 1) : "";
    let p = key ? providers.find((x) => x.key === key) : null;
    if (!p && id) p = providers.find((x) => x.models.includes(id));
    if (p) {
      // An id that is not in the catalogue is kept as-is: it is the "unregistered
      // but live" case the dialog shows a `+` for.
      const model = id || (p.models.filter((x) => !p.disabled.includes(x))[0] || p.models[0]);
      if (model) {
        return {
          baseUrl: p.baseUrl,
          apiKey: p.apiKey,
          model,
          protocol: p.protocol,
          vendor: p.name,
          ref: `${p.key}/${model}`
        };
      }
    }
  }

  const snap = {};
  for (const k of ACTIVE_KEYS) {
    if (typeof state.active[k] === "string" && state.active[k] !== "") snap[k] = state.active[k];
  }
  if (snap.baseUrl) {
    const p = matchProvider(providers, snap.baseUrl, snap.model, protocolForApi(snap.protocol, "openai"));
    if (p) {
      const model = snap.model || p.models.filter((x) => !p.disabled.includes(x))[0] || p.models[0];
      if (model) {
        return {
          baseUrl: p.baseUrl,
          apiKey: p.apiKey,
          model,
          protocol: p.protocol,
          vendor: p.name,
          ref: `${p.key}/${model}`
        };
      }
    }
    // Not matched: the endpoint was removed from the catalogue. Fall through to
    // another provider rather than resurrecting a deleted one.
  }

  for (const p of providers) {
    const on = p.models.filter((id) => !p.disabled.includes(id));
    const model = on[0] || p.models[0];
    if (!p.baseUrl || !model) continue;
    return {
      baseUrl: p.baseUrl,
      apiKey: p.apiKey,
      model,
      protocol: p.protocol,
      vendor: p.name,
      ref: `${p.key}/${model}`
    };
  }

  // Nothing in the catalogue at all: keep whatever the snapshot says, so a
  // brand-new install still reports the DEFAULTS triple.
  return snap;
}

// config.json supplies behaviour fields; model.json supplies the live triple and
// the provider catalogue. model.json wins for its own keys, but a config.json
// that still carries them (pre-migration file, restored .bak, hand edit) is read
// as a fallback rather than ignored -- migration is a file-cleanup step, not the
// thing that makes the values work.
function buildEffective(cfgFile, modelFile) {
  const base = { ...DEFAULTS, ...(cfgFile || {}) };
  const state = normalizeModel(modelFile || {});
  const cfgProviders = Array.isArray((cfgFile || {}).providers) ? (cfgFile || {}).providers : [];
  const providers = state.providers.length ? state.providers : normalizeModel({ providers: cfgProviders }).providers;
  const merged = { ...base, providers };
  const live = resolveActive({ ...state, providers });
  for (const k of ACTIVE_KEYS) {
    const fromModel = typeof live[k] === "string" ? live[k] : "";
    const fromCfg = typeof (cfgFile || {})[k] === "string" ? (cfgFile || {})[k] : "";
    const value = fromModel || fromCfg;
    if (value) merged[k] = value;
  }
  if (live.ref) merged.defaultRef = live.ref;
  // Top up the few model keys that have a meaningful default, so a brand-new
  // install with no model.json still reports "deepseek-chat" like before.
  for (const k of ACTIVE_KEYS) {
    if (typeof merged[k] !== "string" || merged[k] === "") merged[k] = DEFAULTS[k] || "";
  }
  return envOverride(merged);
}

// The initial read is deliberately kept as a single snapshot object that is
// NEVER replaced. Every consumer does `const { config } = require("./config")`
// and reads properties at call time, so mutating this object in place makes a
// file edit visible to the whole service without a restart.
let lastCfgStamp = stampOf(CONFIG_PATH);
let lastModelStamp = stampOf(MODEL_PATH);
let configError = "";

const config = buildEffective(readJsonAt(CONFIG_PATH, "config"), readModelFile());

// Replace the values of the shared object in place, so every holder of a
// reference sees the change immediately.
function applyFresh(cfgFile, modelFile) {
  const fresh = buildEffective(cfgFile, modelFile);
  for (const key of Object.keys(config)) {
    if (!(key in fresh)) delete config[key];
  }
  Object.assign(config, fresh);
  configError = "";
}

// Mask a secret for display in the UI, e.g. "sk-a…wxyz". Nothing in an HTTP
// response ever contains the full key.
function keyHint(k) {
  const s = String(k || "");
  if (!s) return "";
  if (s.length <= 9) return s.slice(0, 2) + "…";
  return s.slice(0, 5) + "…" + s.slice(-4);
}

// Re-read both files when either content fingerprint changed. The files are
// tiny, so hashing costs far less than the HTTP request that triggers the
// check; parsing only happens on a real edit. A half-written or invalid file is
// ignored, so the last good configuration stays live.
function reloadConfig() {
  let cfgStamp = stampOf(CONFIG_PATH);
  let modelStamp = stampOf(MODEL_PATH);
  if (cfgStamp === lastCfgStamp && modelStamp === lastModelStamp) return false;

  // A config.json edit may still carry model keys (old backup, hand edit).
  // Migrating here keeps that path working instead of silently ignoring it.
  if (cfgStamp !== lastCfgStamp && migrateLegacyModelKeys()) {
    cfgStamp = stampOf(CONFIG_PATH);
    modelStamp = stampOf(MODEL_PATH);
  }

  const cfgFile = readJsonAt(CONFIG_PATH, "config");
  const modelFile = readModelFile();
  if (cfgFile === null) {
    configError = "config.json is not a valid JSON object; keeping previous values";
    lastCfgStamp = cfgStamp;              // do not retry until it changes again
    lastModelStamp = modelStamp;
    return false;
  }
  if (modelFile === null) {
    configError = "model.json is not a valid JSON object; keeping previous values";
    lastCfgStamp = cfgStamp;
    lastModelStamp = modelStamp;
    return false;
  }
  lastCfgStamp = cfgStamp;
  lastModelStamp = modelStamp;
  applyFresh(cfgFile, modelFile);
  return true;
}

// Behaviour fields only (kept for callers that predate model.json). Model edits
// must go through saveAll, otherwise the two files drift apart.
function saveConfig(mutate) {
  const raw = readJsonAt(CONFIG_PATH, "config") || {};
  const next = mutate(raw) || raw;
  const nextCfg = {};
  for (const k of Object.keys(next)) if (!INTERNAL_KEYS.has(k)) nextCfg[k] = next[k];
  writeJsonAtomic(CONFIG_PATH, nextCfg);
  const modelFile = readModelFile() || normalizeModel({});
  lastCfgStamp = stampOf(CONFIG_PATH);
  lastModelStamp = stampOf(MODEL_PATH);
  applyFresh(nextCfg, modelFile);
  return nextCfg;
}

// Single write path for anything that touches the live triple or the provider
// catalogue. The caller mutates one flat object (behaviour fields + active keys
// + providers[]); this function splits it back into the two files, so callers
// never need to know which file a key lives in -- or that model.json is keyed.
function saveAll(mutate) {
  const cfgRaw = readJsonAt(CONFIG_PATH, "config") || {};
  const state = readModelFile() || normalizeModel({});
  const live = resolveActive(state);
  const { ref: _liveRef, ...liveFlat } = live;
  // Hand the caller its OWN copy. server.js mutates `raw.providers` in place
  // (list[idx] = entry); sharing the array would overwrite the very old state
  // we consult below for stable keys and per-model metadata.
  const editable = state.providers.map((p) => ({
    ...p,
    models: p.models.slice(),
    disabled: p.disabled.slice(),
    meta: { ...(p.meta || {}) }
  }));
  const flat = { ...cfgRaw, ...liveFlat, providers: editable };
  const next = mutate(flat) || flat;

  const nextCfg = {};
  for (const k of Object.keys(next)) if (!INTERNAL_KEYS.has(k)) nextCfg[k] = next[k];

  // Providers arrive as an array (that is what server.js iterates and what the
  // UI addresses); rebuild the canonical list, keeping each existing key so a
  // rename or a URL edit does not break `default`.
  //
  // Identity is NEVER the display name. Two relays may legitimately be called
  // the same thing while pointing at different endpoints, and a rename must
  // not be able to hijack some other provider's key. The old by-name fallback
  // did exactly that: saving a same-named endpoint reused its key, and
  // modelToDisk() then overwrote the original provider in the dictionary.
  const incoming = Array.isArray(next.providers) ? next.providers : [];
  const bySig = new Map();
  const byKey = new Map();
  for (const p of state.providers) {
    bySig.set(`${p.baseUrl.replace(/\/+$/, "")}|${p.protocol}`, p.key);
    byKey.set(p.key, p);
  }
  const taken = new Set();
  const providers = incoming
    .filter((p) => p && typeof p === "object")
    .map((p) => {
      const base = String(p.baseUrl || "").replace(/\/+$/, "");
      const name = String(p.name || p.vendor || "").trim() || base;
      // Explicit key (the dialog's editKey) wins; the endpoint signature is
      // only the server-side dedupe guard for callers without a key. Anything
      // else gets a fresh slug, so two same-named providers coexist as
      // separate dictionary keys instead of silently merging into one.
      let key = String(p.key || "").trim();
      if (!key) {
        const proto = protocolForApi(p.api || p.protocol, "openai");
        key = bySig.get(`${base}|${proto}`) || "";
      }
      if (!key || taken.has(key)) key = slugify(name, taken);
      taken.add(key);
      // server.js rebuilds a provider as a plain object when the dialog saves
      // an endpoint ({vendor,name,baseUrl,apiKey,protocol,models:string[]}).
      // Without this merge, every extra field on a model entry (display name,
      // contextWindow, cost -- anything another agent tool wrote) would be
      // silently dropped on the very next save. Re-attach the previous
      // per-model metadata by provider key; explicit values from the caller win.
      // `disabled` is deliberately NOT merged: the caller maintains that set
      // itself (re-adding an id must switch it back on).
      const prior = byKey.get(key);
      const meta = { ...((prior && prior.meta) || {}), ...(p.meta || {}) };
      // Drop the long form so canonicalProvider re-derives it from `protocol`;
      // otherwise an edited protocol would be overridden by the stale `api`.
      const { api, ...rest } = p;
      return canonicalProvider(key, { ...rest, name, meta }, taken);
    });

  const active = {};
  for (const k of ACTIVE_KEYS) {
    if (typeof next[k] === "string") active[k] = next[k];
  }
  // Represent the live model as a reference whenever it resolves inside the
  // catalogue (the normal case: saveConfigNormalized registers it). If it does
  // not -- the endpoint was just deleted, or was never saved -- fall back to
  // the first usable model instead of writing a snapshot that points at a
  // provider no longer in the file. The snapshot is kept only when there is no
  // catalogue at all to point into.
  const probe = normalizeModel({ providers, active });
  let snapshot = active;
  if (!probe.defaultRef) {
    const live = resolveActive({ ...probe, active });
    if (live.ref) {
      probe.defaultRef = live.ref;
      snapshot = {};
    }
  } else {
    snapshot = {};
  }
  const nextModel = modelToDisk({ ...probe, active: snapshot });
  // Adding/removing a model must not rewrite the behaviour file: that was the
  // whole point of splitting the two. Write config.json only when its content
  // really changed (a behaviour edit, or stripping stale model keys / an old
  // leaked `ref` off a legacy file).
  if (JSON.stringify(nextCfg) !== JSON.stringify(cfgRaw)) {
    writeJsonAtomic(CONFIG_PATH, nextCfg);
  }
  writeJsonAtomic(MODEL_PATH, nextModel);
  lastCfgStamp = stampOf(CONFIG_PATH);
  lastModelStamp = stampOf(MODEL_PATH);
  applyFresh(nextCfg, normalizeModel(nextModel));
  return next;
}

// Read the config file as-is (no defaults, no env) so a save keeps every field
// the user has, including ones this version of the service does not know about.
function readRawConfig() {
  return readJsonAt(CONFIG_PATH, "config") || {};
}

function configStatus() {
  return {
    path: CONFIG_PATH,
    modelPath: MODEL_PATH,
    stamp: `${lastCfgStamp}|${lastModelStamp}`,
    error: configError
  };
}

// Single place that answers "can this service talk to a model right now", so the
// HTTP layer and the CLI both report the same reason.
function modelReadiness() {
  if (!config.apiKey) {
    return {
      ok: false,
      reason:
        "no API key configured. Save an endpoint in the model dialog, or set " +
        "AGENTCHAT_API_KEY before starting the service. Any OpenAI-compatible " +
        "endpoint works (baseUrl + model)."
    };
  }
  if (!config.baseUrl || !config.model) {
    return { ok: false, reason: "baseUrl and model must both be set (model.json: providers + default)" };
  }
  return { ok: true, reason: "" };
}

module.exports = {
  config, CONFIG_PATH, MODEL_PATH, DEFAULTS, MODEL_VERSION, modelReadiness,
  reloadConfig, configStatus, saveConfig, saveAll, readRawConfig, keyHint,
  // exported for tests: the file-shape translation lives here and nowhere else
  normalizeModel, modelToDisk, apiForProtocol, protocolForApi
};
