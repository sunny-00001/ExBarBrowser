/* AI 助手 - web UI controller.
 *
 * The window is a WebView2 host with no browser chrome. Buttons in the custom
 * titlebar talk back to the host over `chrome.webview.postMessage`; everything
 * else is plain fetch + SSE against the local service.
 */

const $ = (id) => document.getElementById(id);

// 服务端权威回合号。旧事件可能只有 turn，新事件同时带 turnId/turn；两处视图
// 必须认同一个数字，否则活动流能定位到第 N 轮、回答气泡却没有同一个身份。
function turnIdOf(ev) {
  if (!ev) return 0;
  const primary = Number(ev.turnId);
  if (Number.isFinite(primary) && primary > 0) return primary;
  const legacy = Number(ev.turn);
  if (Number.isFinite(legacy) && legacy > 0) return legacy;
  return 0;
}

// 原生侧每次启动 node 都会生成新的随机 token 并拼到 WebView 的 URL 上。
// 所有 /api/* 请求都带它；放在 header 而不是 body，避免和聊天文本混在一起。
const EXBAR_HTTP_TOKEN = new URLSearchParams(location.search).get("exbarHttpToken") || "";

function apiFetch(url, opts = {}) {
  const headers = new Headers(opts.headers || {});
  if (EXBAR_HTTP_TOKEN) headers.set("X-ExBar-Token", EXBAR_HTTP_TOKEN);
  return fetch(url, { ...opts, headers });
}

// 宿主 -> 页面主题切换。不重载页面，聊天上下文、滚动位置和未提交的草稿都保留。
// 首帧主题由 index.html 从 URL 读，运行时改主题走这条 WebMessage。
if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener("message", (ev) => {
    const d = ev && ev.data;
    if (!d || d.type !== "exbar.theme") return;
    if (d.theme === "dark" || d.theme === "light") {
      document.documentElement.setAttribute("data-theme", d.theme);
    }
  });
}

// The dropdown keeps a short, local "recently used" list. Identity is the
// (protocol, baseUrl, id) triple: the same model id on two relays is two
// different choices. No endpoint URL or key is ever stored in plain sight.
const RECENT_MODELS_KEY = "ac.recentModels";
const RECENT_MODELS_LIMIT = 6;
// Pinned choices double as Alt+1..Alt+9 shortcuts, so the cap is nine. Both
// lists live only in localStorage: no endpoint URL or key reaches disk here
// and a stale entry simply stops resolving against /api/config's modelList.
const FAVORITE_MODELS_KEY = "ac.favoriteModels";
const FAVORITE_MODELS_LIMIT = 9;

function modelKey(m) {
  return [
    String((m && m.protocol) || "openai"),
    normBase((m && m.baseUrl) || ""),
    String((m && m.id) || "")
  ].join("\u0000");
}

function loadRecentModels() {
  let raw;
  try { raw = JSON.parse(localStorage.getItem(RECENT_MODELS_KEY) || "[]"); }
  catch (e) { return []; }
  if (!Array.isArray(raw)) return [];
  const seen = new Set();
  return raw
    .map((x) => x && ({
      id: String(x.id || "").trim(),
      baseUrl: String(x.baseUrl || ""),
      protocol: String(x.protocol || "openai")
    }))
    .filter((x) => {
      if (!x.id) return false;
      const k = modelKey(x);
      if (seen.has(k)) return false;
      seen.add(k);
      return true;
    })
    .slice(0, RECENT_MODELS_LIMIT);
}

function loadFavoriteModels() {
  let raw;
  try { raw = JSON.parse(localStorage.getItem(FAVORITE_MODELS_KEY) || "[]"); }
  catch (e) { return []; }
  if (!Array.isArray(raw)) return [];
  const seen = new Set();
  return raw
    .map((x) => x && ({
      id: String(x.id || "").trim(),
      baseUrl: String(x.baseUrl || ""),
      protocol: String(x.protocol || "openai")
    }))
    .filter((x) => {
      if (!x.id) return false;
      const k = modelKey(x);
      if (seen.has(k)) return false;
      seen.add(k);
      return true;
    })
    .slice(0, FAVORITE_MODELS_LIMIT);
}

const state = {
  sessionId: localStorage.getItem("ac.sessionId") || ("s" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8)),
  autoApprove: localStorage.getItem("ac.autoApprove") === "1",
  pinned: localStorage.getItem("ac.pinned") === "1",
  busy: false,
  abort: null,
  // 回合身份：每次 send/stop 都会推进 generation。SSE 事件带着发送时的
  // generation，迟到事件一旦发现 owner 已换人就直接丢弃。
  generation: 0,
  // 当前回合的服务端权威 turnId；start 事件到达时写入，用于二次校验。
  activeTurn: 0,
  // 用户点了"编辑重试"后暂存的旧回合；下一次 send 作为 retryOf 发出。
  pendingEditOf: 0,
  // Composer controls. `mode` is a session-level permission gate the service
  // enforces; it is not written into aichat.json. Higher data-prio chips are
  // hidden first when the docked window gets narrow.
  mode: ["confirm", "auto", "readonly"].includes(localStorage.getItem("ac.mode"))
    ? localStorage.getItem("ac.mode") : "confirm",
  attachments: [],
  contextItems: [],
  contextFolder: "",
  // Cached context anchor. Sending uses these directly instead of making a
  // blocking /api/context round-trip right before the first token.
  contextListerIndex: -1,
  // Last SSE id seen, retained for a future reconnect/resume path.
  lastEventId: 0,
  usage: { total: 0 },
  queue: [],
  prompts: [],
  promptIndex: -1,
  menuMode: null,
  menuAnchor: null,
  mentionQuery: "",
  menuCursor: -1,
  config: null,
  health: null,
  nearBottom: true,
  toolCards: new Map(),
  approvals: new Map(),
  // 最近一次可撤销的工具（给状态栏 peek 的“撤销”按钮用）。只在成功且
  // undoGroup=join 时写入；它不代表撤销栈一定非空，宿主侧仍会校验边界。
  lastUndo: null,
  // 结构化活动流（ROADMAP P4-a）：与 toolCards 同源事件，但按回合分组、可筛选。
  // turns: [{ turn, ts, prompt, tools:[...] }]，按 turn 升序。
  activity: {
    view: localStorage.getItem("ac.view") === "activity" ? "activity" : "chat",
    filter: "all",
    turns: [],
    // 当前回合号，由 SSE tool 事件带下来的 turn 覆盖；历史恢复时按条目重建。
    currentTurn: 0
  },
  // Model picker / custom-model dialog state. `providers` comes from
  // /api/config and contains MASKED keys only. `validation` gates the save
  // button; `selectedProvider` remembers which provider the form is editing
  // so the real key can be reused server-side (useSavedKey) and never needs
  // to travel back to the WebView.
  models: {
    list: [],
    providers: [],
    vendors: [],
    protocols: [],
    found: [],
    foundTotal: 0,
    foundSelected: new Set(),
    foundFilter: "",
    validation: { status: "idle", signature: "", detail: "" },
    validating: false,
    selectedProvider: null,
    // The stable models.json key of the provider being edited. It is the ONLY
    // identity we send to the server: unlike the display name or the endpoint,
    // it pins exactly one entry even when the URL is changed. null = "add".
    editKey: null,
    // "edit" = a saved provider was picked explicitly; "add" = a brand-new
    // one. Switching to "add" is always an explicit action (the 新增服务商
    // button or the 自定义 / 手动输入 row), never a guess made at save time.
    mode: "add",
    collapsed: new Set(),
    validateTimer: null,
    testResults: new Map(),
    // Guards the dropdown while /api/config/model is in flight: a double click
    // on a slow gateway would otherwise send two competing switch requests.
    switching: false,
    // P1 dropdown polish: a local recent group, per-session collapsed provider
    // groups, and an inline error that survives a failed switch so the user can
    // retry without losing their filtered list.
    recent: loadRecentModels(),
    menuCollapsed: new Set(),
    menuExpanded: new Set(),
    switchError: "",
    // P2: pinned models (Alt+1..9) and a "favorites only" search filter.
    favorites: loadFavoriteModels(),
    favOnly: false,
    // Remember which row the keyboard cursor is on. Every config refresh and
    // every star toggle re-renders the list; without this, ↑/↓ would silently
    // jump back to the first row mid-navigation.
    menuCursorKey: "",
    // False when the running service is an older build that does not expose the
    // vendor/protocol catalog in /api/config. The dialog uses it to warn that
    // validation and save endpoints are unavailable until the service restarts.
    serverHasCatalog: false
  }
};
localStorage.setItem("ac.sessionId", state.sessionId);
// Legacy flag: the service now reads `mode`, but keep this in sync so any
// older caller (and the SSE payload) still sees the same approval behaviour.
state.autoApprove = state.mode === "auto";

function saveRecentModels() {
  try { localStorage.setItem(RECENT_MODELS_KEY, JSON.stringify(state.models.recent)); }
  catch (e) { /* private mode / quota: the dropdown still works without history */ }
}

function rememberModel(m) {
  if (!m || !m.id) return;
  const entry = {
    id: String(m.id),
    baseUrl: String(m.baseUrl || ""),
    protocol: String(m.protocol || "openai")
  };
  const key = modelKey(entry);
  const next = [entry, ...state.models.recent.filter((x) => modelKey(x) !== key)]
    .slice(0, RECENT_MODELS_LIMIT);
  const unchanged = next.length === state.models.recent.length &&
    next.every((x, i) => modelKey(x) === modelKey(state.models.recent[i]));
  if (unchanged) return;
  state.models.recent = next;
  saveRecentModels();
}

function setModelSwitchError(message) {
  state.models.switchError = String(message || "");
  const el = $("modelMenuError");
  if (!el) return;
  el.textContent = state.models.switchError;
  el.hidden = !state.models.switchError;
  positionModelMenu();
}

// ---------------------------------------------------------------------------
// favorites: pinned model rows + Alt+1..9 quick switching
// ---------------------------------------------------------------------------

function saveFavoriteModels() {
  try { localStorage.setItem(FAVORITE_MODELS_KEY, JSON.stringify(state.models.favorites)); }
  catch (e) { /* private mode / quota: favorites are a convenience, not state */ }
}

function favoriteKeySet() {
  return new Set(state.models.favorites.map(modelKey));
}

// Resolve stored refs against the LIVE catalogue. A provider removed in the
// custom-model dialog leaves a favorite behind; hiding it is safer than
// pruning it, because re-adding the same endpoint restores the pin. The `i`
// carried on each row is the STORED slot (its Alt+1..9 number), not a position
// in this filtered array — quickSwitchFavorite matches on it to stay in sync
// with favoriteIndex().
function favoriteRows() {
  const byKey = new Map(state.models.list.map((m) => [modelKey(m), m]));
  return state.models.favorites
    .map((f, i) => ({ f, i, m: byKey.get(modelKey(f)) }))
    .filter((x) => !!x.m);
}

// Alt+1..9 follow the stored order, not the order of a filtered search result.
function favoriteIndex(m) {
  const i = state.models.favorites.findIndex((f) => modelKey(f) === modelKey(m));
  return i < 0 ? 0 : i + 1;
}

function toggleFavorite(m) {
  if (!m || !m.id || state.models.switching) return;
  const key = modelKey(m);
  const had = state.models.favorites.some((x) => modelKey(x) === key);
  if (had) {
    state.models.favorites = state.models.favorites.filter((x) => modelKey(x) !== key);
    saveFavoriteModels();
    toast("已取消收藏：" + m.id);
  } else {
    if (state.models.favorites.length >= FAVORITE_MODELS_LIMIT) {
      toast(`最多收藏 ${FAVORITE_MODELS_LIMIT} 个模型（对应 Alt+1~9）`);
      return;
    }
    state.models.favorites.push({
      id: String(m.id), baseUrl: String(m.baseUrl || ""),
      protocol: String(m.protocol || "openai")
    });
    saveFavoriteModels();
    toast(`已收藏 ${m.id} · Alt+${state.models.favorites.length} 快速切换`);
  }
  renderModelMenu();
}

function quickSwitchFavorite(index) {
  if (state.models.switching) return;
  const favorites = state.models.favorites;
  if (!favorites.length) {
    toast("还没有收藏模型：点模型行右侧的 ☆ 收藏，最多 9 个");
    return;
  }
  if (index < 0 || index >= favorites.length) {
    toast(`只有 ${favorites.length} 个收藏模型（Alt+1~${favorites.length}）`);
    return;
  }
  // Alt+N is bound to the N-th STORED pin — the number favoriteIndex() prints
  // on that row — not to a slot in the filtered live list. favoriteRows() drops
  // pins whose provider/model is gone, so rows[index] would silently shift
  // every hotkey after such a pin; match the stored index instead.
  const row = favoriteRows().find((x) => x.i === index);
  if (!row) {
    toast("该收藏模型已不在当前目录，请重新收藏或检查服务商配置");
    return;
  }
  const m = row.m;
  if (m.active) { toast("已经是当前模型：" + m.id); return; }
  closeModelMenu();
  closeModeMenu();
  closeComposerMenu();
  switchModel(m.id, m.baseUrl, m.protocol);
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

function escapeHtml(s) {
  return String(s == null ? "" : s)
    .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
}

function icon(name, cls) {
  return `<svg${cls ? ` class="${cls}"` : ""}><use href="#i-${name}"/></svg>`;
}

function timeText(ts) {
  const d = ts ? new Date(ts) : new Date();
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
}

function toast(msg) {
  const t = $("toast");
  t.textContent = msg;
  t.hidden = false;
  requestAnimationFrame(() => t.classList.add("show"));
  clearTimeout(toast._timer);
  toast._timer = setTimeout(() => {
    t.classList.remove("show");
    setTimeout(() => { t.hidden = true; }, 220);
  }, 2600);
}

function postWindow(action) {
  try {
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage({ type: "window", action });
    }
  } catch (e) { }
}

// 状态栏 chip / peek 的状态源。面板收起后 SSE 仍在跑，所以每条关键事件
// 都要把摘要推给宿主；宿主侧仍做状态白名单与长度裁剪，这里只是先收窄口径。
const HOST_AI_STATES = new Set(["idle", "busy", "approval", "done", "error"]);
function postHostStatus(stateName, summary, detail, canUndo, undoDomain, turnId) {
  if (!HOST_AI_STATES.has(stateName)) return;
  const undo = !!canUndo;
  try {
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage({
        type: "status",
        state: stateName,
        summary: String(summary || "").slice(0, 120),
        detail: String(detail || "").slice(0, 400),
        canUndo: undo,
        undoDomain: undo ? String(undoDomain || "") : "",
        turnId: Number(turnId) || 0
      });
    }
  } catch (e) { }
}

function hostUndoOf(ev) {
  if (!ev || ev.status !== "done" || ev.undoGroup !== "join") return null;
  const domain = String(ev.undoDomain || "");
  if (domain !== "file" && domain !== "bookmark") return null;
  return { domain, turnId: turnIdOf(ev), label: String(ev.label || ev.name || "") };
}

function hostStatusSummary(text, fallback) {
  const line = String(text || "")
    .split(/\r?\n/)
    .map((s) => s.trim())
    .find(Boolean);
  return (line || fallback || "已完成").slice(0, 120);
}

function scrollToBottom(force) {
  const m = $("messages");
  if (force || state.nearBottom) {
    requestAnimationFrame(() => { m.scrollTop = m.scrollHeight; });
  }
}

$("messages").addEventListener("scroll", () => {
  const m = $("messages");
  state.nearBottom = m.scrollHeight - m.scrollTop - m.clientHeight < 130;
});

// ---------------------------------------------------------------------------
// markdown-ish rendering (small, safe, no dependency)
// ---------------------------------------------------------------------------

function renderMarkdown(src) {
  const escaped = escapeHtml(src || "");
  const blocks = [];
  let work = escaped.replace(/```(\w*)\n([\s\S]*?)```/g, (m, lang, code) => {
    const i = blocks.length;
    blocks.push(`<pre><code>${code.replace(/\n$/, "")}</code></pre>`);
    return `\u0000BLOCK${i}\u0000`;
  });

  const inline = (text) => text
    .replace(/`([^`]+)`/g, "<code>$1</code>")
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)/g, '<span style="color:var(--accent-2)">$1</span>');

  const lines = work.split("\n");
  let out = "";
  let list = null;
  const flush = () => { if (list) { out += `</${list}>`; list = null; } };

  for (const line of lines) {
    const blockMatch = line.match(/^\u0000BLOCK(\d+)\u0000$/);
    if (blockMatch) { flush(); out += blocks[Number(blockMatch[1])]; continue; }
    const bullet = line.match(/^\s*[-*•]\s+(.*)$/);
    const numbered = line.match(/^\s*\d+[.)]\s+(.*)$/);
    if (bullet || numbered) {
      const tag = bullet ? "ul" : "ol";
      if (list !== tag) { flush(); list = tag; out += `<${tag}>`; }
      out += `<li>${inline((bullet || numbered)[1])}</li>`;
    } else {
      flush();
      if (line.trim() === "") out += "<br>";
      else out += `<p>${inline(line)}</p>`;
    }
  }
  flush();

  return out.replace(/<p><br><\/p>/g, "<br>").replace(/\n/g, "");
}

// ---------------------------------------------------------------------------
// message rendering
// ---------------------------------------------------------------------------

function hideEmpty() {
  $("emptyState").hidden = true;
  $("thread").hidden = false;
}

function showEmpty() {
  $("emptyState").hidden = false;
  $("thread").hidden = true;
  $("thread").innerHTML = "";
}

function setGreeting() {
  const h = new Date().getHours();
  const word = h < 11 ? "早上好" : h < 14 ? "中午好" : h < 18 ? "下午好" : "晚上好";
  $("greeting").textContent = `${word}，想点什么？`;
}

function appendUser(text, ts, turnId) {
  hideEmpty();
  const el = document.createElement("div");
  el.className = "msg user";
  const id = Number(turnId) || 0;
  if (id) el.dataset.turnId = String(id);
  el.innerHTML = `
    <div class="avatar">${icon("user")}</div>
    <div class="msg-main">
      <div class="msg-meta"><strong>你</strong><span>${timeText(ts)}</span></div>
      <div class="bubble">${renderMarkdown(text)}</div>
    </div>`;
  $("thread").appendChild(el);
  scrollToBottom(true);
  return el;
}

function appendAssistant(text, ts, actions, turnId) {
  hideEmpty();
  const el = document.createElement("div");
  el.className = "msg assistant";
  el.innerHTML = `
    <div class="avatar">${icon("bot")}</div>
    <div class="msg-main">
      <div class="msg-meta"><strong>AI 助手</strong><span>${timeText(ts)}</span></div>
      <div class="turn-actions" hidden>
        <button type="button" class="turn-act" data-turn-action="retry">重试</button>
        <button type="button" class="turn-act" data-turn-action="edit">编辑重试</button>
        <span class="turn-version" hidden></span>
      </div>
      <div class="step-ledger" hidden>
        <div class="ledger-head">
          <span class="ledger-ico">${icon("tool")}</span>
          <span class="ledger-turn" hidden></span>
          <span class="ledger-summary"></span>
          <span class="ledger-risk" hidden></span>
          <span class="ledger-time" hidden></span>
          ${icon("chevron", "ledger-chev")}
        </div>
        <div class="ledger-body"></div>
      </div>
      <div class="bubble${text ? "" : " typing"}">${text ? renderMarkdown(text) : ""}</div>
      ${actions && actions.length ? actionsHtml(actions) : ""}
    </div>`;
  $("thread").appendChild(el);
  scrollToBottom(true);

  if (actions && actions.length) {
    const line = el.querySelector(".actions-line");
    const list = el.querySelector(".actions-list");
    if (line && list) line.addEventListener("click", () => list.classList.toggle("open"));
  }

  // 执行台账：这一轮回答触发的工具调用属于它自己，挂进回答里而不是散落在
  // thread 上。ledgerEl 平时 hidden，第一条工具事件到达时由 mountTool 显示。
  const ledgerEl = el.querySelector(".step-ledger");
  const ledgerBody = el.querySelector(".ledger-body");
  let currentTurnId = Number(turnId) || 0;
  function applyTurnId(value) {
    const id = Number(value) || 0;
    if (!id) return;
    currentTurnId = id;
    el.dataset.turnId = String(id);
    ledgerEl.dataset.turnId = String(id);
    const label = ledgerEl.querySelector(".ledger-turn");
    if (label) {
      label.textContent = `第 ${id} 轮`;
      label.hidden = false;
    }
  }
  applyTurnId(currentTurnId);
  const turnActions = el.querySelector(".turn-actions");
  const versionLabel = el.querySelector(".turn-version");
  let prompt = "";
  function applyVersion(meta) {
    if (!meta) return;
    const attempt = Number(meta.attempt) || 0;
    if (attempt > 1 && versionLabel) {
      versionLabel.textContent = `第 ${attempt} 次尝试`;
      versionLabel.hidden = false;
    }
  }
  function markFailed(reason) {
    if (!turnActions) return;
    turnActions.hidden = false;
    if (versionLabel && reason) {
      versionLabel.textContent = `已停止/失败 · ${String(reason).slice(0, 20)}`;
      versionLabel.hidden = false;
    }
  }
  if (turnActions) {
    turnActions.querySelectorAll("[data-turn-action]").forEach((btn) => {
      btn.addEventListener("click", () => {
        if (!currentTurnId) return;
        if (btn.dataset.turnAction === "edit") editTurn(currentTurnId, prompt);
        else retryTurn(currentTurnId, prompt);
      });
    });
  }
  let ledgerManual = false;
  ledgerEl.querySelector(".ledger-head").addEventListener("click", () => {
    ledgerManual = true;   // 用户折叠过就别再被后续 token/事件强行展开
    ledgerEl.classList.toggle("open");
  });

  return {
    el,
    bubble: el.querySelector(".bubble"),
    ledgerEl,
    ledgerBody,
    get turnId() { return currentTurnId; },
    get prompt() { return prompt; },
    setPrompt(p) { prompt = String(p || ""); },
    setTurnId: applyTurnId,
    setVersion: applyVersion,
    markFailed,
    _ledgerCards: new Map(),   // id -> { node, ev }，与全局 toolCards 同一批事件
    _text: text || "",
    _raf: 0,
    _thinking: null,
    // 把一条工具事件挂到本回答的台账。返回卡片节点供 appendTool 更新内容。
    mountTool(ev) {
      ledgerEl.hidden = false;
      let c = this._ledgerCards.get(ev.id);
      if (!c) {
        const node = document.createElement("div");
        node.className = "ledger-step";
        ledgerBody.appendChild(node);
        c = { node, ev: {} };
        this._ledgerCards.set(ev.id, c);
      }
      Object.assign(c.ev, ev);
      return c.node;
    },
    // 重算台账摘要：N 步 + 风险分布 + 是否有失败/未达成（决定默认展开与否）。
    // 业界共识（progressive disclosure）：折叠"过程"、不折叠"结果与异常"。
    refreshLedger() {
      const cards = [...this._ledgerCards.values()];
      if (!cards.length) { ledgerEl.hidden = true; return; }
      ledgerEl.hidden = false;
      const counts = { read: 0, write: 0, danger: 0 };
      let bad = 0, done = 0, lastTs = 0;
      for (const c of cards) {
        const t = c.ev;
        const rawRisk = rowRisk(t);
        const r = ["read", "write", "danger"].includes(rawRisk) ? rawRisk : "write";
        counts[r]++;
        if (["error", "mismatch", "cancelled", "rejected"].includes(t.status)) bad++;
        if (["done", "mismatch", "error", "rejected", "cancelled"].includes(t.status)) done++;
        if (Number(t.ts) > lastTs) lastTs = Number(t.ts);
      }
      const riskBits = [];
      if (counts.read) riskBits.push(`只读 ${counts.read}`);
      if (counts.write) riskBits.push(`写 ${counts.write}`);
      if (counts.danger) riskBits.push(`危险 ${counts.danger}`);
      ledgerEl.querySelector(".ledger-summary").textContent =
        `执行 ${cards.length} 步${done ? ` · 完成 ${done}` : ""}`;
      const riskEl = ledgerEl.querySelector(".ledger-risk");
      riskEl.textContent = riskBits.join(" · ");
      riskEl.hidden = !riskBits.length;
      riskEl.classList.toggle("has-danger", counts.danger > 0);
      const timeEl = ledgerEl.querySelector(".ledger-time");
      if (timeEl) {
        timeEl.textContent = lastTs ? timeText(lastTs) : "";
        timeEl.hidden = !lastTs;
      }
      ledgerEl.classList.toggle("has-failure", bad > 0);
      ledgerEl.classList.toggle("has-danger", counts.danger > 0);
      // 失败/未达成/有危险操作 → 默认展开（异常不能被折起来）；否则默认折叠。
      // 用户手动折叠/展开过之后，后续流式事件不再覆盖他的选择。
      if (!ledgerManual) ledgerEl.classList.toggle("open", bad > 0 || counts.danger > 0);
    },
    setContent(t) {
      this.bubble.innerHTML = renderMarkdown(t);
      this.bubble.classList.remove("typing");
      scrollToBottom();
    },
    // Reparse markdown at most once per animation frame. Rendering the whole
    // accumulated answer on every token is quadratic on long replies and makes
    // the panel visibly stutter while the model is still writing.
    scheduleRender() {
      if (this._raf) return;
      const a = this;
      this._raf = requestAnimationFrame(() => {
        a._raf = 0;
        a.setContent(a._text || "");
      });
    },
    // Reasoning chunks live in a collapsible box OUTSIDE the answer bubble, so
    // the final reply stays clean markdown and can be folded away.
    addThinking(chunk) {
      if (!chunk) return;
      if (!this._thinking) {
        const box = document.createElement("details");
        box.className = "thinking-box";
        box.open = true;
        box.innerHTML = `<summary>思考过程</summary><pre class="thinking-text"></pre>`;
        this.el.querySelector(".msg-main").insertBefore(box, this.bubble);
        this._thinking = box.querySelector(".thinking-text");
      }
      this._thinking.textContent += chunk;
      scrollToBottom();
    },
    finish(t) {
      if (this._raf) { cancelAnimationFrame(this._raf); this._raf = 0; }
      this._text = t || "";
      this.bubble.innerHTML = renderMarkdown(this._text);
      this.bubble.classList.remove("typing");
      scrollToBottom();
    }
  };
}

function actionsHtml(actions) {
  return `
    <div class="actions-line">${icon("check")}<span>执行了 ${actions.length} 个操作</span>${icon("chevron")}</div>
    <div class="actions-list">${actions.map((a) => `<span>${escapeHtml(a)}</span>`).join("")}</div>`;
}

function appendTool(ev, assistant) {
  hideEmpty();
  const toolTurnId = turnIdOf(ev);
  if (toolTurnId && assistant && typeof assistant.setTurnId === "function") {
    assistant.setTurnId(toolTurnId);
  }
  // peek 的撤销按钮只认成功且声明 undoGroup=join 的工具；历史回放也会走
  // 这里，所以同一条成功记录回放后仍保留正确的撤销域。
  const undo = hostUndoOf(ev);
  if (undo) state.lastUndo = undo;
  // 两步更新到位：① 全局 toolCards 给结构化活动流；② 本轮回答的执行台账。
  // 台账是"同一批事件嵌进回答"的呈现，两者共享同一份 ev，不会画出两种状态。
  let card = state.toolCards.get(ev.id);
  // "mismatch" = handler 说成功、但自断言不符。它与 done 在视觉上必须分开：
  // 一个是"真的成了"，一个是"命令返回了成功但效果没达成"（最危险的一类）。
  const statusText = {
    running: "执行中", done: "完成", error: "失败", rejected: "已跳过",
    cancelled: "已停止", mismatch: "效果未达成"
  }[ev.status] || ev.status;
  const risk = rowRisk(ev);
  const riskText = { read: "只读", write: "写", danger: "危险" }[risk] || "写";
  const isBad = ["error", "mismatch", "cancelled", "rejected"].includes(ev.status);

  // 台账 step：每个工具一条紧凑行，默认只有一行摘要（命名卡片 + 状态 + 风险徽章）。
  let stepNode = null;
  if (assistant && typeof assistant.mountTool === "function") {
    stepNode = assistant.mountTool(ev);
    if (!stepNode.dataset.ready) {
      stepNode.dataset.ready = "1";
      stepNode.innerHTML = `
        <div class="step-row">
          <span class="step-ico"></span>
          <span class="step-title"></span>
          <span class="step-cmd"></span>
          <span class="step-risk ${risk}">${riskText}</span>
          <span class="step-badge ${ev.status}"></span>
          ${icon("chevron", "step-chev")}
        </div>
        <div class="step-detail">
          <div class="tool-label">参数</div>
          <pre class="tool-pre step-args"></pre>
          <div class="tool-label result-label" hidden>结果</div>
          <pre class="tool-pre result-pre" hidden></pre>
          <div class="tool-label assert-label" hidden>自断言</div>
          <div class="assert-list" hidden></div>
        </div>`;
      stepNode.querySelector(".step-row").addEventListener("click", () => stepNode.classList.toggle("open"));
    }
    // 行内字段：标题 / 点分命令名 / 风险档 / 状态 + 图标
    stepNode.querySelector(".step-title").textContent = ev.label || ev.name || "tool";
    stepNode.querySelector(".step-cmd").textContent = ev.command || ev.name || "";
    const stepRisk = stepNode.querySelector(".step-risk");
    stepRisk.className = "step-risk " + risk;
    stepRisk.textContent = riskText;
    const stepBadge = stepNode.querySelector(".step-badge");
    stepBadge.className = "step-badge " + ev.status;
    stepBadge.textContent = statusText;
    stepNode.querySelector(".step-ico").innerHTML = ev.status === "running" ? '<span class="spinner"></span>'
      : icon(ev.status === "done" ? "check" : isBad ? "x" : "tool");
    const stepArgs = stepNode.querySelector(".step-args");
    stepArgs.textContent = JSON.stringify(ev.args || {}, null, 2);
  }

  if (!card) {
    const el = document.createElement("div");
    el.className = "tool-card";
    el.innerHTML = `
      <div class="tool-head">
        <span class="tool-ico">${ev.status === "running" ? '<span class="spinner"></span>' : icon("tool")}</span>
        <span class="tool-title">${escapeHtml(ev.label || ev.name || "tool")}</span>
        <span class="tool-badge ${ev.status}">${statusText}</span>
        ${icon("chevron", "tool-chev")}
      </div>
      <div class="tool-body">
        <div class="tool-label">参数</div>
        <pre class="tool-pre">${escapeHtml(JSON.stringify(ev.args || {}, null, 2))}</pre>
        <div class="tool-label result-label" hidden>结果</div>
        <pre class="tool-pre result-pre" hidden></pre>
        <div class="tool-label assert-label" hidden>自断言</div>
        <div class="assert-list" hidden></div>
      </div>`;
    card = { el };
    state.toolCards.set(ev.id, card);
  }

  const badge = card.el.querySelector(".tool-badge");
  badge.className = "tool-badge " + ev.status;
  badge.textContent = statusText;
  const ico = card.el.querySelector(".tool-ico");
  ico.innerHTML = ev.status === "running" ? '<span class="spinner"></span>'
    : icon(ev.status === "done" ? "check" : isBad ? "x" : "tool");

  const hasResult = ev.result || ev.error || ev.errorCode;
  if (hasResult) {
    const resultLines = [];
    if (ev.errorCode) resultLines.push(`[${ev.errorCode}]`);
    resultLines.push(ev.error ? String(ev.error) : String(ev.result || ""));
    const text = resultLines.join(" ");
    card.el.querySelector(".result-label").hidden = false;
    const pre = card.el.querySelector(".result-pre");
    pre.hidden = false;
    pre.textContent = text;
    if (isBad) card.el.classList.add("open");   // 异常浮到表面，成功保持折叠
    if (stepNode) {
      stepNode.querySelector(".result-label").hidden = false;
      const sPre = stepNode.querySelector(".result-pre");
      sPre.hidden = false;
      sPre.textContent = text;
      if (isBad) stepNode.classList.add("open");
    }
  }

  // 自断言逐条列出：期望值 vs 实际值。只说"未达成"没用，
  // 用户需要知道判据是什么，才能判断是命令坏了还是期望写错了。
  if (Array.isArray(ev.assertions) && ev.assertions.length) {
    const rows = ev.assertions.map((a) => {
      const ok = a && a.ok === true;
      const expr = escapeHtml(a && a.expr ? a.expr : "?");
      const expect = escapeHtml(a && a.expect !== undefined ? String(a.expect) : "");
      const actual = escapeHtml(a && a.actual !== undefined ? String(a.actual) : "");
      return `<div class="assert-row ${ok ? "ok" : "bad"}">
        <span class="assert-mark">${ok ? "✓" : "✗"}</span>
        <span class="assert-expr">${expr}</span>
        <span class="assert-vals">期望 ${expect} · 实际 ${actual}</span>
      </div>`;
    }).join("");
    const label = card.el.querySelector(".assert-label");
    const list = card.el.querySelector(".assert-list");
    label.hidden = false; list.hidden = false; list.innerHTML = rows;
    if (stepNode) {
      const sLabel = stepNode.querySelector(".assert-label");
      const sList = stepNode.querySelector(".assert-list");
      sLabel.hidden = false; sList.hidden = false; sList.innerHTML = rows;
    }
  }

  if (assistant && typeof assistant.refreshLedger === "function") assistant.refreshLedger();
  scrollToBottom();
}

function appendApproval(ev) {
  hideEmpty();
  const el = document.createElement("div");
  el.className = "approval-card";
  el.innerHTML = `
    <div class="approval-top">${icon("alert")}<strong>${escapeHtml(ev.title || "需要你确认")}</strong></div>
    <p class="approval-desc">${escapeHtml(ev.description || "这个操作会修改本机内容，确认后才会执行。")}</p>
    <div class="approval-detail">${escapeHtml(ev.details || "")}</div>
    <div class="approval-btns">
      <button class="btn ghost" data-act="reject">跳过</button>
      <button class="btn primary" data-act="approve">确认执行</button>
    </div>`;
  $("thread").appendChild(el);
  scrollToBottom(true);

  const resolve = async (decision) => {
    const btns = el.querySelector(".approval-btns");
    if (btns.dataset.done) return;
    btns.dataset.done = "1";
    btns.innerHTML = `<span class="approval-resolved">${decision === "approve" ? "正在提交确认…" : "正在提交跳过…"}</span>`;
    try {
      const r = await apiFetch("/api/approve", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id: ev.id, decision })
      });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      btns.innerHTML = `<span class="approval-resolved">${decision === "approve" ? "已确认，正在执行…" : "已跳过"}</span>`;
    } catch (e) {
      // 不能先改文案再说谎：提交失败必须恢复按钮，让用户能重试。
      delete btns.dataset.done;
      btns.innerHTML = `
        <span class="approval-failed">提交失败：${escapeHtml(e.message)}</span>
        <button class="btn ghost" data-act="reject">跳过</button>
        <button class="btn primary" data-act="approve">确认执行</button>`;
      btns.querySelector('[data-act="approve"]').addEventListener("click", () => resolve("approve"));
      btns.querySelector('[data-act="reject"]').addEventListener("click", () => resolve("reject"));
      toast("确认请求失败：" + e.message);
    }
  };
  el.querySelector('[data-act="approve"]').addEventListener("click", () => resolve("approve"));
  el.querySelector('[data-act="reject"]').addEventListener("click", () => resolve("reject"));
  state.approvals.set(ev.id, el);
}

function setStatus(text) {
  const line = $("statusLine");
  if (!text) { line.hidden = true; return; }
  line.hidden = false;
  $("statusText").textContent = text;
}

// ---------------------------------------------------------------------------
// context / config / history
// ---------------------------------------------------------------------------

async function api(url, opts) {
  const r = await apiFetch(url, opts);
  if (!r.ok) {
    if (r.status === 401) {
      throw new Error("本地令牌缺失或已过期，请从 AI 面板打开页面");
    }
    let msg = `HTTP ${r.status}`;
    try {
      const j = await r.json();
      if (j && j.error) msg = j.error;
    } catch (e) { }
    throw new Error(msg);
  }
  return r.json();
}

const JSON_HEADERS = { "Content-Type": "application/json" };
const postJson = (url, body) => api(url, {
  method: "POST", headers: JSON_HEADERS, body: JSON.stringify(body)
});

// ---------------------------------------------------------------------------
// Vendor / protocol catalog fallback
// ---------------------------------------------------------------------------
// service/protocols.js is the source of truth and its /api/config payload wins.
// But the window must still render a usable dropdown when it is pointed at an
// older service build (no `vendors`/`protocols` in the payload) or when that
// request fails -- otherwise the select holds a single placeholder option and
// looks empty. Keep these entries in sync with VENDORS in service/protocols.js.
const FALLBACK_PROTOCOLS = [
  { id: "openai", label: "OpenAI 兼容" },
  { id: "anthropic", label: "Anthropic Messages" }
];

const FALLBACK_VENDORS = [
  { id: "openai",      name: "OpenAI",               baseUrl: "https://api.openai.com/v1",                        protocol: "openai",    defaultModel: "gpt-4o-mini" },
  { id: "deepseek",     name: "DeepSeek",             baseUrl: "https://api.deepseek.com/v1",                       protocol: "openai",    defaultModel: "deepseek-chat" },
  { id: "moonshot",     name: "Moonshot 月之暗面",    baseUrl: "https://api.moonshot.cn/v1",                        protocol: "openai",    defaultModel: "moonshot-v1-8k" },
  { id: "zhipu",        name: "智谱 GLM",             baseUrl: "https://open.bigmodel.cn/api/paas/v4",              protocol: "openai",    defaultModel: "glm-4-flash" },
  { id: "siliconflow",  name: "硅基流动 SiliconFlow", baseUrl: "https://api.siliconflow.cn/v1",                      protocol: "openai",    defaultModel: "Qwen/Qwen2.5-7B-Instruct" },
  { id: "dashscope",    name: "阿里云百炼",           baseUrl: "https://dashscope.aliyuncs.com/compatible-mode/v1", protocol: "openai",    defaultModel: "qwen-plus" },
  { id: "ark",          name: "火山方舟",             baseUrl: "https://ark.cn-beijing.volces.com/api/v3",           protocol: "openai",    defaultModel: "doubao-pro-32k" },
  { id: "anthropic",    name: "Anthropic",            baseUrl: "https://api.anthropic.com/v1",                      protocol: "anthropic", defaultModel: "claude-3-5-haiku-latest" },
  { id: "ollama",       name: "Ollama 本地",          baseUrl: "http://localhost:11434/v1",                          protocol: "openai",    defaultModel: "llama3.1" }
];

// Compact colour marks for the vendor picker. A native <option> cannot hold
// markup, so the picker is a custom list (see renderVendorOptions). Unknown ids
// (e.g. a locally saved custom vendor) fall back to their initial letter.
const VENDOR_MARKS = {
  openai:      { mark: "AI", color: "#10a37f" },
  deepseek:    { mark: "DS", color: "#4d6bfe" },
  moonshot:    { mark: "MK", color: "#7c5cff" },
  zhipu:       { mark: "ZP", color: "#2f7de1" },
  siliconflow: { mark: "SF", color: "#e0872b" },
  dashscope:   { mark: "QW", color: "#e8622d" },
  ark:         { mark: "DK", color: "#d3554c" },
  anthropic:   { mark: "AN", color: "#c4704f" },
  ollama:      { mark: "OL", color: "#6f7b85" }
};

function protocolList() {
  const list = state.models.protocols;
  return (list && list.length) ? list : FALLBACK_PROTOCOLS;
}

function vendorPresets() {
  const list = state.models.vendors;
  return (list && list.length) ? list : FALLBACK_VENDORS;
}

async function loadConfig(opts) {
  const silent = !!(opts && opts.silent);
  try {
    const previous = state.config && state.config.model;
    const next = await api("/api/config");
    state.config = next;
    $("modelName").textContent = next.model || "—";
    fitComposer();
    state.models.list = Array.isArray(next.modelList) ? next.modelList : [];
    state.models.providers = Array.isArray(next.providers) ? next.providers : [];
    state.models.vendors = Array.isArray(next.vendors) ? next.vendors : [];
    state.models.protocols = Array.isArray(next.protocols) ? next.protocols : [];
    // Older service builds omit both fields. Remember that so the dialog can
    // warn instead of silently failing on the new validation/save endpoints.
    state.models.serverHasCatalog = Array.isArray(next.vendors) && Array.isArray(next.protocols);
    renderModelMenu();
    // Seed the recent group from the live model on a fresh profile. A user who
    // never switched models still sees the current choice at the top.
    const activeRow = state.models.list.find((m) => m.active);
    if (activeRow && !state.models.recent.length) {
      rememberModel(activeRow);
      renderModelMenu();
    }
    if (!$("modelModal").hidden) {
      renderVendorOptions();
      renderProtocolOptions();
      renderProviderGroups();
    }
    if (!silent && previous && previous !== next.model) toast(`模型已切换为 ${next.model}`);
  } catch (e) {
    $("modelName").textContent = "服务未连接";
    fitComposer();
  }
}

async function loadHealth() {
  const el = $("connStatus");
  try {
    state.health = await api("/api/health");
    if (!state.health.model.ready) {
      el.className = "conn bad";
      el.textContent = "模型未配置";
      toast("还没有配置 API Key，点下方模型 → 自定义模型即可添加。");
    } else {
      el.className = "conn ok";
      el.textContent = state.health.dryRun ? "dry run" : "服务已连接";
    }
  } catch (e) {
    el.className = "conn bad";
    el.textContent = "服务未连接";
  }
}

async function loadContext(refresh) {
  const btn = $("refreshBtn");
  if (refresh) btn.classList.add("spin");
  try {
    const c = await api("/api/context" + (refresh ? "?refresh=1" : ""));
    const chip = $("contextChip");
    const text = $("contextText");
    // The composer's attachment picker and @ mention list read the same
    // snapshot, so refreshing the folder context feeds both at once.
    state.contextItems = Array.isArray(c.selected) ? c.selected : [];
    state.contextFolder = String(c.folder || "");
    state.contextListerIndex = Number.isFinite(Number(c.listerIndex)) ? Number(c.listerIndex) : -1;
    if (c.folder) {
      chip.classList.remove("warn");
      const name = c.folderName || c.folder;
      let label = name;
      if (c.selectedCount > 0) label += ` · 已选 ${c.selectedCount} 项`;
      else if (c.itemCount != null) label += ` · ${c.itemCount} 项`;
      text.textContent = label;
      chip.title = c.folder + (c.reason ? `\n${c.reason}` : "");
    } else {
      chip.classList.add("warn");
      text.textContent = "未获取到当前文件夹";
    }
    return c;
  } catch (e) {
    $("contextChip").classList.add("warn");
    $("contextText").textContent = "上下文不可用";
    return null;
  } finally {
    if (refresh) btn.classList.remove("spin");
  }
}

async function loadHistory() {
  try {
    const h = await api("/api/history?sessionId=" + encodeURIComponent(state.sessionId));
    const msgs = h.messages || [];
    state.toolCards.clear();
    rebuildActivityFromHistory(msgs);
    if (!msgs.length) { showEmpty(); return; }
    showEmpty();
    let lastPrompt = "";
    for (let i = 0; i < msgs.length; i++) {
      const m = msgs[i];
      if (m.role === "user") {
        lastPrompt = String(m.content || "");
        appendUser(m.content, m.ts, turnIdOf(m));
      } else {
        const a = appendAssistant(m.content || "", m.ts, m.actions, turnIdOf(m));
        a.setPrompt(lastPrompt);
        if (m.retryOf || m.attempt || m.supersededBy) a.setVersion(m);
        // 历史里的工具调用也要回到"产生它的那条回答"里。没有 id 的旧数据
        // 补一个稳定 id，否则同轮多步会塌到同一张台账卡片上。
        (m.tools || []).forEach((t, j) => {
          const tool = Object.assign({}, t);
          if (!tool.id) tool.id = `h${i + 1}-${j + 1}`;
          if (!tool.turn && m.turn) tool.turn = m.turn;
          appendTool(tool, a);
        });
        // Failed / stopped turns are persisted too. Show why the reply looks
        // incomplete instead of replaying a truncated answer as if it were final.
        if (m.error) {
          a.finish((m.content || "") + `\n\n⚠ ${m.error === "stopped" ? "已停止" : m.error}`);
          a.bubble.style.color = "var(--amber)";
          if (a.markFailed) a.markFailed(m.error);
        }
        lastPrompt = "";
      }
    }
    $("emptyState").hidden = true;
    $("thread").hidden = false;
    scrollToBottom(true);
  } catch (e) {
    showEmpty();
  }
}

// ---------------------------------------------------------------------------
// chat turn
// ---------------------------------------------------------------------------

function setBusy(on) {
  state.busy = on;
  const btn = $("sendBtn");
  btn.classList.toggle("stop", on);
  btn.innerHTML = icon(on ? "stop" : "send");
  btn.title = on ? "停止 (Esc)" : "发送 (Enter)";
  $("input").disabled = false;
}

// 事件归属：generation 是前端回合身份，ev.turn 是服务端回合身份。
// 传 generation=undefined 时跳过检查，兼容历史调用与离线测试；真实 send 一定传。
function ownsEvent(ev, generation) {
  if (!ev) return false;
  if (generation !== undefined && generation !== state.generation) return false;
  const turn = turnIdOf(ev);
  const active = Number(state.activeTurn) || 0;
  if (turn && active && turn !== active) return false;
  return true;
}

// 活动流的轮次与回答气泡的 ledger 用同一个服务端 turnId。历史恢复时来自落盘
// 条目，实时流里最早由 start 事件写入；两者都不从用户消息条数推算。
// ---------------------------------------------------------------------------
// 结构化活动流视图（ROADMAP P4-a）
//
// 与聊天气泡消费的是同一批 SSE tool 事件，但这里按回合分组、按风险/状态筛选、
// 点行展开详情。数据模型 activity.turns 是唯一真源，视图是它的纯函数渲染 ——
// 实时事件与历史恢复都只改数据、再调 renderActivity()，两条路径不会画出两种样子。
// ---------------------------------------------------------------------------

const ACT_STATUS = {
  running: "执行中", done: "完成", error: "失败", rejected: "已跳过",
  cancelled: "已停止", mismatch: "效果未达成"
};
const ACT_RISK = {
  read: "只读", write: "写", danger: "危险"
};

// 把一条 tool 事件并入活动流。同一个 id 的多次更新（running → done）就地改，
// 不新增行 —— 与 toolCards 的更新口径一致。
function recordActivity(ev) {
  const a = state.activity;
  const turn = turnIdOf(ev) || a.currentTurn || 0;
  a.currentTurn = turn;
  let g = a.turns.find((x) => x.turn === turn || (x.turnId && x.turnId === turn));
  if (!g) {
    g = {
      turn, turnId: turn, ts: ev.ts || Date.now(), prompt: "", tools: [],
      retryOf: Number(ev.retryOf) || 0,
      rootTurnId: Number(ev.rootTurnId) || 0,
      attempt: Number(ev.attempt) || 0
    };
    a.turns.push(g);
  }
  if (!g.turnId && turn) g.turnId = turn;
  if (!g.retryOf && ev.retryOf) g.retryOf = Number(ev.retryOf);
  if (!g.rootTurnId && ev.rootTurnId) g.rootTurnId = Number(ev.rootTurnId);
  if (!g.attempt && ev.attempt) g.attempt = Number(ev.attempt);
  if (!g.ts && ev.ts) g.ts = ev.ts;
  // 本轮第一个事件到达时消费发送时记下的提示词，之后清空避免串到下一轮。
  if (a.pendingPrompt && !g.prompt) {
    g.prompt = String(a.pendingPrompt);
    a.pendingPrompt = "";
  }
  let t = g.tools.find((x) => x.id === ev.id);
  if (!t) { t = {}; g.tools.push(t); }
  Object.assign(t, ev);
  renderActivity();
}

// 从落盘历史重建活动流。历史的 assistant 条目带 turn + 完整 tools，
// user 条目的内容作为该回合的提示词（紧邻其后的 assistant 回合）。
function rebuildActivityFromHistory(msgs) {
  const a = state.activity;
  a.turns = [];
  let lastPrompt = "";
  let nextTurn = 1;
  for (const m of msgs) {
    if (!m) continue;
    if (m.role === "user") {
      lastPrompt = String(m.content || "");
      continue;
    }
    const turn = turnIdOf(m) || nextTurn++;
    let g = a.turns.find((x) => x.turn === turn || (x.turnId && x.turnId === turn));
    if (!g) {
      g = {
        turn, turnId: turnIdOf(m) || 0, ts: m.ts || 0, prompt: lastPrompt, tools: [],
        retryOf: Number(m.retryOf) || 0,
        rootTurnId: Number(m.rootTurnId) || 0,
        attempt: Number(m.attempt) || 0,
        supersededBy: Number(m.supersededBy) || 0
      };
      a.turns.push(g);
    }
    if (!g.retryOf && m.retryOf) g.retryOf = Number(m.retryOf);
    if (!g.rootTurnId && m.rootTurnId) g.rootTurnId = Number(m.rootTurnId);
    if (!g.attempt && m.attempt) g.attempt = Number(m.attempt);
    if (!g.supersededBy && m.supersededBy) g.supersededBy = Number(m.supersededBy);
    for (const t of (m.tools || [])) {
      const row = Object.assign({}, t);
      row.turn = turn;
      if (!row.id) row.id = `h${turn}-${g.tools.length}`;
      g.tools.push(row);
    }
    lastPrompt = "";
  }
  a.turns.sort((x, y) => x.turn - y.turn);
  a.currentTurn = a.turns.reduce((mx, x) => Math.max(mx, x.turn), 0);
  renderActivity();
}

function actMatchesFilter(t) {
  const f = state.activity.filter;
  if (f === "all") return true;
  if (f === "fail") return ["error", "mismatch", "cancelled", "rejected"].includes(t.status);
  return (t.risk || rowRisk(t)) === f;
}

// 历史条目可能没有显式 risk（旧数据），按 annotations 无法再查时的保守回落：
// 有 error 视为失败仍归原档；这里只在缺 risk 时按命令名猜 read（nav./view./file.list…），
// 其余写。仅用于展示，不影响审批与安全判定（那在服务端）。
function rowRisk(t) {
  if (t.risk) return t.risk;
  const n = String(t.name || t.command || "");
  return /^(nav|view|win|tab\.list|bmk\.list|file\.list|tool\.list|state|session\.list)/.test(n)
    ? "read" : "write";
}

function actRowHtml(t) {
  const status = String(t.status || "running");
  const statusText = ACT_STATUS[status] || status;
  const risk = rowRisk(t);
  const riskText = ACT_RISK[risk] || "写";
  const cmd = String(t.command || t.name || "");
  const title = String(t.label || t.name || "命令");
  const ts = Number(t.ts) ? `<span class="act-time">${timeText(t.ts)}</span>` : "";

  // 详情：参数 / 结果 / 自断言 / 失败原因。与其他卡片一致的四段结构。
  const parts = [];
  parts.push(`<div class="act-detail-label">参数</div><pre>${escapeHtml(JSON.stringify(t.args || {}, null, 2))}</pre>`);
  if (t.result || t.error || t.errorCode) {
    const lines = [];
    if (t.errorCode) lines.push(`[${t.errorCode}]`);
    lines.push(t.error ? String(t.error) : String(t.result || ""));
    parts.push(`<div class="act-detail-label">结果</div><pre>${escapeHtml(lines.join(" "))}</pre>`);
  }
  if (Array.isArray(t.assertions) && t.assertions.length) {
    const rows = t.assertions.map((a) => {
      const ok = a && a.ok === true;
      const expr = escapeHtml(a && a.expr ? a.expr : "?");
      const expect = escapeHtml(a && a.expect !== undefined ? String(a.expect) : "");
      const actual = escapeHtml(a && a.actual !== undefined ? String(a.actual) : "");
      return `<div class="act-assert ${ok ? "ok" : "bad"}">
        <span class="act-assert-mark">${ok ? "✓" : "✗"}</span>
        <span class="act-assert-expr">${expr}</span>
        <span class="act-assert-vals">期望 ${expect} · 实际 ${actual}</span>
      </div>`;
    }).join("");
    parts.push(`<div class="act-detail-label">自断言</div>${rows}`);
  }
  if (status === "error" || status === "mismatch" || status === "rejected" || status === "cancelled") {
    parts.push(`<div class="act-detail-label">失败原因</div><div class="act-fail">${escapeHtml(statusText)}${t.error ? "：" + escapeHtml(String(t.error)) : ""}</div>`);
  }

  return `<div class="act-row" data-id="${escapeHtml(String(t.id || ""))}">
      <span class="act-dot ${status}"></span>
      <span class="act-title" title="${escapeHtml(title)}">${escapeHtml(title)}</span>
      <code class="act-cmd" title="${escapeHtml(cmd)}">${escapeHtml(cmd)}</code>
      <span class="act-risk ${risk}">${riskText}</span>
      <span class="act-status ${status}">${escapeHtml(statusText)}</span>
      ${ts}
    </div>
    <div class="act-detail">${parts.join("")}</div>`;
}

// 切换活动分支：只改 selectedTurnId，不删除任何历史。回到聊天视图后重新加载
// 历史，活动流与内联台账都会按新分支重建。
async function selectBranch(rootTurnId, turnId) {
  if (!turnId) return;
  try {
    await api("/api/branch", {
      method: "POST",
      headers: JSON_HEADERS,
      body: JSON.stringify({
        sessionId: state.sessionId,
        rootTurnId: Number(rootTurnId) || 0,
        turnId: Number(turnId)
      })
    });
    toast(`已切换到第 ${turnId} 轮`);
    await loadHistory();
  } catch (e) {
    toast("切换版本失败：" + e.message);
  }
}

// 从活动流按权威 turnId 定位回答。聊天视图与活动流共享同一个 id，
// 所以点击轮次可以直接打开对应回答的内联执行台账，而不是按时间猜消息。
function focusAssistantTurn(turnId) {
  const id = Number(turnId) || 0;
  if (!id) return;
  setView("chat");
  const node = document.querySelector(`.msg.assistant[data-turn-id="${id}"]`);
  if (!node) return;
  const ledger = node.querySelector(".step-ledger");
  if (ledger) {
    ledger.hidden = false;
    ledger.classList.add("open");
  }
  if (typeof node.scrollIntoView === "function") {
    node.scrollIntoView({ block: "start", behavior: "smooth" });
  }
}

function renderActivity() {
  const a = state.activity;
  // 计数徽标：全部工具事件数（不受筛选影响），让用户一眼看到有没有活动。
  const total = a.turns.reduce((n, g) => n + g.tools.length, 0);
  const badge = $("activityCount");
  if (badge) { badge.textContent = String(total); badge.hidden = total === 0; }

  const list = $("activityList");
  const empty = $("activityEmpty");
  if (!list || !empty) return;
  if (!total) { list.innerHTML = ""; empty.hidden = false; return; }
  empty.hidden = true;

  let html = "";
  for (const g of a.turns) {
    const rows = g.tools.filter(actMatchesFilter);
    if (!rows.length) continue;
    const turnTime = Number(g.ts) ? timeText(g.ts) : "";
    const prompt = escapeHtml(g.prompt || "（对话回合）");
    const turnId = Number(g.turnId) || 0;
    const rootTurnId = Number(g.rootTurnId) || turnId;
    const canSelect = turnId && (Number(g.attempt) > 1 || Number(g.retryOf) || Number(g.supersededBy));
    const headBits = [`第 ${g.turn} 轮`];
    if (Number(g.attempt) > 1) headBits.push(`尝试 ${g.attempt}`);
    if (Number(g.supersededBy)) headBits.push("旧版本");
    const headLabel = headBits.join(" · ");
    html += `<div class="act-turn"${turnId ? ` data-turn-id="${turnId}"` : ""}>
      <div class="act-turn-head">
        ${turnId
          ? `<button type="button" class="act-turn-no" data-focus-turn="${turnId}" title="定位到第 ${turnId} 轮回答">${headLabel}</button>`
          : `<span class="act-turn-no">${headLabel}</span>`}
        ${canSelect
          ? `<button type="button" class="act-branch" data-select-root="${rootTurnId}" data-select-turn="${turnId}" title="切换到此版本继续">切换</button>`
          : ""}
        <span class="act-turn-prompt" title="${prompt}">${prompt}</span>
        ${turnTime ? `<span class="act-turn-time">${turnTime}</span>` : ""}
      </div>
      ${rows.map(actRowHtml).join("")}
    </div>`;
  }
  if (!html) {
    list.innerHTML = "";
    empty.hidden = false;
    empty.querySelector("strong").textContent = "没有符合筛选的命令";
    return;
  }
  list.innerHTML = html;
  // 展开/收起详情
  list.querySelectorAll(".act-row").forEach((row) => {
    row.addEventListener("click", () => {
      row.classList.toggle("open");
    });
  });
  list.querySelectorAll("[data-focus-turn]").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      focusAssistantTurn(Number(btn.dataset.focusTurn));
    });
  });
  list.querySelectorAll("[data-select-turn]").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      selectBranch(Number(btn.dataset.selectRoot), Number(btn.dataset.selectTurn));
    });
  });
}

function setView(view) {
  const a = state.activity;
  a.view = view === "activity" ? "activity" : "chat";
  localStorage.setItem("ac.view", a.view);
  const chatOn = a.view === "chat";
  $("messages").hidden = !chatOn;
  $("activityView").hidden = chatOn;
  $("activityFilters").hidden = chatOn;
  $("activityFilterSep").hidden = chatOn;
  $("tabChat").classList.toggle("active", chatOn);
  $("tabActivity").classList.toggle("active", !chatOn);
  if (!chatOn) renderActivity();
  else scrollToBottom(true);
}

function setActivityFilter(filter) {
  state.activity.filter = filter;
  $("activityFilters").querySelectorAll(".filter-chip").forEach((b) => {
    b.classList.toggle("active", b.dataset.filter === filter);
  });
  renderActivity();
}

// Approval cards are per-turn. If the turn ends while one is still open, the
// buttons must stop looking clickable -- /api/approve would 404 anyway.
function clearApprovals() {
  for (const el of state.approvals.values()) {
    const btns = el.querySelector(".approval-btns");
    if (btns && !btns.dataset.done) {
      btns.dataset.done = "1";
      btns.innerHTML = `<span class="approval-resolved">回合已结束</span>`;
    }
  }
  state.approvals.clear();
}

function handleEvent(ev, assistant, generation) {
  if (!ownsEvent(ev, generation)) return;
  const evTurnId = turnIdOf(ev);
  if (evTurnId && assistant && typeof assistant.setTurnId === "function") {
    assistant.setTurnId(evTurnId);
  }
  if (assistant && typeof assistant.setVersion === "function" && (ev.retryOf || ev.attempt)) {
    assistant.setVersion(ev);
  }
  if (ev.type === "start") {
    state.activeTurn = evTurnId || 0;
  }
  switch (ev.type) {
    case "start":
      setStatus("正在连接模型…");
      postHostStatus("busy", "正在连接模型…", "", false, "", evTurnId);
      break;
    case "status":
      setStatus(ev.text || "处理中…");
      postHostStatus("busy", ev.text || "处理中…", "", false, "", evTurnId);
      break;
    case "delta":
      assistant._text = (assistant._text || "") + (ev.text || "");
      assistant.scheduleRender();
      break;
    case "thinking":
      assistant.addThinking(ev.text || "");
      break;
    case "tool":
      appendTool(ev, assistant);
      recordActivity(ev);
      if (ev.status === "running" || ev.status === "done") {
        const verb = ev.status === "done" ? "完成" : "执行";
        postHostStatus("busy", `${verb}：${ev.label || ev.name || "tool"}`,
                       "", false, "", evTurnId);
      }
      break;
    case "approval":
      setStatus("等待你确认…");
      appendApproval(ev);
      postHostStatus("approval", ev.title || "需要你确认",
                     ev.description || ev.details || "", false, "", evTurnId);
      break;
    case "done":
      assistant.finish(ev.reply || assistant._text || "");
      recordUsage(ev.usage);
      if (ev.actions && ev.actions.length && !assistant.el.querySelector(".actions-line")) {
        const wrap = document.createElement("div");
        wrap.innerHTML = actionsHtml(ev.actions);
        const main = assistant.el.querySelector(".msg-main");
        while (wrap.firstChild) main.appendChild(wrap.firstChild);
        const line = main.querySelector(".actions-line");
        const list = main.querySelector(".actions-list");
        if (line && list) line.addEventListener("click", () => list.classList.toggle("open"));
      }
      setStatus("");
      setBusy(false);
      const undo = state.lastUndo;
      postHostStatus("done",
                     hostStatusSummary(ev.reply || assistant._text, "已完成"),
                     undo ? `第 ${evTurnId || undo.turnId || 0} 轮：${undo.label}` : `第 ${evTurnId || 0} 轮完成`,
                     !!undo, undo ? undo.domain : "", evTurnId);
      loadContext(true);
      break;
    case "error": {
      // Keep whatever already streamed. Replacing the bubble with just "⚠ ..."
      // used to throw away the entire partial answer on every network hiccup.
      const msg = ev.message === "stopped" ? "已停止" : (ev.message || "出错了");
      const prefix = assistant._text ? assistant._text + "\n\n" : "";
      assistant.finish(prefix + `⚠ ${msg}`);
      assistant.bubble.style.color = "var(--amber)";
      if (assistant.markFailed) assistant.markFailed(msg);
      recordUsage(ev.usage);
      clearApprovals();
      setStatus("");
      setBusy(false);
      {
        const undo = state.lastUndo;
        postHostStatus("error", msg, "", !!undo, undo ? undo.domain : "", evTurnId);
      }
      break;
    }
    default:
      break;
  }
}

// 旧回合的重试/编辑都生成新的权威 turnId；旧回合保留在历史里并标记 supersededBy。
function retryTurn(turnId, prompt) {
  if (state.busy) { toast("请先停止当前对话"); return; }
  const text = String(prompt || "").trim();
  if (!text) { toast("找不到原提示词，请用编辑重试"); return; }
  toast(`正在重试第 ${turnId} 轮…`);
  return send(text, [], Number(turnId) || 0);
}

function editTurn(turnId, prompt) {
  if (state.busy) { toast("请先停止当前对话"); return; }
  state.pendingEditOf = Number(turnId) || 0;
  const input = $("input");
  if (input) {
    input.value = String(prompt || "");
    if (typeof input.focus === "function") input.focus();
  }
  toast("修改提示词后按发送，即为编辑重试");
}

async function send(text, attachmentsOverride, retryOfOverride) {
  if (state.busy) return;
  text = String(text || "").trim();
  if (!text) return;

  // 先夺取本回合身份。stop() 与新 send 都会推进 generation，旧流里的
  // done/error/tool 即便晚到几分钟也过不了 ownsEvent，无法清掉新回合状态。
  const generation = ++state.generation;
  state.activeTurn = 0;
  state.lastUndo = null;   // 新回合不继承旧回合的可撤销承诺
  const retryOf = Number(retryOfOverride) || Number(state.pendingEditOf) || 0;
  state.pendingEditOf = 0;

  // A queued message carries its own attachment snapshot. A live send reads
  // the current chip selection, then clears it so paths are not sent twice.
  // Clearing the input here would erase anything the user typed while the
  // previous answer was streaming, so input ownership stays with submitInput().
  const queued = Array.isArray(attachmentsOverride);
  const attachments = queued ? attachmentsOverride : state.attachments.slice();
  if (!queued) {
    state.attachments = [];
    syncAttachChip();
  }
  appendUser(text, Date.now());
  const assistant = appendAssistant("", Date.now(), null);
  assistant._text = "";
  if (assistant.setPrompt) assistant.setPrompt(text);
  // 活动流的回合提示词：本轮第一个 tool 事件到达时由 recordActivity 消费。
  state.activity.pendingPrompt = text;
  setBusy(true);
  setStatus("正在理解意图…");
  postHostStatus("busy", "正在理解意图…", "", false, "", 0);

  const ctrl = new AbortController();
  state.abort = ctrl;

  // Use the context snapshot already held in state. A blocking /api/context
  // round-trip here delays the first token, and if it fails the turn cannot
  // start at all even though /api/chat itself is perfectly healthy.
  const payload = {
    sessionId: state.sessionId,
    text,
    retryOf,
    // Legacy field: an older service build ignores `mode` and reads this. It is
    // kept in sync with the 3-state mode so behaviour degrades gracefully.
    autoApprove: state.autoApprove,
    mode: state.mode,
    attachments,
    tabPath: String(state.contextFolder || ""),
    listerIndex: Number.isFinite(state.contextListerIndex) ? state.contextListerIndex : -1
  };

  let aborted = false;
  let terminal = false;      // saw a `done` or `error` event
  try {
    const res = await apiFetch("/api/chat", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
      signal: ctrl.signal
    });
    if (!res.ok || !res.body) {
      const msg = res.status === 401
        ? "本地令牌缺失或已过期，请从 AI 面板打开页面"
        : `HTTP ${res.status}`;
      throw new Error(msg);
    }

    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buf = "";
    let lastId = state.lastEventId;
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buf = (buf + decoder.decode(value, { stream: true })).replace(/\r\n/g, "\n");
      let idx;
      while ((idx = buf.indexOf("\n\n")) >= 0) {
        const block = buf.slice(0, idx);
        buf = buf.slice(idx + 2);
        const lines = block.split("\n");
        // One SSE frame may carry `event:`, `id:` and SEVERAL `data:` lines.
        // Join them exactly like the backend parser does instead of silently
        // dropping everything after the first data line.
        const dataLines = lines
          .filter((l) => l.startsWith("data:"))
          .map((l) => l.slice(5).replace(/^ /, ""));
        const idLine = lines.find((l) => l.startsWith("id:"));
        if (idLine) lastId = Number(idLine.slice(3).trim()) || lastId;
        if (!dataLines.length) continue;   // ": keep" / "retry:" frames
        try {
          const ev = JSON.parse(dataLines.join("\n"));
          if (ev.type === "done" || ev.type === "error") terminal = true;
          handleEvent(ev, assistant, generation);
        } catch (e) { }
      }
    }
    if (generation === state.generation) state.lastEventId = lastId;

    // A healthy stream always ends with `done` or `error`. Reaching EOF without
    // one means a proxy cut the connection (or the service died) and the text we
    // have is truncated: say so instead of presenting it as a finished answer.
    // 旧回合已被 stop/新 send 夺权时，连 EOF 提示也不许写到旧气泡上。
    if (!terminal && !aborted && generation === state.generation) {
      assistant.finish((assistant._text || "") + "\n\n⚠ 连接意外中断，回复可能不完整，请重试。");
      assistant.bubble.style.color = "var(--amber)";
      postHostStatus("error", "连接意外中断", "回复可能不完整，请重试", false, "", state.activeTurn);
    }
  } catch (e) {
    if (e.name === "AbortError") aborted = true;
    else if (generation === state.generation) {
      const prefix = assistant._text ? assistant._text + "\n\n" : "";
      assistant.finish(prefix + `⚠ 无法连接本地服务：${e.message}`);
      assistant.bubble.style.color = "var(--amber)";
      postHostStatus("error", "无法连接本地服务", String(e.message || ""), false, "", state.activeTurn);
    }
  } finally {
    // Only the turn that still owns state.abort may tear down the UI. A fast
    // user can stop one turn and start the next before this one's reader has
    // fully unwound; an unguarded setBusy(false) would flip the NEW turn back
    // to idle and orphan its AbortController.
    if (generation === state.generation && state.abort === ctrl) {
      setBusy(false);
      setStatus("");
      assistant.bubble.classList.remove("typing");
      clearApprovals();
      state.abort = null;
      // A user-initiated stop should not silently fire the next queued message.
      if (!aborted) flushQueue();
    }
  }
}

// Send the oldest queued message, if the previous turn has fully settled.
function flushQueue() {
  if (state.busy || !state.queue.length) return;
  const item = state.queue.shift();
  send(item.text, item.attachments || []);
}

function stop() {
  // 先夺权再 abort：旧流随后到达的任何事件都会被 ownsEvent 丢弃，
  // 它的 finally 也不会再碰 busy/审批/队列。
  state.generation++;
  const ctrl = state.abort;
  state.abort = null;
  if (ctrl) ctrl.abort();
  setBusy(false);
  setStatus("");
  postHostStatus("done", "已停止", "本轮回答已由你停止", false, "", state.activeTurn);
  toast("已停止");
}

// ---------------------------------------------------------------------------
// model dropdown + custom model dialog
// ---------------------------------------------------------------------------

function renderModelMenu() {
  const wrap = $("modelGroups");
  const q = ($("modelSearch").value || "").trim().toLowerCase();
  const favKeys = favoriteKeySet();
  // "只看收藏" narrows the source BEFORE the text filter, so searching inside
  // the pinned list still behaves like an ordinary search.
  const source = state.models.favOnly
    ? state.models.list.filter((m) => favKeys.has(modelKey(m)))
    : state.models.list;
  const list = source.filter((m) => !q ||
    String(m.id).toLowerCase().includes(q) ||
    String(m.provider || "").toLowerCase().includes(q) ||
    String(m.keyHint || "").toLowerCase().includes(q));
  const empty = $("modelMenuEmpty");
  empty.hidden = list.length > 0;
  empty.textContent = state.models.favOnly && !state.models.favorites.length
    ? "还没有收藏模型 · 点模型行右侧的 ☆ 收藏"
    : "没有匹配的模型";
  const errorBox = $("modelMenuError");
  errorBox.textContent = state.models.switchError;
  errorBox.hidden = !state.models.switchError;

  // Identity is (protocol, baseUrl, id), not id alone. Two relays may expose
  // the same id under the same vendor name; only then do we add a small
  // discriminator, so the usual one-provider list stays uncluttered.
  const dupIds = new Set();
  const seenIds = new Set();
  for (const m of list) {
    if (seenIds.has(m.id)) dupIds.add(m.id);
    seenIds.add(m.id);
  }

  // Group by service provider, not by interface address. The server resolves
  // modelList[].provider to a vendor name; a provider may own several
  // endpoints, but every row still carries its own baseUrl for switching.
  const groups = new Map();
  for (const m of list) {
    const key = m.provider || "自定义接口";
    if (!groups.has(key)) groups.set(key, { name: key, items: [] });
    groups.get(key).items.push(m);
  }

  const savedScroll = wrap.scrollTop;
  // Keep the group holding the active model on top, and the active row first in
  // that group. With a large /models catalogue the chip's own model used to be
  // buried somewhere below the fold.
  const ordered = [...groups.values()].sort((a, b) =>
    Number(b.items.some((m) => m.active)) - Number(a.items.some((m) => m.active)));
  for (const g of ordered) g.items.sort((a, b) => Number(b.active) - Number(a.active));

  // Special sections sit above the provider groups. Both are hidden while
  // filtering: duplicate rows in search results are noise, and every match
  // must remain visible. Favorites are removed from "最近使用" so a pinned
  // model is not printed twice on every open.
  const byKey = new Map(state.models.list.map((m) => [modelKey(m), m]));
  const specials = [];
  if (!q && !state.models.favOnly) {
    const fav = favoriteRows();
    if (fav.length) specials.push({ name: "收藏", items: fav.map((x) => x.m), favorite: true, hotkeys: true });
    const favSet = new Set(fav.map((x) => modelKey(x.m)));
    const recent = state.models.recent
      .map((r) => byKey.get(modelKey(r)))
      .filter((m) => m && !favSet.has(modelKey(m)));
    if (recent.length) specials.push({ name: "最近使用", items: recent, recent: true });
  }
  const sections = state.models.favOnly
    ? (list.length ? [{ name: "收藏", items: list, favorite: true, hotkeys: true }] : [])
    : [...specials, ...ordered];

  // Large /models catalogues (300+ rows) collapse non-active provider groups
  // by default. Searching always expands everything so a collapsed group can
  // never hide a match. The active group is never collapsible.
  const canCollapse = !q;
  const bigCatalog = canCollapse && list.length >= 300;
  const collapsedFor = (g) => !g.recent && !g.favorite && canCollapse &&
    !g.items.some((m) => m.active) &&
    (state.models.menuCollapsed.has(g.name) ||
      (bigCatalog && !state.models.menuExpanded.has(g.name)));

  // The star is a SIBLING of the row button on purpose: nesting a button in a
  // button breaks keyboard/AT semantics and makes a star click switch models.
  const rowHtml = (m, hotkey) => {
    const dup = dupIds.has(m.id);
    let host = "";
    if (dup) {
      try { host = new URL(m.baseUrl).hostname; }
      catch (e) { host = String(m.baseUrl || ""); }
    }
    const tag = dup ? (m.keyHint || protocolLabel(m.protocol) || "接口") : "";
    const fav = favKeys.has(modelKey(m));
    const attrs = `data-model="${escapeHtml(m.id)}" data-base="${escapeHtml(m.baseUrl)}" data-protocol="${escapeHtml(m.protocol || "openai")}"`;
    const favLabel = fav ? "取消收藏" : "收藏（Alt+数字快速切换）";
    return `<div class="model-row">` +
      `<button class="model-item${m.active ? " on" : ""}" ${attrs} title="${escapeHtml(dup ? `${m.id} · ${host} · ${protocolLabel(m.protocol)}` : m.id)}">` +
      `<span class="mi-dot"></span><span class="mi-name">${escapeHtml(m.id)}</span>` +
      (dup ? `<span class="mi-tag">${escapeHtml(tag)}</span>` : "") +
      (hotkey ? `<span class="mi-hotkey" title="Alt+${hotkey} 快速切换">${hotkey}</span>` : "") +
      `${m.active ? icon("check", "mi-check") : ""}</button>` +
      `<button class="mi-star${fav ? " on" : ""}" ${attrs} data-fav="1" ` +
      `title="${escapeHtml(favLabel)}" aria-label="${escapeHtml(favLabel + " " + m.id)}" aria-pressed="${fav}">` +
      `${icon("star")}</button></div>`;
  };

  let html = "";
  for (const g of sections) {
    const collapsed = collapsedFor(g);
    html += `<div class="model-group${g.recent ? " recent" : ""}${g.favorite ? " favorite" : ""}${collapsed ? " collapsed" : ""}">`;
    if (g.recent || g.favorite || !canCollapse) {
      html += `<div class="model-group-title static">` +
        `<span class="model-group-caret">${g.recent || g.favorite ? "·" : "▾"}</span>` +
        `<span>${escapeHtml(g.name)}</span>` +
        `<span class="model-group-count">${g.items.length}</span></div>`;
    } else {
      html += `<button class="model-group-title" data-group="${escapeHtml(g.name)}" aria-expanded="${!collapsed}">` +
        `<span class="model-group-caret">▾</span>` +
        `<span>${escapeHtml(g.name)}</span>` +
        `<span class="model-group-count">${g.items.length}</span></button>`;
    }
    if (!collapsed) html += g.items.map((m) => rowHtml(m, g.hotkeys ? favoriteIndex(m) : 0)).join("");
    html += `</div>`;
  }
  wrap.innerHTML = html;
  // Restore the keyboard cursor after a re-render (config refresh, star
  // toggle, group collapse). Match on the model identity, not on row index.
  if (state.models.menuCursorKey) {
    const cur = [...wrap.querySelectorAll(".model-item")].find(
      (b) => modelKey({ id: b.dataset.model, baseUrl: b.dataset.base,
        protocol: b.dataset.protocol }) === state.models.menuCursorKey);
    if (cur) cur.classList.add("cursor");
  }
  // loadConfig() re-renders on focus / visibility / every config write. Without
  // restoring scrollTop, browsing a long catalogue jumps back to the top.
  wrap.scrollTop = savedScroll;
  wrap.querySelectorAll(".model-item").forEach((b) => {
    b.addEventListener("click", (e) => {
      e.stopPropagation();
      switchModel(b.dataset.model, b.dataset.base, b.dataset.protocol);
    });
  });
  wrap.querySelectorAll(".mi-star").forEach((b) => {
    b.addEventListener("click", (e) => {
      e.stopPropagation();
      const m = { id: b.dataset.model, baseUrl: b.dataset.base, protocol: b.dataset.protocol };
      toggleFavorite(m);
    });
  });
  wrap.querySelectorAll(".model-group-title[data-group]").forEach((b) => {
    b.addEventListener("click", (e) => {
      e.stopPropagation();
      const name = b.dataset.group;
      const wasCollapsed = b.getAttribute("aria-expanded") === "false";
      if (wasCollapsed) {
        state.models.menuCollapsed.delete(name);
        state.models.menuExpanded.add(name);
      } else {
        state.models.menuCollapsed.add(name);
        state.models.menuExpanded.delete(name);
      }
      renderModelMenu();
    });
  });
  if (!$("modelPopover").hidden) positionModelMenu();
}

async function switchModel(model, baseUrl, protocol) {
  const item = state.models.list.find((m) => m.id === model && m.baseUrl === baseUrl &&
    (m.protocol || "openai") === (protocol || "openai"));
  if (item && item.active) { closeModelMenu(); return; }
  if (state.models.switching) return;
  state.models.switching = true;
  const menu = $("modelPopover");
  menu.classList.add("switching");
  setModelSwitchError("");
  try {
    await postJson("/api/config/model", { model, baseUrl, protocol: protocol || "openai" });
    await loadConfig();                       // toasts the model change
    const active = state.models.list.find((m) => m.active) || item ||
      { id: model, baseUrl, protocol: protocol || "openai" };
    rememberModel(active);
    // "只看收藏" must not hide the model the user just switched to.
    if (state.models.favOnly && !favoriteKeySet().has(modelKey(active))) setFavOnly(false);
    else renderModelMenu();                   // refresh the recent group
    setModelSwitchError("");
    closeModelMenu();
    loadHealth();
  } catch (e) {
    const message = "切换失败：" + e.message;
    setModelSwitchError(message);             // keep the list open for a retry
    toast(message);
  } finally {
    state.models.switching = false;
    menu.classList.remove("switching");
  }
}

// ---------------------------------------------------------------------------
// custom model dialog: vendor / endpoint / key / model-id (four rows)
// ---------------------------------------------------------------------------

const CM_IDLE = "填写接口地址与 API Key，点击「验证」通过后才可保存。";

function normBase(s) {
  return String(s == null ? "" : s).trim().replace(/\/+$/, "").toLowerCase();
}

function currentForm() {
  return {
    vendor: $("cmVendor").value.trim(),
    baseUrl: $("cmBase").value.trim(),
    protocol: $("cmProtocol").value || "openai",
    key: $("cmKey").value.trim(),
    modelId: $("cmModelId").value.trim()
  };
}

function protocolLabel(id) {
  const p = protocolList().find((x) => x.id === id);
  return p ? p.label : (id === "anthropic" ? "Anthropic Messages" : "OpenAI 兼容");
}

function presetIdByName(name) {
  const s = String(name || "").trim().toLowerCase();
  const v = vendorPresets().find((x) => String(x.name).toLowerCase() === s);
  return v ? v.id : "";
}

// Which saved provider matches the form AS IT IS RIGHT NOW? Deriving key state
// from (protocol, baseUrl) instead of `selectedProvider` matters: after picking
// a saved vendor the user may edit the endpoint or protocol, and the service
// would then have no key for the new address. Keeping the old hint would claim
// "留空即复用" and produce a confusing validation failure.
function currentSavedProvider() {
  const f = currentForm();
  return (state.models.providers || []).find((p) =>
    normBase(p.baseUrl) === normBase(f.baseUrl) &&
    (p.protocol || "openai") === f.protocol) || null;
}

// The key the dialog is currently allowed to replace. Only an explicit edit
// carries one; "add" never does, which is what makes the save unambiguous.
function activeEditKey() {
  const s = state.models;
  return s.mode === "edit" && s.editKey ? s.editKey : "";
}

function savedKeyForForm() {
  const p = currentSavedProvider();
  if (p) return { hasKey: !!p.hasKey, keyHint: p.keyHint || "••••" };
  // Editing a saved provider whose endpoint is being changed: the secret
  // still belongs to that provider (the server resolves it through editKey),
  // so "留空即复用" stays honest instead of demanding a re-typed key.
  const s = state.models;
  if (s.mode === "edit" && s.editKey && s.selectedProvider) {
    return { hasKey: !!s.selectedProvider.hasKey,
      keyHint: s.selectedProvider.keyHint || "••••" };
  }
  const f = currentForm();
  const c = state.config || {};
  // The active endpoint may have a key in aichat.json even before any provider
  // is saved; keyForProvider() on the server falls back to it as well.
  if (c.hasApiKey && normBase(c.baseUrl) === normBase(f.baseUrl) &&
      (c.protocol || "openai") === f.protocol) {
    return { hasKey: true, keyHint: c.keyHint || "••••" };
  }
  return { hasKey: false, keyHint: "" };
}

// The signature covers exactly what the server's validation receipt is keyed
// on (protocol + endpoint + key). The vendor name is cosmetic, so renaming
// it does not force a re-validation.
function keyModeLabel() {
  const f = currentForm();
  if (f.key) return "typed:" + f.key;
  const saved = savedKeyForForm();
  return saved.hasKey ? "saved:" + saved.keyHint : "none";
}

function formSignature() {
  const f = currentForm();
  return [f.protocol, normBase(f.baseUrl), keyModeLabel()].join("\u0000");
}

function setValidation(status, detail) {
  const v = state.models.validation;
  v.status = status;
  v.detail = detail || "";
  v.signature = status === "ok" ? formSignature() : "";

  $("cmLamp").className = "cm-lamp " + status;
  $("cmLamp").title = {
    idle: "未验证", validating: "正在验证", ok: "验证通过", fail: "验证失败"
  }[status] || "未验证";
  // A brand-new endpoint is not saved yet, so /api/config/providers/models
  // would refuse the add. Do not promise "直接添加模型" in that case.
  const alreadySaved = !!currentSavedProvider();
  const texts = {
    idle: CM_IDLE,
    validating: "正在验证接口地址、协议与 API Key…",
    ok: alreadySaved
      ? "验证通过 · 可以保存服务商，也可以直接添加模型。"
      : "验证通过 · 请先点「保存」，再添加模型。",
    fail: "验证失败：" + (v.detail || "接口不可用")
  };
  const st = $("cmStatus");
  st.className = "cm-status " + (status === "idle" ? "" : status);
  st.textContent = texts[status] || CM_IDLE;
  $("cmSave").disabled = status !== "ok";

  // The main window's LED describes the ACTIVE endpoint, not whichever
  // provider happens to be in the dialog. Only light it up for the active one;
  // otherwise validating a second provider would falsely imply the chat is
  // now configured to use it.
  if (status === "ok") {
    const f = currentForm();
    const c = state.config || {};
    if (normBase(f.baseUrl) === normBase(c.baseUrl) &&
        f.protocol === (c.protocol || "openai")) {
      const conn = $("connStatus");
      conn.className = "conn ok";
      conn.textContent = state.health && state.health.dryRun ? "dry run" : "服务已连接";
    }
  }
}

function invalidateValidation() {
  const v = state.models.validation;
  if (v.status === "validating" || v.status === "idle") return;
  if (v.status !== "ok" || v.signature !== formSignature()) setValidation("idle");
}

function renderProtocolOptions() {
  const sel = $("cmProtocol");
  const cur = sel.value || (state.config && state.config.protocol) || "openai";
  const list = protocolList();
  sel.innerHTML = list.map((p) =>
    `<option value="${escapeHtml(p.id)}">${escapeHtml(p.label)}</option>`).join("");
  sel.value = list.some((p) => p.id === cur) ? cur : list[0].id;
}

function vendorMark(v, extraClass) {
  const known = VENDOR_MARKS[String(v.id || "").toLowerCase()];
  if (known) {
    return `<span class="cm-vendor-mark${extraClass || ""}" style="background:${escapeHtml(known.color)}">${escapeHtml(known.mark)}</span>`;
  }
  const initial = String(v.name || "?").trim().charAt(0).toUpperCase() || "?";
  return `<span class="cm-vendor-mark${extraClass || ""}" style="background:#5a6470">${escapeHtml(initial)}</span>`;
}

// Flat list on purpose: no placeholder row and no group captions. Presets come
// first, saved providers follow, and "自定义" sits at the very bottom.
function renderVendorOptions() {
  const presets = vendorPresets();
  let html = presets.map((v) =>
    `<button type="button" class="cm-vendor-item" role="option" data-vendor="preset:${escapeHtml(v.id)}" title="${escapeHtml(v.baseUrl)}">` +
      vendorMark(v) +
      `<span class="cm-vendor-name">${escapeHtml(v.name)}</span>` +
      `<span class="cm-vendor-base">${escapeHtml(v.baseUrl)}</span>` +
    `</button>`).join("");

  const saved = state.models.providers || [];
  if (saved.length) {
    // Two relays may legitimately share a display name at different endpoints.
    // The base URL is always shown; when names collide, append the masked key
    // hint so the two rows are still distinguishable at a glance.
    const nameCounts = new Map();
    for (const p of saved) {
      const n = String(p.vendor || p.name || "").trim().toLowerCase();
      nameCounts.set(n, (nameCounts.get(n) || 0) + 1);
    }
    html += `<div class="cm-vendor-sep"></div>` + saved.map((p) => {
      const n = String(p.vendor || p.name || "").trim().toLowerCase();
      const dup = nameCounts.get(n) > 1;
      const detail = escapeHtml(p.baseUrl) +
        (dup && p.keyHint ? ` · ${escapeHtml(p.keyHint)}` : "");
      // A synthetic row is an unsaved live endpoint: it has no stable key yet.
      // Use an explicit sentinel rather than falling back to a drifting index.
      const pick = p.key || "";
      return `<button type="button" class="cm-vendor-item" role="option" data-vendor="saved:${pick ? escapeHtml(pick) : "__active__"}" title="${escapeHtml(p.baseUrl)}">` +
        `<span class="cm-vendor-mark saved"><svg><use href="#i-key"/></svg></span>` +
        `<span class="cm-vendor-name">${escapeHtml(p.vendor || p.name)}</span>` +
        `<span class="cm-vendor-base">${detail}</span>` +
      `</button>`;
    }).join("");
  }

  html += `<div class="cm-vendor-sep"></div>` +
    `<button type="button" class="cm-vendor-item custom" role="option" data-vendor="custom">` +
      `<span class="cm-vendor-mark custom-mark"><svg><use href="#i-plus"/></svg></span>` +
      `<span class="cm-vendor-name">自定义 / 手动输入</span>` +
    `</button>`;

  $("cmVendorMenu").innerHTML = html;
}

// The list is a fixed overlay: it follows the combo box instead of adding any
// height to the dialog. Flip upward when there is not enough room below.
function positionVendorMenu() {
  const menu = $("cmVendorMenu");
  if (menu.hidden) return;
  const r = $("cmVendorBox").getBoundingClientRect();
  const h = menu.offsetHeight || 228;
  const below = r.bottom + 2;
  const top = (below + h <= window.innerHeight - 4 || r.top - h - 2 < 0)
    ? below : r.top - h - 2;
  menu.style.left = Math.round(r.left) + "px";
  menu.style.top = Math.round(top) + "px";
  menu.style.width = Math.round(r.width) + "px";
}

function toggleVendorMenu(force) {
  const menu = $("cmVendorMenu");
  const btn = $("cmVendorBtn");
  const open = (force == null) ? menu.hidden : !!force;
  menu.hidden = !open;
  btn.classList.toggle("open", open);
  $("cmVendor").setAttribute("aria-expanded", String(open));
  if (open) positionVendorMenu();
}

function closeVendorMenu() {
  toggleVendorMenu(false);
}

function renderKeyHint() {
  const f = currentForm();
  const saved = savedKeyForForm();
  const hint = $("cmKeyHint");
  if (f.key) {
    hint.textContent = "将使用新填写的 Key";
  } else if (saved.hasKey) {
    hint.innerHTML = `已保存 ${escapeHtml(saved.keyHint)} · ` +
      `<span class="cm-key-replace" id="cmKeyReplace">更换 Key</span>`;
  } else {
    hint.textContent = "新服务商必须填写";
  }
  $("cmKey").placeholder = (saved.hasKey && !f.key)
    ? `已保存 ${saved.keyHint}（留空即复用）` : "sk-…";
}

function updateFoundSelection() {
  const sel = state.models.foundSelected;
  const rows = [...$("cmFoundList").querySelectorAll(".cm-found-row")];
  const visible = rows.filter((r) => !r.hidden);
  let hiddenSelected = 0;
  for (const row of rows) {
    const cb = row.querySelector("input");
    if (!cb) continue;
    row.classList.toggle("checked", cb.checked);
    if (row.hidden && cb.checked) hiddenSelected++;
  }
  // Filtering is a view, not a selection change: hidden rows keep their ticks.
  // Say so instead of silently adding models the user cannot currently see.
  $("cmFoundCount").textContent = `已选 ${sel.size} 个` +
    (hiddenSelected ? `（含隐藏 ${hiddenSelected}）` : "");
  $("cmFoundSelectAll").textContent = visible.length > 0 &&
    visible.every((r) => { const cb = r.querySelector("input"); return cb && cb.checked; })
    ? "清空" : "全选";
}

function clearFound() {
  state.models.found = [];
  state.models.foundTotal = 0;
  state.models.foundSelected.clear();
  state.models.foundFilter = "";
  $("cmFound").hidden = true;
  $("cmFoundList").innerHTML = "";
  $("cmFoundCount").textContent = "已选 0 个";
  $("cmFoundSelectAll").textContent = "全选";
  $("cmFoundTitle").textContent = "可添加的模型";
  $("cmFoundEmpty").hidden = true;
}

// Live filtering straight from the model-ID field. Comma/space separated terms
// are OR'ed so "gpt, claude" shows both families. The field doubles as a
// filter box and a manual-add box; addModels() keeps those two roles apart.
function filterFound(raw) {
  const q = String(raw == null ? "" : raw).trim().toLowerCase();
  state.models.foundFilter = q;
  const list = $("cmFoundList");
  const rows = [...list.querySelectorAll(".cm-found-row")];
  if (!rows.length) return; // no probe result yet
  const terms = q.split(/[\s,，]+/).filter(Boolean);
  let visible = 0;
  for (const row of rows) {
    const cb = row.querySelector("input");
    const id = String((cb && cb.dataset.id) || "").toLowerCase();
    const match = !terms.length || terms.some((t) => id.indexOf(t) >= 0);
    row.hidden = !match;
    if (match) visible++;
  }
  const empty = $("cmFoundEmpty");
  if (empty) empty.hidden = visible !== 0;
  const total = state.models.foundTotal || state.models.found.length;
  const shown = state.models.found.length;
  const clipped = total > shown ? `（仅列出前 ${shown} 个）` : "";
  $("cmFoundTitle").textContent = terms.length
    ? `可添加的模型 · 匹配 ${visible} / 共 ${total} 个${clipped}`
    : `可添加的模型 · 共 ${total} 个${clipped}`;
  updateFoundSelection();
}

// One model per row, each with its own checkbox, so a probe result can be
// added in one batch instead of clicking chips one at a time.
function renderFound(models) {
  const all = Array.isArray(models) ? models : [];
  // An empty probe result used to leave a blank box with a stale title. Hide
  // the section entirely and let the caller say "fill in the model ID".
  if (!all.length) { clearFound(); return; }
  // Keep the true upstream count for the header. Only the first 200 rows are
  // rendered (the list is a small scroll area), and silently reporting 200 as
  // the total contradicted the "找到 N 个模型" toast.
  state.models.foundTotal = all.length;
  state.models.found = all.slice(0, 200);
  state.models.foundSelected.clear();
  $("cmFoundList").innerHTML = state.models.found.map((id) =>
    `<label class="cm-found-row" title="${escapeHtml(id)}">` +
      `<input type="checkbox" data-id="${escapeHtml(id)}" />` +
      `<span class="cm-found-id">${escapeHtml(id)}</span>` +
    `</label>`).join("");
  $("cmFound").hidden = false;
  filterFound($("cmModelId").value);
}

function applyPreset(v) {
  // A saved provider schedules an auto-validation on switch. Selecting a
  // preset right afterwards must cancel it, or it fires against the new form.
  clearTimeout(state.models.validateTimer);
  state.models.selectedProvider = null;
  state.models.editKey = null;
  state.models.mode = "add";
  $("cmVendor").value = v.name;
  $("cmBase").value = v.baseUrl;
  renderProtocolOptions();
  $("cmProtocol").value = v.protocol;
  $("cmKey").value = "";
  $("cmBaseHint").textContent = "已按服务商预设填入，可修改";
  setValidation("idle");
  renderKeyHint();
  clearFound();
  $("cmKey").focus();
}

// The combo box must say WHAT a save will do. A stable key makes the intent
// explicit: editing a saved provider replaces exactly that key, even if the
// URL was changed. Warn on a changed URL so "I meant to add a new one" cannot
// silently become a replacement of the provider that was loaded.
function renderModeHint() {
  const s = state.models;
  if (s.mode !== "edit" || !s.editKey) return;
  const p = s.selectedProvider;
  const label = (p && (p.vendor || p.name)) || s.editKey;
  const orig = p ? normBase(p.baseUrl) : "";
  const cur = normBase($("cmBase").value);
  $("cmBaseHint").textContent = orig && cur && cur !== orig
    ? `正在编辑「${label}」· 地址已改，保存会替换该服务商；要新增请点「新增服务商」`
    : `正在编辑「${label}」· 保存会更新这个服务商`;
}

function applySaved(p) {
  state.models.selectedProvider = p;
  // A saved provider has a stable key; a synthetic (unsaved live endpoint)
  // does not, so picking it must stay in "add" mode.
  state.models.editKey = p.key || null;
  state.models.mode = p.key ? "edit" : "add";
  $("cmVendor").value = p.vendor || p.name || "";
  $("cmBase").value = p.baseUrl;
  renderProtocolOptions();
  $("cmProtocol").value = p.protocol || "openai";
  $("cmKey").value = "";
  $("cmBaseHint").textContent = "已载入保存的接口地址";
  renderModeHint();
  renderKeyHint();
  setValidation("idle");
  clearFound();
  // Switching to a saved provider immediately validates with the saved key.
  // The key is never sent back to the UI: useSavedKey makes the server reuse
  // the real value from aichat.json.
  scheduleAutoValidate();
}

// Explicit "add a brand-new provider" action. Without this the dialog opens on
// the active provider (applySaved -> edit mode), so simply typing another
// vendor/URL into the same form looked like an edit and the server replaced the
// old provider. Resetting the form makes "new provider" an unambiguous intent.
function resetProviderForm() {
  clearTimeout(state.models.validateTimer);
  closeVendorMenu();
  state.models.selectedProvider = null;
  state.models.editKey = null;
  state.models.mode = "add";
  $("cmVendor").value = "";
  $("cmBase").value = "";
  $("cmKey").value = "";
  $("cmModelId").value = "";
  renderProtocolOptions();
  $("cmProtocol").value = "openai";
  $("cmBaseHint").textContent = "新增服务商：填写接口地址与 API Key";
  renderKeyHint();
  setValidation("idle");
  clearFound();
  $("cmVendor").focus();
}

function scheduleAutoValidate() {
  clearTimeout(state.models.validateTimer);
  state.models.validateTimer = setTimeout(() => validateKey(true), 300);
}

async function validateKey(auto) {
  clearTimeout(state.models.validateTimer);
  const f = currentForm();
  if (!/^https?:\/\//i.test(f.baseUrl)) {
    if (!auto) setValidation("fail", "接口地址无效：请填写 http(s):// 开头的 URL");
    else setValidation("idle");
    return;
  }
  if (!f.key && !savedKeyForForm().hasKey) {
    if (auto) { setValidation("idle"); return; }
    setValidation("fail", "请先填写 API Key，或选择一个已保存的服务商");
    return;
  }

  state.models.validating = true;
  const signature = formSignature();
  setValidation("validating");
  const btn = $("cmValidate");
  btn.disabled = true;
  // The model-ID field doubles as a live filter. When /models is unavailable
  // the server falls back to a one-token chat request, so a bare filter word
  // ("gpt") must never be sent as the model -- it would fail with a bogus
  // "model not found" and make a valid key look broken. Only pass an exact ID
  // from the probe result, or a manually typed ID when no probe list is shown.
  const rawModel = f.modelId.split(/[\s,，]+/)[0] || "";
  const listVisible = !$("cmFound").hidden && state.models.found.length > 0;
  const exact = state.models.found.some((id) =>
    String(id).toLowerCase() === rawModel.toLowerCase());
  const validateModel = (!listVisible || exact) ? rawModel : "";
  const editKey = activeEditKey();
  let r;
  try {
    const validateBody = {
      baseUrl: f.baseUrl,
      protocol: f.protocol,
      apiKey: f.key,
      useSavedKey: !f.key,
      vendorId: presetIdByName(f.vendor),
      model: validateModel
    };
    if (editKey) validateBody.editKey = editKey;
    r = await postJson("/api/models/validate", validateBody);
  } catch (e) {
    r = { ok: false, error: e.message };
  } finally {
    state.models.validating = false;
    btn.disabled = false;
  }

  // The user may have edited the form while the request was in flight. A
  // green light must never describe values that were not actually validated.
  if (formSignature() !== signature) {
    setValidation("idle");
    return;
  }

  if (!r.ok) {
    setValidation("fail", r.error + (r.detail ? " · " + r.detail : ""));
    if (!auto) toast("验证失败：" + (r.error || ""));
    return;
  }
  setValidation("ok");
  if (Array.isArray(r.models) && r.models.length) renderFound(r.models);
  if (!auto) toast(`验证通过（${r.ms} ms，${r.via === "models" ? "模型列表" : "最小请求"}）`);
}

async function saveProvider() {
  if (state.models.validation.status !== "ok") {
    toast("请先通过 API Key 验证");
    return;
  }
  const f = currentForm();
  // The key is the identity, not the name or the endpoint. It is sent only
  // when the user explicitly opened a saved provider (mode === "edit"); an
  // "add" never carries one, so the server can never replace another entry.
  // Editing the URL while in edit mode is allowed: the key still pins the
  // exact provider to replace, and cmBaseHint warns about the change.
  const editKey = activeEditKey();
  const isEdit = !!editKey;
  const payload = {
    vendor: f.vendor,
    baseUrl: f.baseUrl,
    protocol: f.protocol,
    apiKey: f.key,
    useSavedKey: !f.key
  };
  if (editKey) payload.editKey = editKey;
  try {
    await postJson("/api/config/providers", payload);
    await loadConfig({ silent: true });
    // Stay in "editing this provider" mode: the key is masked, URL is filled,
    // and models can be added without re-entering either one. Locate by the
    // key we just saved; fall back to the endpoint for a brand-new provider.
    const p = (state.models.providers || []).find((x) =>
      editKey ? (x.key || "") === editKey :
        (normBase(x.baseUrl) === normBase(f.baseUrl) &&
         (x.protocol || "openai") === f.protocol));
    if (p) {
      state.models.selectedProvider = p;
      state.models.editKey = p.key || null;
      state.models.mode = p.key ? "edit" : "add";
      $("cmKey").value = "";
      renderKeyHint();
      renderModeHint();
    }
    setValidation("ok");
    toast(isEdit
      ? `已保存服务商「${f.vendor || "未命名"}」，可继续添加模型`
      : `已新增服务商「${f.vendor || "未命名"}」，原有服务商保持不变`);
  } catch (e) {
    toast("保存失败：" + e.message);
    if (/验证/.test(e.message)) setValidation("idle");
  }
}

async function fetchModels() {
  const f = currentForm();
  if (!/^https?:\/\//i.test(f.baseUrl)) { toast("请先填写接口地址"); return; }
  const btn = $("cmFetch");
  const ico = btn.querySelector("svg");
  btn.disabled = true;
  ico.classList.add("spin");
  let r;
  try {
    const probeBody = {
      baseUrl: f.baseUrl, protocol: f.protocol,
      apiKey: f.key, useSavedKey: !f.key
    };
    const editKey = activeEditKey();
    if (editKey) probeBody.editKey = editKey;
    r = await postJson("/api/models/probe", probeBody);
  } catch (e) {
    r = { ok: false, error: e.message };
  } finally {
    btn.disabled = false;
    ico.classList.remove("spin");
  }
  if (!r.ok) {
    clearFound();
    toast("获取模型失败：" + (r.error || "") + (r.detail ? " · " + r.detail : "") + "。也可以直接填写模型 ID。");
    return;
  }
  const models = Array.isArray(r.models) ? r.models : [];
  if (!models.length) {
    clearFound();
    toast("该接口没有返回模型列表，请直接填写模型 ID");
    return;
  }
  renderFound(models);
  toast(`找到 ${models.length} 个模型，输入可过滤 · 勾选后点「添加」批量加入`);
}

async function addModels(forceManual) {
  // Checked rows win; the text field stays as the fallback for manually typed
  // IDs (comma/space separated, as before). Mixing both would silently add
  // models the user did not intend, so the two sources never merge.
  const raw = $("cmModelId").value.trim();
  const checked = [...state.models.foundSelected];
  const typed = raw.split(/[\s,，]+/).map((s) => s.trim()).filter(Boolean);
  const ids = [...new Set(checked.length ? checked : typed)];
  if (!ids.length) { toast("请勾选模型，或填写模型 ID"); return; }

  // A bare word typed while a probe list is visible is a filter term, not a
  // model ID. Refuse to create a bogus model from it. Enter in the field is
  // the explicit "add exactly what I typed" escape hatch (forceManual).
  const listVisible = !$("cmFound").hidden && state.models.found.length > 0;
  const isMulti = /[\s,，]/.test(raw);
  const exact = state.models.found.some((id) => String(id).toLowerCase() === raw.toLowerCase());
  if (!forceManual && !checked.length && listVisible && raw && !isMulti && !exact) {
    toast("这是过滤词：请勾选列表中的模型，或按 Enter 手动添加该 ID");
    return;
  }
  const f = currentForm();
  const saved = (state.models.providers || []).find((p) =>
    normBase(p.baseUrl) === normBase(f.baseUrl) &&
    (p.protocol || "openai") === f.protocol);
  if (!saved) {
    toast("请先验证并保存服务商，再添加模型");
    return;
  }
  try {
    await postJson("/api/config/providers/models", {
      action: "add", models: ids,
      baseUrl: f.baseUrl, protocol: f.protocol,
      apiKey: f.key, useSavedKey: !f.key
    });
    await loadConfig({ silent: true });
    $("cmModelId").value = "";
    if (checked.length) {
      state.models.foundSelected.clear();
      for (const cb of $("cmFoundList").querySelectorAll("input[data-id]")) cb.checked = false;
    }
    // The field may have been acting as a filter; clearing it must restore
    // the full list rather than leaving a stale filtered view behind.
    filterFound("");
    updateFoundSelection();
    toast(`已添加 ${ids.length} 个模型到「${f.vendor || saved.vendor || "该服务商"}」`);
  } catch (e) {
    toast("添加失败：" + e.message);
  }
}

function renderProviderGroups() {
  const ps = state.models.providers || [];
  const cfg = state.config || {};
  const wrap = $("cmGroups");
  const total = ps.reduce((n, p) => n + (p.models || []).length, 0);
  const enabled = ps.reduce((n, p) => n + Number(p.enabledCount != null
    ? p.enabledCount
    : (p.models || []).length - (p.disabledModels || []).length), 0);
  $("cmGroupsCount").textContent = total === enabled
    ? `${ps.length} 个服务商 · ${total} 个模型`
    : `${ps.length} 个服务商 · ${total} 个模型 · ${enabled} 启用`;
  if (!ps.length) {
    wrap.innerHTML = `<div class="cm-empty">还没有保存的服务商。验证通过并保存后，这里会按服务商分组显示模型。</div>`;
    return;
  }

  wrap.innerHTML = ps.map((p, i) => {
    const synthetic = !!p.synthetic;
    const key = (p.protocol || "openai") + "|" + p.baseUrl;
    const collapsed = state.models.collapsed.has(key);
    const isActive = normBase(p.baseUrl) === normBase(cfg.baseUrl) &&
      (p.protocol || "openai") === (cfg.protocol || "openai");
    const implicit = new Set(p.implicitModels || []);
    const off = new Set(p.disabledModels || []);
    const rows = (p.models || []).map((id) => {
      const current = isActive && id === cfg.model;
      // The active model can be visible in the catalogue without being written
      // into providers[].models (legacy config). Offer a register action
      // instead of edit/delete, which the server would reject for this id.
      const ghost = implicit.has(id);
      const test = state.models.testResults.get(key + "\u0000" + id);
      const on = !off.has(id);
      // The switch reads "is this model offered in the dropdown". The server
      // refuses to switch off the model in use and refuses ids it does not own,
      // so both cases render the box checked + locked instead of failing later.
      const lockTitle = current
        ? "当前生效的模型不能停用，请先切换到其它模型"
        : "未写入该服务商的模型清单，登记后才能切换启用状态";
      const box = `<input type="checkbox" class="cm-enable" data-enable` +
        (on ? " checked" : "") +
        ((current || ghost) ? ` disabled title="${lockTitle}"` : ` title="${on ? "已启用 · 点击停用（从模型列表隐藏）" : "已停用 · 点击启用"}"`) +
        ` aria-label="启用 ${escapeHtml(id)}" />`;
      const actions = ghost
        ? `<span class="cm-unregistered" title="当前生效但未写入该服务商的模型清单；切换后可能从列表消失">未登记</span>` +
          `<button class="cm-icon-btn" data-act="register" title="写入该服务商的模型清单">${icon("plus")}</button>`
        : `<button class="cm-icon-btn" data-act="edit" title="编辑模型 ID">${icon("edit")}</button>` +
          `<button class="cm-icon-btn" data-act="test" title="测试该模型">${icon("bolt")}</button>` +
          `<button class="cm-icon-btn danger" data-act="delete" title="删除该模型">${icon("close")}</button>`;
      return `<div class="cm-model-row${current ? " current" : ""}${ghost ? " implicit" : ""}${on ? "" : " off"}" data-provider="${i}" data-model="${escapeHtml(id)}">` +
        box +
        `<span class="cm-model-id" title="${escapeHtml(id)}">${escapeHtml(id)}</span>` +
        (current ? `<span class="cm-current-tag">当前</span>` : "") +
        (on ? "" : `<span class="cm-off-tag">已停用</span>`) +
        (test ? `<span class="cm-test-result ${test.ok ? "ok" : "fail"}" title="${escapeHtml(test.text)}">${escapeHtml(test.text)}</span>` : "") +
        `<span class="cm-model-actions">${actions}</span></div>`;
    }).join("") || `<div class="cm-empty">该服务商下还没有模型</div>`;

    return `<div class="cm-group${collapsed ? " collapsed" : ""}" data-key="${escapeHtml(key)}">` +
      `<div class="cm-group-head" data-toggle="${i}">` +
        icon("chevron", "cm-group-chev") +
        `<span class="cm-group-name">${escapeHtml(p.vendor || p.name || "未命名")}</span>` +
        `<span class="cm-group-host">${escapeHtml(p.baseUrl)}</span>` +
        `<span class="cm-badge">${escapeHtml(p.protocolLabel || protocolLabel(p.protocol))}</span>` +
        (synthetic
          ? `<span class="cm-badge unsaved" title="当前生效的接口还没有保存成服务商；点模型右侧 + 即可登记">未保存</span>`
          : `<button class="cm-group-remove" data-remove="${i}" title="移除该服务商">${icon("close")}</button>`) +
      `</div><div class="cm-model-list">${rows}</div></div>`;
  }).join("");
}

function startEditModel(p, id, row) {
  if (row.querySelector(".cm-edit-input")) return;
  const nameEl = row.querySelector(".cm-model-id");
  nameEl.hidden = true;
  const input = document.createElement("input");
  input.className = "cm-edit-input";
  input.value = id;
  input.spellcheck = false;
  nameEl.after(input);
  input.focus();
  input.select();

  let fired = false;
  const done = async (commit) => {
    if (fired) return;
    fired = true;
    const next = input.value.trim();
    input.remove();
    nameEl.hidden = false;
    if (!commit || !next || next === id) return;
    try {
      await postJson("/api/config/providers/models", {
        action: "edit", baseUrl: p.baseUrl, protocol: p.protocol,
        model: id, newModel: next
      });
      await loadConfig({ silent: true });
      toast(`模型已改名为 ${next}`);
    } catch (e) {
      toast("编辑失败：" + e.message);
    }
  };
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); done(true); }
    else if (e.key === "Escape") { e.preventDefault(); done(false); }
  });
  input.addEventListener("blur", () => done(true));
}

async function testModel(p, id) {
  const key = (p.protocol || "openai") + "|" + p.baseUrl + "\u0000" + id;
  state.models.testResults.set(key, { ok: true, text: "测试中…" });
  renderProviderGroups();
  let r;
  try {
    r = await postJson("/api/models/test", {
      baseUrl: p.baseUrl, protocol: p.protocol, model: id,
      apiKey: "", useSavedKey: true
    });
  } catch (e) {
    r = { ok: false, error: e.message };
  }
  state.models.testResults.set(key, {
    ok: !!r.ok,
    text: r.ok ? `✓ ${r.ms}ms` : `✗ ${String(r.error || "失败").slice(0, 40)}`
  });
  renderProviderGroups();
  toast(r.ok ? `「${id}」测试通过（${r.ms} ms）` : `「${id}」测试失败：${r.error || ""}`);
}

// Toggle a saved model's enabled state. Disabled models stay in the provider's
// catalogue (so nothing is lost) but are hidden from the model dropdown.
async function setModelEnabled(p, id, enabled) {
  try {
    await postJson("/api/config/providers/models", {
      action: enabled ? "enable" : "disable",
      baseUrl: p.baseUrl, protocol: p.protocol, model: id
    });
    await loadConfig({ silent: true });
    toast(enabled ? `已启用「${id}」` : `已停用「${id}」，模型列表不再显示它`);
  } catch (e) {
    toast((enabled ? "启用" : "停用") + "失败：" + e.message);
    renderProviderGroups();   // server refused -- put the checkbox back
  }
}

async function registerModel(p, id) {
  try {
    await postJson("/api/config/providers/models", {
      action: "add", models: [id], baseUrl: p.baseUrl,
      protocol: p.protocol, apiKey: "", useSavedKey: true
    });
    await loadConfig({ silent: true });
    toast(`已把「${id}」写入该服务商的模型清单`);
  } catch (e) {
    toast("登记失败：" + e.message);
  }
}

async function deleteModel(p, id) {
  try {
    await postJson("/api/config/providers/models", {
      action: "delete", baseUrl: p.baseUrl, protocol: p.protocol, model: id
    });
    await loadConfig({ silent: true });
    toast(`已删除「${id}」`);
  } catch (e) {
    toast("删除失败：" + e.message);
  }
}

async function removeProvider(p) {
  try {
    await postJson("/api/config/providers/remove", { baseUrl: p.baseUrl, protocol: p.protocol });
    // If the provider being edited was removed, drop its key so the next save
    // cannot try to replace a key that no longer exists.
    if (p.key && state.models.editKey === p.key) {
      state.models.editKey = null;
      state.models.selectedProvider = null;
      state.models.mode = "add";
    }
    await loadConfig({ silent: true });
    toast(`已移除服务商「${p.vendor || p.name}」`);
  } catch (e) {
    toast("移除失败：" + e.message);
  }
}

async function openModelModal() {
  closeModelMenu();
  closeModeMenu();
  // Refresh first: the four rows must reflect the provider actually in use.
  await loadConfig({ silent: true });
  const c = state.config || {};
  renderProtocolOptions();
  renderVendorOptions();
  clearFound();
  $("cmModelId").value = "";
  closeVendorMenu();
  state.models.testResults.clear();

  const active = (state.models.providers || []).find((p) => p.active);
  if (active) {
    applySaved(active);
  } else {
    state.models.editKey = null;
    state.models.mode = "add";
    // The active endpoint may have a key in aichat.json even before any
    // provider is saved. Let the user validate it without re-typing.
    state.models.selectedProvider = c.hasApiKey ? {
      vendor: c.vendor || "", baseUrl: c.baseUrl || "",
      protocol: c.protocol || "openai", hasKey: true,
      keyHint: c.keyHint, models: []
    } : null;
    $("cmVendor").value = c.vendor || "";
    $("cmBase").value = c.baseUrl || "";
    renderProtocolOptions();
    $("cmProtocol").value = c.protocol || "openai";
    $("cmKey").value = "";
    renderKeyHint();
    setValidation("idle");
  }
  // In edit mode applySaved already wrote a precise hint; do not overwrite it.
  if (!(state.models.mode === "edit" && state.models.editKey)) {
    $("cmBaseHint").textContent = "选择常见服务商会自动填充";
  }
  renderProviderGroups();
  // A stale service cannot serve /api/models/validate or the new provider
  // routes. Say so up front instead of letting the user hit a confusing 404.
  const stale = !state.models.serverHasCatalog;
  const summary = $("cmSummary");
  summary.classList.toggle("warn", stale);
  summary.textContent = stale
    ? "服务端为旧版本：常见服务商已用内置目录显示，但需重启 AgentChat 服务后才能验证 / 保存。"
    : "验证通过 → 保存服务商 → 直接添加模型，无需重填 URL 和 Key。";
  $("modelModal").hidden = false;
  setTimeout(() => $("cmVendor").focus(), 30);
}

function closeModelModal() {
  clearTimeout(state.models.validateTimer);
  closeVendorMenu();
  $("modelModal").hidden = true;
}

// ---------------------------------------------------------------------------
// wiring
// ---------------------------------------------------------------------------

function minInputHeight() {
  const cs = getComputedStyle($("input"));
  const lh = parseFloat(cs.lineHeight) || 16;
  return Math.ceil(lh * 3 + parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom));
}

function autoGrow() {
  const t = $("input");
  t.style.height = "auto";
  t.style.height = Math.max(minInputHeight(), Math.min(t.scrollHeight, 130)) + "px";
}

// Anchor the dropdown to the model chip: same left edge, sitting just above
// it. Fixed CSS offsets drift once the composer grows to three lines, and the
// catalogue length changes the popover height after the config reload.
function positionModelMenu() {
  const p = $("modelPopover");
  if (p.hidden) return;
  const r = $("modelBtn").getBoundingClientRect();
  const width = p.offsetWidth || 292;
  const gap = 6;
  const left = Math.max(4, Math.min(r.left, window.innerWidth - width - 4));
  p.style.left = Math.round(left) + "px";
  p.style.bottom = Math.round(window.innerHeight - r.top + gap) + "px";
}

// One place owns the popover's hidden + aria-expanded pair. Alt+M, Escape,
// outside clicks and the other overlays all go through it.
function closeModelMenu() {
  $("modelPopover").hidden = true;
  $("modelBtn").setAttribute("aria-expanded", "false");
}

function openModelMenu() {
  const p = $("modelPopover");
  closeModeMenu();
  closeComposerMenu();
  p.hidden = false;
  $("modelBtn").setAttribute("aria-expanded", "true");
  $("modelSearch").value = "";
  setModelSwitchError("");
  state.models.menuCursorKey = "";
  // Clear the previous filter synchronously. Waiting for loadConfig()'s async
  // re-render left the stale filtered list under an empty search box.
  renderModelMenu();
  positionModelMenu();
  $("modelSearch").focus();
  loadConfig();
}

// The star filter is a view of the SAME catalogue, not a second model source.
function setFavOnly(on) {
  state.models.favOnly = !!on;
  const b = $("modelFavOnly");
  if (b) {
    b.classList.toggle("on", state.models.favOnly);
    b.setAttribute("aria-pressed", String(state.models.favOnly));
  }
  renderModelMenu();
}

// ---------------------------------------------------------------------------
// composer controls: permission mode, context, enhance, slash commands, usage
// ---------------------------------------------------------------------------

const MODES = ["confirm", "auto", "readonly"];
const MODE_LABEL = { confirm: "确认后执行", auto: "自动（危险确认）", readonly: "只读 / 计划" };
const MODE_TITLE = {
  confirm: "所有会改状态的命令先弹出确认卡片",
  auto: "普通写入自动执行；危险命令仍需确认，并受 denyCommands 与宿主开关约束",
  readonly: "不执行任何修改，只给出方案与命令清单"
};

// 快捷指令分两类：
//   1) 直接发给 AI 的话术：命中 service/intents.js 的本地直调表，不经过模型推理；
//   2) 面板自身动作：清输入框、新对话、模型/模式菜单等，不产生聊天回合。
// 两类放在同一个 / 菜单里，用户不必先猜「这句话要不要走模型」。
function quickPrompt(text) {
  const t = $("input");
  t.value = String(text || "");
  autoGrow();
  submitInput();
}

const SLASH_COMMANDS = [
  // ---- 高频文件管理：本地意图直调 ----
  { id: "上一级", label: "回到上一级文件夹", run: () => quickPrompt("回到上一级文件夹") },
  { id: "后退", label: "后退到上一个位置", run: () => quickPrompt("后退") },
  { id: "前进", label: "前进到下一个位置", run: () => quickPrompt("前进") },
  { id: "刷新", label: "刷新当前文件夹与视图", run: () => quickPrompt("刷新当前文件夹") },
  { id: "主目录", label: "打开主目录", run: () => quickPrompt("打开主目录") },
  { id: "下载", label: "打开下载文件夹", run: () => quickPrompt("打开下载文件夹") },
  { id: "文档", label: "打开文档文件夹", run: () => quickPrompt("打开文档文件夹") },
  { id: "桌面", label: "打开桌面", run: () => quickPrompt("打开桌面") },
  { id: "此电脑", label: "打开此电脑", run: () => quickPrompt("打开此电脑") },
  { id: "详细信息", label: "切换为详细信息视图", run: () => quickPrompt("切换为详细信息") },
  { id: "大图标", label: "切换为大图标视图", run: () => quickPrompt("切换为大图标") },
  { id: "列表", label: "切换为列表视图", run: () => quickPrompt("切换为列表视图") },
  { id: "按名称", label: "按名称排序", run: () => quickPrompt("按名称排序") },
  { id: "按日期", label: "按日期排序", run: () => quickPrompt("按日期排序") },
  { id: "按类型", label: "按类型排序", run: () => quickPrompt("按类型排序") },
  { id: "按大小", label: "按大小排序", run: () => quickPrompt("按大小排序") },
  { id: "全选", label: "选中所有项目", run: () => quickPrompt("全选") },
  { id: "选文件", label: "仅选中文件", run: () => quickPrompt("选中所有文件") },
  { id: "选文件夹", label: "仅选中文件夹", run: () => quickPrompt("选中所有文件夹") },
  { id: "选图片", label: "选中所有图片", run: () => quickPrompt("选中所有图片") },
  { id: "新标签", label: "新建标签页", run: () => quickPrompt("新建标签") },
  { id: "关标签", label: "关闭当前标签页", run: () => quickPrompt("关闭当前标签页") },
  {
    id: "搜索",
    label: "搜索文件（填入关键词）",
    run: () => {
      const t = $("input");
      t.value = "搜索 ";
      autoGrow();
      closeComposerMenu();
      t.focus();
      t.setSelectionRange(t.value.length, t.value.length);
    }
  },

  // ---- 面板自身动作 ----
  { id: "清空", label: "清空输入框", run: () => { $("input").value = ""; autoGrow(); $("input").focus(); } },
  { id: "新对话", label: "新建对话", run: () => newChat() },
  { id: "刷新上下文", label: "刷新面板上下文（不发消息）", run: () => loadContext(true) },
  { id: "模型", label: "打开模型选择", run: () => $("modelBtn").click() },
  { id: "模式", label: "选择执行模式", run: () => openModeMenu() },
  { id: "上下文", label: "管理附加文件", run: () => openComposerMenu("ctx", $("attachBtn")) },
  { id: "用量", label: "查看 token 用量", run: () => toast(`本会话累计 ${fmtTokens(state.usage.total)} tokens（各轮用量之和）`) },
  { id: "停止", label: "停止当前回答", run: () => { if (state.busy) stop(); else toast("当前没有进行中的回答"); } }
];

function fmtTokens(n) {
  return n >= 1000 ? (n / 1000).toFixed(n >= 10000 ? 0 : 1) + "k" : String(n);
}

function updateMode() {
  const pill = $("modeBtn");
  pill.classList.toggle("auto", state.mode === "auto");
  pill.classList.toggle("readonly", state.mode === "readonly");
  $("modeText").textContent = MODE_LABEL[state.mode];
  pill.title = "当前：" + MODE_TITLE[state.mode] + " · 点击选择执行模式";
  state.autoApprove = state.mode === "auto";
  localStorage.setItem("ac.mode", state.mode);
  if (!$("modePopover").hidden) renderModeMenu();
  fitComposer();
}

// The three permission modes are a short list, not a cycle: every option and
// its consequence are visible before anything changes. Accidental clicks on
// the chip no longer flip the safety gate.
function renderModeMenu() {
  const wrap = $("modeList");
  wrap.innerHTML = MODES.map((id) => {
    const on = state.mode === id;
    return `<button class="mode-item${on ? " on" : ""}" data-mode="${id}" role="menuitemradio" aria-checked="${on}">` +
      `<span class="mode-dot"></span>` +
      `<span class="mode-item-text"><strong>${escapeHtml(MODE_LABEL[id])}</strong>` +
      `<small>${escapeHtml(MODE_TITLE[id])}</small></span>` +
      (on ? icon("check", "mi-check") : `<span class="mi-check"></span>`) +
      `</button>`;
  }).join("");
  wrap.querySelectorAll(".mode-item").forEach((b) => {
    b.addEventListener("click", (e) => { e.stopPropagation(); setMode(b.dataset.mode); });
  });
}

function positionModeMenu() {
  const p = $("modePopover");
  if (p.hidden) return;
  const btn = $("modeBtn");
  // fitComposer() may hide the chip in a very narrow window (Shift+Tab can
  // still open the list). Anchor to the composer in that case rather than to
  // a zero-sized rect, which would push the popover off screen.
  const r = btn.dataset.hidden === "1"
    ? $("input").getBoundingClientRect()
    : btn.getBoundingClientRect();
  const width = p.offsetWidth || 232;
  const gap = 6;
  const left = Math.max(4, Math.min(r.left, window.innerWidth - width - 4));
  p.style.left = Math.round(left) + "px";
  p.style.bottom = Math.round(window.innerHeight - r.top + gap) + "px";
}

function closeModeMenu() {
  const p = $("modePopover");
  // The menu owns focus while open (p.focus()). Give it back to the chip so
  // keyboard users can keep tabbing from where they were; a click elsewhere
  // keeps its own focus because activeElement is no longer inside the menu.
  if (!p.hidden && p.contains(document.activeElement)) $("modeBtn").focus();
  p.hidden = true;
  $("modeBtn").setAttribute("aria-expanded", "false");
}

function openModeMenu() {
  const p = $("modePopover");
  closeModelMenu();                  // only one overlay in this corner
  closeComposerMenu();
  renderModeMenu();
  p.hidden = false;
  $("modeBtn").setAttribute("aria-expanded", "true");
  positionModeMenu();
  const cur = p.querySelector(".mode-item.on") || p.querySelector(".mode-item");
  if (cur) cur.classList.add("cursor");
  p.focus();                         // arrow keys / Enter land on the menu
}

function setMode(id) {
  if (!MODES.includes(id)) return;
  const changed = state.mode !== id;
  state.mode = id;
  updateMode();
  closeModeMenu();
  if (changed) toast("执行模式：" + MODE_LABEL[state.mode]);
}

function syncAttachChip() {
  const n = state.attachments.length;
  $("attachLabel").textContent = n ? `上下文 ${n}` : "上下文";
  $("attachBtn").classList.toggle("on", n > 0);
  $("attachBtn").title = n
    ? `已附加 ${n} 个文件 · 点击管理`
    : "上下文：引用当前选中的文件，或在输入框敲 @ 引用";
  fitComposer();
}

function recordUsage(u) {
  const n = Number(u && (u.total_tokens ||
    (Number(u.prompt_tokens || 0) + Number(u.completion_tokens || 0)))) || 0;
  if (!n) return;
  state.usage.total += n;
  const el = $("ctxUsage");
  el.hidden = false;
  el.textContent = "ctx " + fmtTokens(state.usage.total);
  el.title = `本会话累计 ${state.usage.total} tokens（各轮用量之和，非精确上下文窗口占用）\n本轮 ${n}`;
  fitComposer();
}

// One fixed overlay serves the attachment picker, @ mentions, slash commands
// and the "more" menu. Anchoring it to the calling control keeps it attached
// when the composer grows, without adding a second row to the window.
function positionComposerMenu() {
  const m = $("composerMenu");
  if (m.hidden) return;
  const r = (state.menuAnchor || $("attachBtn")).getBoundingClientRect();
  const w = m.offsetWidth || 220;
  const h = m.offsetHeight || 160;
  const left = Math.max(4, Math.min(r.left, window.innerWidth - w - 4));
  const above = r.top - h - 3;
  let top = above >= 4 ? above : r.bottom + 3;
  // A short docked window can make even the "above" placement overflow; clamp
  // it so the last rows stay reachable instead of being cut off at the edge.
  if (top + h > window.innerHeight - 4) top = Math.max(4, window.innerHeight - h - 4);
  m.style.left = Math.round(left) + "px";
  m.style.top = Math.round(top) + "px";
}

function closeComposerMenu() {
  $("composerMenu").hidden = true;
  state.menuMode = null;
  state.menuAnchor = null;
}

function openComposerMenu(mode, anchorEl) {
  // Only one overlay at a time: the model / mode popovers sit in the same
  // corner of the composer and would otherwise overlap the shared menu.
  closeModelMenu();
  closeModeMenu();
  state.menuMode = mode;
  state.menuAnchor = anchorEl || $("attachBtn");
  state.menuCursor = 0;
  renderComposerMenu();
  $("composerMenu").hidden = false;
  positionComposerMenu();
}

function renderComposerMenu() {
  const mode = state.menuMode;
  let html = "";
  if (mode === "ctx") {
    const items = state.contextItems || [];
    html = `<div class="acm-head"><span>上下文 / 文件引用</span><span>勾选后随消息发送</span></div>`;
    if (!items.length) {
      html += `<div class="acm-empty">当前没有选中的文件 · 点标题栏刷新，或直接敲 @</div>`;
    } else {
      for (const it of items) {
        const p = String(it.path || it.name || "");
        const on = state.attachments.includes(p);
        html += `<button type="button" class="acm-item${on ? " checked" : ""}" data-ctx="${escapeHtml(p)}">` +
          `<span class="acm-check">${on ? "✓" : ""}</span>` +
          `<span class="acm-name">${escapeHtml(it.name || p)}</span>` +
          `<span class="acm-path" title="${escapeHtml(p)}">${escapeHtml(p)}</span></button>`;
      }
    }
    html += `<div class="acm-sep"></div><div class="acm-tip">结构化附加，不会把 @"路径" 混进提示词文本；输入框敲 @ 也能引用。</div>`;
  } else if (mode === "mention") {
    const q = String(state.mentionQuery || "").toLowerCase();
    const items = (state.contextItems || []).filter((it) =>
      String(it.name || it.path || "").toLowerCase().includes(q)).slice(0, 40);
    state.mentionItems = items;
    html = `<div class="acm-head"><span>@ 引用</span><span>点击或 Enter 插入</span></div>`;
    if (!items.length) html += `<div class="acm-empty">没有匹配的文件</div>`;
    else items.forEach((it, i) => {
      const p = String(it.path || it.name || "");
      html += `<button type="button" class="acm-item${i === 0 ? " checked" : ""}" data-mention="${escapeHtml(p)}">` +
        `<span class="acm-name">${escapeHtml(it.name || p)}</span>` +
        `<span class="acm-path" title="${escapeHtml(p)}">${escapeHtml(p)}</span></button>`;
    });
  } else {
    html = `<div class="acm-head"><span>${mode === "slash" ? "/ 命令" : "更多"}</span><span>点击执行</span></div>`;
    for (const c of SLASH_COMMANDS) {
      html += `<button type="button" class="acm-item" data-cmd="${escapeHtml(c.id)}">` +
        `<span class="acm-name">${escapeHtml(c.label)}</span>` +
        `<span class="acm-key">/${escapeHtml(c.id)}</span></button>`;
    }
    html += `<div class="acm-sep"></div><div class="acm-tip">输入框敲 / 打开命令 · ↑ 调出上一条 · Shift+Enter 换行。</div>`;
  }
  $("composerMenu").innerHTML = html;
  setComposerCursor(state.menuCursor);
}

// Keyboard navigation for the shared overlay. One `cursor` item is the Enter /
// Tab target; `state.menuCursor` is an index into the current .acm-item list.
function composerMenuItems() {
  return [...$("composerMenu").querySelectorAll(".acm-item")];
}

function setComposerCursor(i) {
  const items = composerMenuItems();
  if (!items.length) { state.menuCursor = -1; return; }
  state.menuCursor = ((i % items.length) + items.length) % items.length;
  items.forEach((el, idx) => el.classList.toggle("cursor", idx === state.menuCursor));
}

function moveComposerCursor(delta) {
  setComposerCursor(state.menuCursor < 0 ? 0 : state.menuCursor + delta);
}

function composerMenuActivate() {
  const items = composerMenuItems();
  if (!items.length) { closeComposerMenu(); return; }
  (items[state.menuCursor] || items[0]).click();
}

function toggleAttachment(path) {
  if (!path) return;
  const i = state.attachments.indexOf(path);
  if (i >= 0) state.attachments.splice(i, 1);
  else state.attachments.push(path);
  syncAttachChip();
  if (state.menuMode === "ctx" && !$("composerMenu").hidden) {
    renderComposerMenu();
    positionComposerMenu();
  }
}

// ↑ / ↓ recall prompts from this session. Only kicks in at the caret boundary
// so it never steals ordinary cursor movement in a multi-line message.
function recallPrompt(delta) {
  const p = state.prompts;
  if (!p.length) return;
  state.promptIndex = state.promptIndex < 0
    ? (delta < 0 ? p.length - 1 : 0)
    : Math.max(0, Math.min(p.length - 1, state.promptIndex + delta));
  const t = $("input");
  t.value = p[state.promptIndex] || "";
  autoGrow();
  t.setSelectionRange(t.value.length, t.value.length);
}

function runSlashCommand(id) {
  const c = SLASH_COMMANDS.find((x) => x.id === id);
  closeComposerMenu();
  if (c) c.run();
}

function insertMention(path) {
  const t = $("input");
  const caret = t.selectionStart == null ? t.value.length : t.selectionStart;
  const at = t.value.lastIndexOf("@", Math.max(0, caret - 1));
  const start = at >= 0 ? at : caret;
  const insert = `@"${path}" `;
  t.value = t.value.slice(0, start) + insert + t.value.slice(caret);
  const pos = start + insert.length;
  t.setSelectionRange(pos, pos);
  if (!state.attachments.includes(path)) state.attachments.push(path);
  syncAttachChip();
  closeComposerMenu();
  autoGrow();
  t.focus();
}

// Rule-based rewrite rather than an LLM call: this panel only ever asks for
// file operations, so a deterministic template is cheaper, instant and
// cannot fail. It also matches the read-only mode's "plan first" wording.
function enhancePrompt() {
  const t = $("input");
  const raw = t.value.trim();
  closeComposerMenu();
  if (!raw) { toast("先写点什么再增强"); return; }
  const bits = [];
  if (state.contextFolder && !/当前文件夹|这个文件夹|目录下|目录里/.test(raw)) {
    bits.push(`范围限定在当前文件夹「${state.contextFolder}」及其子目录`);
  }
  if (!/先|列出|不要|确认|预览/.test(raw)) {
    bits.push("先列出结果与操作方案，不要修改任何文件；等我确认后再执行");
  }
  if (!/依据|规则|按|根据|标准/.test(raw)) bits.push("说明判断依据");
  if (!bits.length) { toast("提示词已经很完整了"); return; }
  t.value = raw.replace(/[。；;\s]+$/, "") + "。" + bits.join("；") + "。";
  autoGrow();
  t.focus();
  toast("已补全范围与「先列出、不修改」，可直接编辑");
}

// With the composer one row tall, overflow must never wrap or grow the window.
// Hide the least important chips (highest data-prio) until the row fits.
function fitComposer() {
  const left = $("composerLeft");
  if (!left) return;
  const items = [...left.querySelectorAll("[data-prio]")];
  for (const el of items) el.dataset.hidden = "0";
  const ordered = items.slice().sort((a, b) => Number(b.dataset.prio) - Number(a.dataset.prio));
  for (const el of ordered) {
    if (left.scrollWidth <= left.clientWidth + 1) break;
    el.dataset.hidden = "1";
  }
}

// Detect @ / slash prefixes as the user types. A menu opened this way owns
// Enter and Escape until it closes, so it never competes with send().
function onInputChange() {
  autoGrow();
  const t = $("input");
  const caret = t.selectionStart == null ? t.value.length : t.selectionStart;
  const before = t.value.slice(0, caret);
  const at = before.lastIndexOf("@");
  const frag = at >= 0 ? before.slice(at + 1) : "";
  // Only a fresh token ("@name" at start or after whitespace), never the "@"
  // inside an email address or a Windows path.
  const tokenStart = at <= 0 || /[\s([{]/.test(before[at - 1]);
  if (at >= 0 && tokenStart && frag.length <= 40 && !/[\s@]/.test(frag)) {
    state.mentionQuery = frag;
    openComposerMenu("mention", t);
    return;
  }
  if (/^\/[^\s]*$/.test(t.value)) {
    openComposerMenu("slash", t);
    return;
  }
  // Any click-opened picker (ctx / more) must get out of the way once the user
  // starts typing, otherwise Enter would toggle a menu row instead of sending.
  if (state.menuMode) closeComposerMenu();
}

// Busy turns used to drop whatever the user typed. Queue instead, and flush
// one queued message after the current stream settles.
function submitInput() {
  const t = $("input");
  const text = t.value.trim();
  if (!text) return;
  // History recall: keep the last 50 prompts, reset the cursor on each new send.
  if (state.prompts[state.prompts.length - 1] !== text) {
    state.prompts.push(text);
    if (state.prompts.length > 50) state.prompts.shift();
  }
  state.promptIndex = -1;
  if (state.busy) {
    state.queue.push({ text, attachments: state.attachments.slice() });
    t.value = "";
    state.attachments = [];
    syncAttachChip();
    autoGrow();
    closeComposerMenu();
    toast(`已加入待发送队列（${state.queue.length} 条），当前回答结束后自动发送`);
    return;
  }
  t.value = "";
  autoGrow();
  closeComposerMenu();
  send(text);
}

async function newChat() {
  if (state.busy) { toast("请先停止当前对话"); return; }
  try {
    await apiFetch("/api/clear", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ sessionId: state.sessionId })
    });
  } catch (e) { }
  state.toolCards.clear();
  state.approvals.clear();
  state.lastUndo = null;
  postHostStatus("idle", "AI · 空闲", "", false, "", 0);
  state.activity.turns = [];
  state.activity.currentTurn = 0;
  state.activity.pendingPrompt = "";
  renderActivity();
  state.queue = [];
  state.attachments = [];
  state.prompts = [];
  state.promptIndex = -1;
  state.usage = { total: 0 };
  $("ctxUsage").hidden = true;
  closeComposerMenu();
  syncAttachChip();
  showEmpty();
  setGreeting();
  $("input").value = "";
  autoGrow();
  $("input").focus();
}

function wireEvents() {
  // 视图切换：对话 / 结构化活动流。同一批事件两种看法，切换不丢状态。
  $("tabChat").addEventListener("click", () => setView("chat"));
  $("tabActivity").addEventListener("click", () => setView("activity"));
  $("activityFilters").querySelectorAll(".filter-chip").forEach((b) => {
    b.addEventListener("click", () => setActivityFilter(b.dataset.filter));
  });

  // Custom titlebar. 面板在 WebView2 里是主窗口的子窗口：send to host 的只保留
  // minimize / close(收起) / maximize / pin。拖整窗做不到，所以不再发 drag，
  // 免得按钮看起来能点、点了却什么都没发生。
  const hostActions = !!(window.chrome && window.chrome.webview);
  if (hostActions) {
    $("titlebar").addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      postWindow("maximize");
    });
    document.querySelectorAll("[data-win]").forEach((b) => {
      b.addEventListener("click", () => postWindow(b.dataset.win));
    });
    const closeBtn = document.querySelector('[data-win="close"]');
    if (closeBtn) closeBtn.title = "收起 AI 面板";
  } else {
    // 浏览器直开时没有宿主窗口，隐藏这些按钮而不是留一排空点击。
    document.querySelectorAll("[data-win], #pinBtn").forEach((b) => { b.hidden = true; });
  }
  $("pinBtn").classList.toggle("pinned", state.pinned);
  $("pinBtn").addEventListener("click", () => {
    state.pinned = !state.pinned;
    localStorage.setItem("ac.pinned", state.pinned ? "1" : "0");
    $("pinBtn").classList.toggle("pinned", state.pinned);
    postWindow(state.pinned ? "pin" : "unpin");
    toast(state.pinned ? "窗口已置顶" : "已取消置顶");
  });
  $("newBtn").addEventListener("click", newChat);

  // Three-state permission gate lives in the composer now. `state.mode` is
  // sent with every turn; updateMode() also mirrors it into the legacy
  // autoApprove flag for older service builds. Clicking opens a list instead
  // of cycling, so an accidental click can never weaken the gate.
  $("modeBtn").addEventListener("click", (e) => {
    e.stopPropagation();
    if (!$("modePopover").hidden) closeModeMenu();
    else openModeMenu();
  });
  $("modePopover").addEventListener("keydown", (e) => {
    const items = [...$("modeList").querySelectorAll(".mode-item")];
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      if (!items.length) return;
      const cur = items.findIndex((b) => b.classList.contains("cursor"));
      const next = e.key === "ArrowDown"
        ? (cur < 0 ? 0 : Math.min(items.length - 1, cur + 1))
        : (cur < 0 ? items.length - 1 : Math.max(0, cur - 1));
      items.forEach((b, i) => b.classList.toggle("cursor", i === next));
      items[next].scrollIntoView({ block: "nearest" });
    } else if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      const target = items.find((b) => b.classList.contains("cursor")) || items[0];
      if (target) target.click();
    }
  });

  $("refreshBtn").addEventListener("click", () => loadContext(true));

  // context / enhance / more: all three share one fixed overlay so the
  // composer stays a single row. Buttons toggle their own menu closed.
  const toggleMenu = (mode, anchor) => {
    if (!$("composerMenu").hidden && state.menuMode === mode) closeComposerMenu();
    else openComposerMenu(mode, anchor);
  };
  $("attachBtn").addEventListener("click", async (e) => {
    e.stopPropagation();
    if (!$("composerMenu").hidden && state.menuMode === "ctx") { closeComposerMenu(); return; }
    openComposerMenu("ctx", $("attachBtn"));
    // Refresh the selected-file snapshot so a just-made selection shows up.
    await loadContext(false);
    if (state.menuMode === "ctx" && !$("composerMenu").hidden) {
      renderComposerMenu();
      positionComposerMenu();
    }
  });
  $("enhanceBtn").addEventListener("click", (e) => { e.stopPropagation(); enhancePrompt(); });
  $("moreBtn").addEventListener("click", (e) => { e.stopPropagation(); toggleMenu("more", $("moreBtn")); });

  // Delegate clicks inside the shared overlay. ctx rows toggle an attachment,
  // mention rows insert a reference, cmd rows run a slash command.
  $("composerMenu").addEventListener("click", (e) => {
    const ctxItem = e.target.closest("[data-ctx]");
    if (ctxItem) { toggleAttachment(ctxItem.dataset.ctx); return; }
    const mention = e.target.closest("[data-mention]");
    if (mention) { insertMention(mention.dataset.mention); return; }
    const cmd = e.target.closest("[data-cmd]");
    if (cmd) {
      // /模式 opens the mode dropdown. Without stopping propagation the
      // document-level outside-click handler would close it again in the
      // same event, since the click target is this menu, not #modePopover.
      e.stopPropagation();
      runSlashCommand(cmd.dataset.cmd);
      return;
    }
  });

  $("modelBtn").addEventListener("click", (e) => {
    e.stopPropagation();
    if (!$("modelPopover").hidden) closeModelMenu();
    else openModelMenu();
  });
  $("modelFavOnly").addEventListener("click", (e) => {
    e.stopPropagation();
    setFavOnly(!state.models.favOnly);
    // Keep the search box focused so typing a filter still works right away.
    $("modelSearch").focus();
  });
  $("modelSearch").addEventListener("input", renderModelMenu);
  $("modelSearch").addEventListener("keydown", (e) => {
    const items = [...$("modelGroups").querySelectorAll(".model-item")];
    const cursorKey = (b) => modelKey({ id: b.dataset.model, baseUrl: b.dataset.base,
      protocol: b.dataset.protocol });
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      if (!items.length) return;
      const cur = items.findIndex((b) => b.classList.contains("cursor"));
      const next = e.key === "ArrowDown"
        ? (cur < 0 ? 0 : Math.min(items.length - 1, cur + 1))
        : (cur < 0 ? items.length - 1 : Math.max(0, cur - 1));
      items.forEach((b, i) => b.classList.toggle("cursor", i === next));
      state.models.menuCursorKey = cursorKey(items[next]);
      items[next].scrollIntoView({ block: "nearest" });
    } else if ((e.key === "s" || e.key === "S" || e.code === "KeyS") && e.altKey) {
      // Keyboard equivalent of the row's star button: star the highlighted
      // model without needing a 17px mouse target.
      e.preventDefault();
      const target = items.find((b) => b.classList.contains("cursor")) || items[0];
      if (target) {
        state.models.menuCursorKey = cursorKey(target);
        toggleFavorite({ id: target.dataset.model, baseUrl: target.dataset.base,
          protocol: target.dataset.protocol });
      }
    } else if (e.key === "Enter") {
      e.preventDefault();
      const target = items.find((b) => b.classList.contains("cursor")) || items[0];
      if (target) target.click();
    }
  });
  window.addEventListener("resize", () => {
    positionModelMenu();
    positionModeMenu();
    positionVendorMenu();
    positionComposerMenu();
    fitComposer();
  });
  const modalBody = document.querySelector(".modal-body");
  if (modalBody) modalBody.addEventListener("scroll", () => positionVendorMenu());
  // The composer grows with the textarea and the modal is vertically centred,
  // so both fixed overlays must follow their anchors even without a window
  // resize (showing the probe list or a validation result moves things).
  if (window.ResizeObserver) {
    const composer = $("composer");
    if (composer) new window.ResizeObserver(() => {
      positionComposerMenu();
      positionModeMenu();
      fitComposer();
    }).observe(composer);
    const modal = document.querySelector(".modal");
    if (modal) new window.ResizeObserver(() => positionVendorMenu()).observe(modal);
  }
  $("customModelBtn").addEventListener("click", (e) => { e.stopPropagation(); openModelModal(); });
  document.addEventListener("click", (e) => {
    if (!e.target.closest("#modelPopover") && !e.target.closest("#modelBtn")) {
      closeModelMenu();
    }
    if (!e.target.closest("#modePopover") && !e.target.closest("#modeBtn")) {
      closeModeMenu();
    }
    if (!$("cmVendorMenu").hidden &&
        !e.target.closest("#cmVendorMenu") && !e.target.closest("#cmVendorBtn")) {
      closeVendorMenu();
    }
    // One shared overlay: close it on any outside click, but never on the
    // controls that open it (those toggle it themselves).
    if (!$("composerMenu").hidden &&
        !e.target.closest("#composerMenu") &&
        !e.target.closest("#attachBtn") &&
        !e.target.closest("#moreBtn")) {
      closeComposerMenu();
    }
  });

  // custom model dialog (four rows)
  $("modelModalClose").addEventListener("click", closeModelModal);
  $("cmCancel").addEventListener("click", closeModelModal);
  $("modelModal").addEventListener("click", (e) => {
    if (e.target === $("modelModal")) closeModelModal();
  });

  ["cmVendor", "cmBase", "cmKey", "cmModelId"].forEach((id) => {
    $(id).addEventListener("input", () => {
      if (id === "cmVendor") closeVendorMenu();
      if (id === "cmModelId") filterFound($(id).value);
      invalidateValidation();
      renderKeyHint();
      renderModeHint();
    });
  });
  $("cmProtocol").addEventListener("change", () => {
    invalidateValidation();
    $("cmBaseHint").textContent = "协议已切换，请确认接口地址";
    renderModeHint();
  });
  $("cmVendorBtn").addEventListener("click", (e) => {
    e.stopPropagation();
    toggleVendorMenu();
  });
  // Win32 ComboBox convention: Alt+Down opens the list straight from the field.
  $("cmVendor").addEventListener("keydown", (e) => {
    if (e.altKey && (e.key === "ArrowDown" || e.key === "ArrowUp")) {
      e.preventDefault();
      toggleVendorMenu(true);
    }
  });
  $("cmVendorMenu").addEventListener("click", (e) => {
    const item = e.target.closest("[data-vendor]");
    if (!item) return;
    const v = item.dataset.vendor;
    closeVendorMenu();
    // "自定义 / 手动输入" is an explicit ADD: clear the whole form, the
    // validation state and the found-model list, and leave edit mode. Keeping
    // a stale identity here is what used to overwrite a saved provider.
    if (v === "custom") { resetProviderForm(); return; }
    if (v.startsWith("preset:")) {
      const preset = vendorPresets().find((x) => x.id === v.slice(7));
      if (preset) applyPreset(preset);
    } else if (v.startsWith("saved:")) {
      const id = v.slice(6);
      const saved = state.models.providers || [];
      const p = id === "__active__"
        ? saved.find((x) => !x.key && x.active)
        : saved.find((x) => (x.key || "") === id);
      if (p) applySaved(p);
    }
  });
  $("cmKeyHint").addEventListener("click", (e) => {
    if (e.target.closest("#cmKeyReplace")) $("cmKey").focus();
  });
  $("cmValidate").addEventListener("click", () => validateKey(false));
  $("cmNewProvider").addEventListener("click", resetProviderForm);
  $("cmSave").addEventListener("click", saveProvider);
  $("cmFetch").addEventListener("click", fetchModels);
  // addModels(forceManual) must receive a real boolean: passing the click
  // event directly would be truthy and silently bypass the "this is a filter
  // word, not a model ID" guard added for the live-filter input.
  $("cmAdd").addEventListener("click", () => addModels(false));
  $("cmModelId").addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); addModels(true); }
  });
  $("cmBase").addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); validateKey(false); }
  });
  $("cmFoundList").addEventListener("change", (e) => {
    const cb = e.target.closest("input[data-id]");
    if (!cb) return;
    if (cb.checked) state.models.foundSelected.add(cb.dataset.id);
    else state.models.foundSelected.delete(cb.dataset.id);
    updateFoundSelection();
  });
  $("cmFoundSelectAll").addEventListener("click", () => {
    // "全选" means the rows the user can currently see, never the hidden ones.
    const rows = [...$("cmFoundList").querySelectorAll(".cm-found-row")]
      .filter((r) => !r.hidden);
    const sel = state.models.foundSelected;
    const all = rows.length > 0 && rows.every((r) => {
      const cb = r.querySelector("input");
      return cb && cb.checked;
    });
    for (const row of rows) {
      const cb = row.querySelector("input");
      if (!cb) continue;
      cb.checked = !all;
      if (all) sel.delete(cb.dataset.id);
      else sel.add(cb.dataset.id);
    }
    updateFoundSelection();
  });
  // The enable switch fires `change`, not `click`, so it never collides with
  // the edit / test / delete buttons handled above.
  $("cmGroups").addEventListener("change", (e) => {
    const cb = e.target.closest("input.cm-enable");
    if (!cb) return;
    const row = cb.closest(".cm-model-row");
    if (!row) return;
    const p = state.models.providers[Number(row.dataset.provider)];
    if (!p) return;
    setModelEnabled(p, row.dataset.model, cb.checked);
  });
  $("cmGroups").addEventListener("click", (e) => {
    const rm = e.target.closest("[data-remove]");
    if (rm) {
      e.stopPropagation();
      const p = state.models.providers[Number(rm.dataset.remove)];
      if (p) removeProvider(p);
      return;
    }
    const act = e.target.closest("[data-act]");
    if (act) {
      e.stopPropagation();
      const row = act.closest(".cm-model-row");
      const p = state.models.providers[Number(row.dataset.provider)];
      const id = row.dataset.model;
      if (!p) return;
      if (act.dataset.act === "edit") startEditModel(p, id, row);
      else if (act.dataset.act === "test") testModel(p, id);
      else if (act.dataset.act === "delete") deleteModel(p, id);
      else if (act.dataset.act === "register") registerModel(p, id);
      return;
    }
    const head = e.target.closest(".cm-group-head");
    if (head) {
      const group = head.closest(".cm-group");
      const key = group.dataset.key;
      if (state.models.collapsed.has(key)) state.models.collapsed.delete(key);
      else state.models.collapsed.add(key);
      group.classList.toggle("collapsed");
    }
  });

  // Escape closes the vendor list first, then the dialog, then the mode /
  // model dropdowns and the shared composer menu; capture phase so it wins
  // over the composer's own "stop the turn" handler.
  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape") return;
    // An inline model-ID edit owns Escape: cancel that edit, keep the dialog.
    // This handler is in the capture phase and would otherwise close the whole
    // modal before the input's own keydown listener ever sees the key.
    if (e.target && e.target.closest && e.target.closest(".cm-edit-input")) return;
    if (!$("cmVendorMenu").hidden) {
      e.preventDefault(); e.stopPropagation(); closeVendorMenu(); return;
    }
    if (!$("modelModal").hidden) {
      e.preventDefault(); e.stopPropagation(); closeModelModal(); return;
    }
    if (!$("modePopover").hidden) {
      e.preventDefault(); e.stopPropagation(); closeModeMenu(); return;
    }
    if (!$("composerMenu").hidden) {
      e.preventDefault(); e.stopPropagation(); closeComposerMenu(); return;
    }
    if (!$("modelPopover").hidden) {
      e.preventDefault(); e.stopPropagation(); closeModelMenu();
    }
  }, true);

  // Global model shortcuts. Alt is used instead of Ctrl so WebView2's own
  // accelerators (Ctrl+1..9 switches tabs) are never stolen. `e.code` is the
  // fallback for IME/layouts where Alt+Digit does not produce a printable key.
  // Capture phase, but it deliberately steps aside while the custom-model
  // dialog owns the keyboard and while a key is being held down.
  document.addEventListener("keydown", (e) => {
    if (!e.altKey || e.ctrlKey || e.metaKey || e.shiftKey || e.repeat) return;
    if (!$("modelModal").hidden) return;
    const isM = (e.key || "").toLowerCase() === "m" || e.code === "KeyM";
    if (isM) {
      e.preventDefault(); e.stopPropagation();
      if (!$("modelPopover").hidden) closeModelMenu();
      else openModelMenu();
      return;
    }
    let n = /^[1-9]$/.test(e.key || "") ? Number(e.key) : 0;
    if (!n && /^Digit[1-9]$/.test(e.code || "")) n = Number(e.code.slice(5));
    if (!n && /^Numpad[1-9]$/.test(e.code || "")) n = Number(e.code.slice(6));
    if (!n) return;
    e.preventDefault(); e.stopPropagation();
    quickSwitchFavorite(n - 1);
  }, true);

  // The service re-reads aichat.json on every request, so refresh the model
  // chip whenever the window is refocused. Editing the file no longer needs a
  // service restart or even a window reopen.
  window.addEventListener("focus", () => loadConfig());
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) loadConfig();
  });

  document.querySelectorAll(".suggestion").forEach((b) => {
    b.addEventListener("click", () => {
      $("input").value = b.dataset.prompt;
      submitInput();
    });
  });

  const input = $("input");
  input.addEventListener("input", onInputChange);
  input.addEventListener("keydown", (e) => {
    // A visible overlay owns the navigation keys while it is open.
    const menuOpen = !$("composerMenu").hidden && !!state.menuMode;
    if (menuOpen && (e.key === "ArrowDown" || e.key === "ArrowUp")) {
      e.preventDefault();
      moveComposerCursor(e.key === "ArrowDown" ? 1 : -1);
      return;
    }
    if (menuOpen && (e.key === "Enter" || e.key === "Tab") && !e.shiftKey) {
      e.preventDefault();
      composerMenuActivate();
      return;
    }
    if (e.key === "Tab" && e.shiftKey) {
      e.preventDefault();
      openModeMenu();
      return;
    }
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      submitInput();
    } else if (e.key === "Escape" && state.busy) {
      e.preventDefault();
      stop();
    } else if (e.key === "ArrowUp" && !e.shiftKey && input.selectionStart === 0) {
      e.preventDefault();
      recallPrompt(-1);
    } else if (e.key === "ArrowDown" && !e.shiftKey && state.promptIndex >= 0 &&
               input.selectionStart === input.value.length) {
      e.preventDefault();
      recallPrompt(1);
    }
  });

  $("sendBtn").addEventListener("click", () => {
    if (state.busy) stop();
    else submitInput();
  });
}

async function init() {
  setGreeting();
  setView(state.activity.view);
  updateMode();
  syncAttachChip();
  wireEvents();
  autoGrow();
  fitComposer();
  await Promise.all([loadConfig(), loadHealth(), loadHistory()]);
  // The anchor may be empty on a fresh start (nothing has read the window yet).
  // Ask the bridge once so the folder chip still shows where the user is,
  // instead of leaving "正在读取当前文件夹…" forever.
  const c = await loadContext(false);
  if (!c || !c.folder) await loadContext(true);
  // Restore the pinned state in the host (host owns the actual Topmost flag).
  if (state.pinned) postWindow("pin");
  $("input").focus();
}

init();
