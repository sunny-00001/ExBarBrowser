// AI 面板向宿主上报状态的离线验证：不合成输入、不抢焦点，只跑真实 app.js 的函数。
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const appPath = path.join(__dirname, "..", "aichat", "ui", "app.js");
const src = fs.readFileSync(appPath, "utf8").replace(/\ninit\(\);\s*$/, "\n");

class FakeEl {
  constructor(id = "") {
    this.id = id;
    this.dataset = {};
    this.style = {};
    this.hidden = false;
    this.title = "";
    this.textContent = "";
    this.innerHTML = "";
    this.value = "";
    this.disabled = false;
    this.scrollTop = 0;
    this.scrollHeight = 0;
    this.clientHeight = 0;
    this.children = [];
    this.listeners = {};
    this.classList = {
      toggle() {}, add() {}, remove() {}, contains() { return false; }
    };
  }
  querySelector() { return new FakeEl(); }
  querySelectorAll() { return []; }
  addEventListener(type, fn) {
    (this.listeners[type] || (this.listeners[type] = [])).push(fn);
  }
  fire(type, ev = {}) {
    for (const fn of this.listeners[type] || []) fn(ev);
  }
  closest() { return null; }
  appendChild(c) { this.children.push(c); return c; }
  focus() {}
}

const nodes = {};
const getEl = (id) => (nodes[id] || (nodes[id] = new FakeEl(id)));
const posted = [];

const sandbox = {
  console,
  document: {
    getElementById: getEl,
    createElement: () => new FakeEl(),
    querySelector: () => new FakeEl(),
    querySelectorAll: () => [],
    addEventListener() {}
  },
  localStorage: {
    _d: {},
    getItem(k) { return Object.prototype.hasOwnProperty.call(this._d, k) ? this._d[k] : null; },
    setItem(k, v) { this._d[k] = String(v); }
  },
  fetch: async () => ({
    ok: true,
    status: 200,
    body: null,
    json: async () => ({ selected: [], folder: "", listerIndex: -1 })
  }),
  setInterval: () => 0, clearInterval: () => {},
  setTimeout: (fn) => { if (fn) fn(); return 0; }, clearTimeout: () => {},
  requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
  location: { href: "", search: "" }, navigator: { userAgent: "" },
  addEventListener() {}, removeEventListener() {}, innerWidth: 420, innerHeight: 800,
  URLSearchParams, Headers,
  AbortController: function () { this.signal = {}; this.aborted = false; this.abort = () => { this.aborted = true; }; },
  Date, JSON, Math, Number, String, Array, Object, Set, Map, RegExp, Error,
  Promise, Boolean, parseInt, parseFloat, isNaN
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;
sandbox.chrome = {
  webview: {
    postMessage: (m) => posted.push(m),
    addEventListener() {},
    removeEventListener() {}
  }
};

vm.createContext(sandbox);
vm.runInContext(src, sandbox, { filename: "app.js" });
const FN = (name) => vm.runInContext(name, sandbox);
const S = vm.runInContext("state", sandbox);

let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}

console.log("\n[1] postHostStatus 白名单与字段");
FN("postHostStatus")("done", "摘要", "详情", true, "file", 7);
ok(posted.length === 1, "合法状态发出一条消息");
const m = posted[0];
ok(m && m.type === "status" && m.state === "done", "消息类型/状态正确");
ok(m && m.summary === "摘要" && m.detail === "详情" && m.turnId === 7, "摘要/详情/回合号正确");
ok(m && m.canUndo === true && m.undoDomain === "file", "撤销域随消息上报");
posted.length = 0;
FN("postHostStatus")("navigate", "不该发出", "", false, "", 0);
ok(posted.length === 0, "白名单外状态被前端拦掉");

console.log("\n[2] hostUndoOf 只认成功且声明 join 的工具");
ok(FN("hostUndoOf")({ status: "done", undoGroup: "join", undoDomain: "file", turnId: 3, label: "改名" }),
   "成功 + join + file 可撤销");
ok(!FN("hostUndoOf")({ status: "error", undoGroup: "join", undoDomain: "file" }),
   "失败工具不承诺可撤销");
ok(!FN("hostUndoOf")({ status: "done", undoGroup: "none", undoDomain: "file" }),
   "undoGroup=none 不承诺可撤销");
ok(!FN("hostUndoOf")({ status: "done", undoGroup: "join", undoDomain: "settings" }),
   "未知撤销域不承诺可撤销");

console.log("\n[3] done 事件把摘要和撤销能力一起送到宿主");
posted.length = 0;
S.generation = 1;
S.activeTurn = 7;
S.lastUndo = FN("hostUndoOf")({ status: "done", undoGroup: "join", undoDomain: "bookmark",
                                turnId: 7, label: "删除书签" });
const assistant = {
  _text: "",
  final: "",
  bubble: new FakeEl("bubble"),
  el: new FakeEl("assistant"),
  finish(text) { this.final = text; },
  scheduleRender() {},
  markFailed() {}
};
FN("handleEvent")({ type: "done", reply: "全部完成", turnId: 7, usage: null },
                  assistant, S.generation);
const done = posted.find((x) => x.type === "status" && x.state === "done");
ok(!!done, "done 事件产生宿主状态");
ok(done && done.summary === "全部完成", "摘要取回答首行");
ok(done && done.canUndo === true && done.undoDomain === "bookmark", "撤销能力来自本轮工具元数据");
ok(assistant.final === "全部完成", "回答气泡本身仍正常收尾");

console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
process.exit(fail ? 1 : 0);
