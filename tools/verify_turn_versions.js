// 回合重试/编辑的 UI 契约：旧回合不覆盖，新请求带 retryOf，活动流显示版本。
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const appPath = path.join(__dirname, "..", "aichat", "ui", "app.js");
const src = fs.readFileSync(appPath, "utf8").replace(/\ninit\(\);\s*$/, "\n");

class FakeEl {
  constructor() {
    this.className = ""; this.dataset = {}; this.style = {};
    this.hidden = false; this.textContent = ""; this.innerHTML = "";
    this.children = []; this.value = ""; this._q = new Map();
    const classes = new Set();
    this.classList = {
      add: (c) => classes.add(c),
      remove: (c) => classes.delete(c),
      contains: (c) => classes.has(c),
      toggle: (c, force) => {
        const on = force === undefined ? !classes.has(c) : !!force;
        if (on) classes.add(c); else classes.delete(c);
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
  focus() { this.focused = true; }
}

const nodes = {};
const getEl = (id) => (nodes[id] || (nodes[id] = new FakeEl()));
let lastBody = null;
let lastBranchBody = null;
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
  fetch: async (url, opts) => {
    const u = String(url);
    if (u.includes("/api/branch")) {
      lastBranchBody = JSON.parse(String(opts.body || "{}"));
      return { ok: true, json: async () => ({ ok: true }) };
    }
    if (u.includes("/api/history")) {
      return { ok: true, json: async () => ({ messages: [] }) };
    }
    lastBody = JSON.parse(String(opts.body || "{}"));
    return {
      ok: true,
      body: {
        getReader() {
          return {
            read: async () => ({ done: true, value: undefined }),
            cancel: async () => {}
          };
        }
      }
    };
  },
  setInterval: () => 0, clearInterval: () => {},
  setTimeout: (fn) => { if (fn) fn(); return 0; }, clearTimeout: () => {},
  requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
  location: { href: "", search: "" }, navigator: { userAgent: "" },
  URLSearchParams, Headers, TextDecoder,
  AbortController: function () { this.signal = {}; this.aborted = false; this.abort = () => { this.aborted = true; }; },
  Date, JSON, Math, Number, String, Array, Object, Set, Map, RegExp, Error,
  Promise, Boolean, parseInt, parseFloat, isNaN
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(src, sandbox, { filename: "app.js" });
vm.runInContext(`
  setStatus = () => {};
  setBusy = (on) => { state.busy = !!on; };
  loadContext = () => {};
  clearApprovals = () => {};
  recordUsage = () => {};
  flushQueue = () => {};
  toast = (x) => { globalThis.__toast = String(x); };
`, sandbox);

const S = vm.runInContext("state", sandbox);
const FN = (name) => vm.runInContext(name, sandbox);
let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}

console.log("\n[1] 失败回合显示重试入口，版本标记可读");
const failed = FN("appendAssistant")("旧回答", Date.now(), null, 10);
failed.setVersion({ retryOf: 9, rootTurnId: 9, attempt: 2 });
ok(failed.el.querySelector(".turn-version").textContent === "第 2 次尝试", "尝试号写入 DOM");
failed.markFailed("stopped");
ok(failed.el.querySelector(".turn-actions").hidden === false, "失败回合显示重试按钮");

console.log("\n[2] 直接重试沿用原提示词，并带权威 retryOf");
failed.setPrompt("原始问题");
S.busy = false;
lastBody = null;
(async () => {
  await FN("retryTurn")(10, failed.prompt);
  ok(lastBody && lastBody.text === "原始问题", "重试沿用原提示词");
  ok(lastBody && Number(lastBody.retryOf) === 10, "请求带 retryOf=10");

  console.log("\n[3] 编辑重试先把原提示词放进输入框，下一次 send 带 retryOf");
  S.busy = false; S.pendingEditOf = 0;
  FN("editTurn")(10, "原始问题");
  ok(S.pendingEditOf === 10, "暂存被编辑的旧回合");
  ok(nodes.input.value === "原始问题" && nodes.input.focused === true, "输入框预填并聚焦");

  lastBody = null;
  await FN("send")("改过的问题");
  ok(lastBody && lastBody.text === "改过的问题", "编辑后的文本被发送");
  ok(lastBody && Number(lastBody.retryOf) === 10, "编辑后仍指向旧回合分叉点");
  ok(S.pendingEditOf === 0, "编辑目标发送后清空");

  console.log("\n[4] 历史版本字段进入活动流");
  FN("rebuildActivityFromHistory")([
    { role: "user", content: "原始", turnId: 10 },
    { role: "assistant", content: "旧", turnId: 10, error: "stopped", tools: [{ id: "o1" }] },
    { role: "user", content: "新", turnId: 11, retryOf: 10, rootTurnId: 10, attempt: 2 },
    { role: "assistant", content: "新答", turnId: 11, retryOf: 10, rootTurnId: 10, attempt: 2, tools: [{ id: "n1" }] },
    { role: "assistant", content: "旧版本标记", turnId: 10, supersededBy: 11, tools: [] }
  ]);
  const g2 = S.activity.turns.find((x) => x.turn === 11);
  ok(g2 && g2.retryOf === 10 && g2.attempt === 2, "新回合保留血缘字段");
  ok(S.activity.turns.some((x) => x.supersededBy === 11 || x.turn === 10), "旧回合仍可被识别");

  FN("renderActivity")();
  const html = nodes.activityList.innerHTML;
  ok(html.includes("尝试 2"), "活动流显示尝试号");
  ok(html.includes("旧版本"), "活动流标记被取代的旧版本");
  ok(html.includes("data-select-turn="), "活动流提供版本切换按钮");

  console.log("\n[5] 切换版本写入分支选择并重载历史");
  lastBranchBody = null;
  await FN("selectBranch")(10, 10);
  ok(lastBranchBody && Number(lastBranchBody.rootTurnId) === 10, "切换请求带 rootTurnId");
  ok(lastBranchBody && Number(lastBranchBody.turnId) === 10, "切换请求带目标 turnId");
  ok(sandbox.__toast.includes("已切换"), "切换成功后给出反馈");

  console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
  process.exit(fail ? 1 : 0);
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
