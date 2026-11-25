// 内联执行台账的离线验证：stub 最小 DOM，加载真实 app.js，断言工具调用
// 挂到"产生它的助手回答"里、状态就地更新、异常默认展开、历史恢复也重建。
// 不发网络请求，也不依赖 jsdom。
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const appPath = path.join(__dirname, "..", "aichat", "ui", "app.js");
let src = fs.readFileSync(appPath, "utf8").replace(/\ninit\(\);\s*$/, "\n");

function makeClassList(owner) {
  const parts = () => String(owner.className || "").split(/\s+/).filter(Boolean);
  return {
    contains(name) { return parts().includes(name); },
    add(name) { const p = parts(); if (!p.includes(name)) owner.className = p.concat(name).join(" "); },
    remove(name) { owner.className = parts().filter((x) => x !== name).join(" "); },
    toggle(name, force) {
      const has = parts().includes(name);
      const want = force === undefined ? !has : !!force;
      if (want) this.add(name); else this.remove(name);
      return want;
    }
  };
}

class FakeEl {
  constructor(tag) {
    this.tagName = String(tag || "div").toUpperCase();
    this.className = "";
    this.classList = makeClassList(this);
    this.dataset = {};
    this.style = {};
    this.hidden = false;
    this.title = "";
    this.textContent = "";
    this.parentNode = null;
    this.children = [];
    this._listeners = {};
    this._q = new Map();
    this._html = "";
  }
  set innerHTML(v) {
    this._html = String(v == null ? "" : v);
    this.children = [];
    this._q.clear();
  }
  get innerHTML() { return this._html; }
  appendChild(c) { this.children.push(c); c.parentNode = this; return c; }
  insertBefore(c, ref) {
    const i = this.children.indexOf(ref);
    if (i < 0) this.children.push(c); else this.children.splice(i, 0, c);
    c.parentNode = this;
    return c;
  }
  querySelector(sel) {
    if (!this._q.has(sel)) this._q.set(sel, new FakeEl("div"));
    return this._q.get(sel);
  }
  querySelectorAll() { return []; }
  addEventListener(type, fn) {
    (this._listeners[type] || (this._listeners[type] = [])).push(fn);
  }
  click() { for (const fn of this._listeners.click || []) fn(); }
  focus() {}
  get firstChild() { return this.children[0] || null; }
}

const nodes = new Map();
const el = (id) => {
  if (!nodes.has(id)) nodes.set(id, new FakeEl("div"));
  return nodes.get(id);
};
for (const id of ["messages", "thread", "emptyState", "toast", "status", "activityCount",
                  "activityList", "activityEmpty", "activityFilters", "activityFilterSep",
                  "activityView", "tabChat", "tabActivity"]) el(id);

let historyResponse = { messages: [] };
const sandbox = {
  console,
  document: {
    getElementById: el,
    createElement: (tag) => new FakeEl(tag),
    querySelectorAll: () => [],
    addEventListener: () => {}
  },
  localStorage: {
    _d: {},
    getItem(k) { return Object.prototype.hasOwnProperty.call(this._d, k) ? this._d[k] : null; },
    setItem(k, v) { this._d[k] = String(v); }
  },
  fetch: async () => ({ ok: true, json: async () => historyResponse }),
  EventSource: function () {},
  setInterval: () => 0, clearInterval: () => {},
  setTimeout: () => 0, clearTimeout: () => {},
  requestAnimationFrame: (fn) => { if (fn) fn(); return 0; },
  cancelAnimationFrame: () => {},
  location: { href: "", search: "" },
  navigator: { userAgent: "", clipboard: { writeText: async () => {} } },
  URLSearchParams, Headers,
  AbortController: function () { this.signal = {}; this.abort = function () {}; },
  Date, JSON, Math, Number, String, Array, Object, Set, Map, RegExp, Error,
  Promise, Boolean, parseInt, parseFloat, isNaN,
  __assistants: []
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(src, sandbox, { filename: "app.js" });

const FN = (name) => vm.runInContext(name, sandbox);
const S = vm.runInContext("state", sandbox);
const thread = el("thread");
let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}

console.log("\n[1] 同一批 tool 事件内嵌进回答，不散落到 thread");
const a = FN("appendAssistant")("正在整理", 1000, null);
const ledger = a.ledgerEl;
FN("appendTool")({ id: "s1", name: "file.list", command: "file.list", label: "列出文件",
  status: "running", risk: "read", step: 1, ts: 1001, args: { path: "C:\\Pictures" } }, a);
FN("appendTool")({ id: "s2", name: "view.set", command: "view.set", label: "切换视图",
  status: "done", risk: "write", step: 2, ts: 1002, args: { mode: "details" }, result: "ok" }, a);
ok(ledger.hidden === false, "首条工具事件后台账可见");
ok(a.ledgerBody.children.length === 2, "两条工具 = 两行台账");
ok(!ledger.classList.contains("open"), "全部正常时默认折叠");
ok(ledger.querySelector(".ledger-summary").textContent.includes("执行 2 步 · 完成 1"),
  "摘要含步数与完成数");
ok(ledger.querySelector(".ledger-risk").textContent.includes("只读 1") &&
   ledger.querySelector(".ledger-risk").textContent.includes("写 1"), "摘要含风险分布");
ok(thread.children.filter((x) => x.className.includes("tool-card")).length === 0,
  "thread 上没有散落的独立工具卡");

console.log("\n[2] running → done 就地更新，不新增行");
FN("appendTool")({ id: "s1", name: "file.list", command: "file.list", label: "列出文件",
  status: "done", risk: "read", step: 1, ts: 1003, result: "12 entries" }, a);
ok(a.ledgerBody.children.length === 2, "仍是 2 行");
const first = a.ledgerBody.children[0];
ok(first.querySelector(".step-badge").textContent === "完成", "状态更新为完成");
ok(first.querySelector(".step-risk").textContent === "只读", "只读徽章保留");
ok(first.querySelector(".result-pre").textContent === "12 entries", "结果写进行内详情");

console.log("\n[3] 失败/未达成/危险默认展开（progressive disclosure）");
const b = FN("appendAssistant")("整理完成", 2000, null);
FN("appendTool")({ id: "s3", name: "fs.rename", command: "fs.rename", label: "重命名",
  status: "mismatch", risk: "write", step: 1, ts: 2001, args: { from: "a.jpg" },
  error: "效果未达成", assertions: [{ expr: "exists", expect: true, actual: false, ok: false }] }, b);
FN("appendTool")({ id: "s4", name: "fs.delete", command: "fs.delete", label: "删除临时文件",
  status: "done", risk: "danger", step: 2, ts: 2002, args: { targets: ["tmp.txt"] } }, b);
ok(b.ledgerEl.classList.contains("open"), "mismatch + danger → 整块台账展开");
ok(b.ledgerEl.classList.contains("has-failure"), "失败态加到台账上");
ok(b.ledgerEl.classList.contains("has-danger"), "危险态加到台账上");
ok(b.ledgerBody.children.every((x) => x.classList.contains("open"))
  === false, "成功但危险的那一行仍保持自身折叠");
ok(b.ledgerBody.children[0].querySelector(".assert-list").innerHTML
  .includes("exists"), "自断言明细写进行内详情");
b.ledgerEl.querySelector(".ledger-head").click();
ok(!b.ledgerEl.classList.contains("open"), "用户可以手动折叠异常台账");
FN("appendTool")({ id: "s5", name: "fs.delete", command: "fs.delete", label: "清理",
  status: "done", risk: "danger", step: 3, ts: 2003, args: {} }, b);
ok(!b.ledgerEl.classList.contains("open"), "后续流式事件不覆盖手动折叠");

console.log("\n[4] 历史恢复：旧回答也重建执行台账");
historyResponse = { messages: [
  { role: "user", content: "删掉临时文件", ts: 3000 },
  { role: "assistant", content: "已删除", ts: 3100, turn: 1, tools: [
    { name: "file.list", command: "file.list", label: "列出文件", status: "done",
      risk: "read", step: 1, ts: 3050, args: { path: "C:\\Temp" }, result: "3 entries" },
    { name: "fs.delete", command: "fs.delete", label: "删除", status: "done",
      risk: "danger", step: 2, ts: 3060, args: { targets: ["tmp.txt"] } }
  ] }
] };
thread.innerHTML = "";
sandbox.__assistants.length = 0;
vm.runInContext(`
  const __origAppendAssistant = appendAssistant;
  appendAssistant = function () {
    const a = __origAppendAssistant.apply(null, arguments);
    globalThis.__assistants.push(a);
    return a;
  };
`, sandbox);
(async () => {
  await FN("loadHistory")();
  const h = sandbox.__assistants[sandbox.__assistants.length - 1];
  ok(!!h, "恢复出 1 条助手消息");
  const hLedger = h && h.ledgerEl;
  ok(!!hLedger && hLedger.hidden === false, "历史回答的台账可见");
  ok(!!h && h.ledgerBody.children.length === 2, "旧数据无 id 也重建出 2 行");
  ok(!!hLedger && hLedger.querySelector(".ledger-risk").textContent.includes("危险 1"), "历史危险档保留");
  ok(!!hLedger && hLedger.classList.contains("open"), "历史含危险操作 → 默认展开");
  ok(!!h && h.ledgerBody.children[0].querySelector(".step-cmd").textContent
    === "file.list", "历史点分命令名回填");
  ok(S.activity.turns.length === 1 && S.activity.turns[0].tools.length === 2,
    "独立活动流仍从同一份历史重建");

  console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
  process.exit(fail ? 1 : 0);
})();
