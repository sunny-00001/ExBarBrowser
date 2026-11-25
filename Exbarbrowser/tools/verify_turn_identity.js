// 权威 turnId 在活动流与回答内联台账之间的共享契约。用真实 app.js + 最小
// DOM 沙箱，不发网络请求、不启动浏览器。
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const appPath = path.join(__dirname, "..", "aichat", "ui", "app.js");
const src = fs.readFileSync(appPath, "utf8").replace(/\ninit\(\);\s*$/, "\n");

class FakeEl {
  constructor() {
    this.className = "";
    this.dataset = {};
    this.style = {};
    this.hidden = false;
    this.textContent = "";
    this.innerHTML = "";
    this.children = [];
    this.scrolled = false;
    this._q = new Map();
    this._classes = new Set();
    const self = this;
    this.classList = {
      add(c) { self._classes.add(c); },
      remove(c) { self._classes.delete(c); },
      contains(c) { return self._classes.has(c); },
      toggle(c, force) {
        const on = force === undefined ? !self._classes.has(c) : !!force;
        if (on) self._classes.add(c); else self._classes.delete(c);
        return on;
      }
    };
  }
  querySelector(sel) {
    if (!this._q.has(sel)) this._q.set(sel, new FakeEl());
    return this._q.get(sel);
  }
  querySelectorAll() { return []; }
  addEventListener() {}
  appendChild(c) { this.children.push(c); return c; }
  scrollIntoView() { this.scrolled = true; }
}

const nodes = {};
const getEl = (id) => (nodes[id] || (nodes[id] = new FakeEl()));
const sandbox = {
  console,
  document: {
    getElementById: getEl,
    createElement: () => new FakeEl(),
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener: () => {}
  },
  localStorage: {
    _d: {},
    getItem(k) { return Object.prototype.hasOwnProperty.call(this._d, k) ? this._d[k] : null; },
    setItem(k, v) { this._d[k] = String(v); }
  },
  fetch: async () => ({ ok: false, status: 500 }),
  setInterval: () => 0, clearInterval: () => {},
  setTimeout: (fn) => { if (fn) fn(); return 0; }, clearTimeout: () => {},
  requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
  location: { href: "", search: "" }, navigator: { userAgent: "" },
  URLSearchParams, Headers,
  AbortController: function () { this.signal = {}; this.aborted = false; this.abort = () => { this.aborted = true; }; },
  Date, JSON, Math, Number, String, Array, Object, Set, Map, RegExp, Error,
  Promise, Boolean, parseInt, parseFloat, isNaN
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(src, sandbox, { filename: "app.js" });
vm.runInContext(`
  renderActivity = () => {};
  setStatus = () => {};
  setBusy = () => {};
  loadContext = () => {};
  recordUsage = () => {};
  clearApprovals = () => {};
  appendTool = () => {};
  appendApproval = () => {};
  toast = () => {};
  syncAttachChip = () => {};
  setView = (v) => { globalThis.__view = v; };
`, sandbox);

const S = vm.runInContext("state", sandbox);
const FN = (name) => vm.runInContext(name, sandbox);
let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}

console.log("\n[1] turnIdOf 只认服务端权威身份，旧 turn 仅作兼容回落");
const turnIdOf = FN("turnIdOf");
ok(turnIdOf({ turnId: 41, turn: 99 }) === 41, "turnId 优先于旧 turn");
ok(turnIdOf({ turn: 42 }) === 42, "旧事件仍可读 turn");
ok(turnIdOf({ turnId: 0, turn: 0 }) === 0, "无有效身份返回 0");

console.log("\n[2] 回答气泡与内联台账落同一个 turnId");
const assistant = FN("appendAssistant")("", Date.now(), null, 41);
ok(assistant.turnId === 41, "assistant 持有权威 turnId");
ok(assistant.el.dataset.turnId === "41", "气泡 DOM 带 data-turn-id");
ok(assistant.ledgerEl.dataset.turnId === "41", "台账 DOM 带同一 data-turn-id");
ok(assistant.ledgerEl.querySelector(".ledger-turn").textContent === "第 41 轮", "台账显示轮次");
assistant.setTurnId(42);
ok(assistant.el.dataset.turnId === "42" && assistant.ledgerEl.dataset.turnId === "42",
  "start 事件能就地改写身份");

console.log("\n[3] 活动流分组使用同一身份");
S.activity.turns = [];
S.activity.currentTurn = 0;
FN("recordActivity")({ type: "tool", id: "a", turnId: 41, turn: 99 });
FN("recordActivity")({ type: "tool", id: "b", turnId: 42, turn: 42 });
ok(S.activity.turns.length === 2, "不同 turnId 不合并");
ok(S.activity.turns[0].turnId === 41 && S.activity.turns[0].turn === 41,
  "turnId 覆盖历史遗留的冲突 turn");

console.log("\n[4] 历史恢复优先 turnId，不用旧 turn 顶替");
FN("rebuildActivityFromHistory")([
  { role: "user", content: "旧消息", turnId: 7, turn: 6 },
  { role: "assistant", content: "回答", turnId: 7, turn: 6, tools: [] }
]);
ok(S.activity.turns.length === 1 && S.activity.turns[0].turnId === 7,
  "历史分组取 turnId=7");

console.log("\n[5] start 事件把权威 turnId 同时交给事件归属与台账");
S.generation = 1;
S.activeTurn = 0;
const live = FN("appendAssistant")("", Date.now(), null, null);
FN("handleEvent")({ type: "start", turnId: 51, turn: 50 }, live, 1);
ok(S.activeTurn === 51, "activeTurn 取 turnId");
ok(live.turnId === 51, "实时台账写入同一 turnId");

console.log("\n[6] 活动流轮次可按 turnId 定位到对应回答");
const target = new FakeEl();
const focusLedger = target.querySelector(".step-ledger");
sandbox.document.querySelector = (sel) => {
  ok(sel === '.msg.assistant[data-turn-id="51"]', "按稳定身份查 DOM：" + sel);
  return target;
};
FN("focusAssistantTurn")(51);
ok(sandbox.__view === "chat", "定位后切回聊天视图");
ok(focusLedger.classList.contains("open"), "对应台账被展开");
ok(target.scrolled === true, "对应回答被滚动到可见区");

console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
process.exit(fail ? 1 : 0);
